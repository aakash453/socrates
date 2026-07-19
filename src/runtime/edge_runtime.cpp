#include "socrates/runtime/edge_runtime.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "socrates/cluster/leader_election.h"
#include "socrates/cluster/membership_service.h"
#include "socrates/discovery/discovery_service.h"
#include "socrates/inference/inference_backend.h"
#include "socrates/logging.h"
#include "socrates/model/manifest_repository.h"
#include "socrates/model/model_manager.h"
#include "socrates/persistence/assignment_store.h"
#include "socrates/pipeline/inference_pipeline.h"
#include "socrates/profiler/capability_profiler.h"
#include "socrates/scheduler/scheduler.h"
#include "socrates/security/identity_provider.h"

#include "socrates/tracing.h"
#include "socrates/transport/transport.h"
#include "socrates/runtime/backend_dispatch.h"

namespace socrates::runtime {

class EdgeRuntimeImpl final : public EdgeRuntime {
 public:
  ~EdgeRuntimeImpl() override {
    heartbeat_active_ = false;
    if (heartbeat_worker_.joinable()) heartbeat_worker_.join();
  }

  Result<bool> start(const RuntimeConfig& config, RuntimeCallback callback,
                      CancellationToken cancellation) override {
    std::lock_guard lock(mutex_);
    if (state_ != RuntimeState::kStopped) {
      return Result<bool>::Err(ErrorCode::kFailedPrecondition, "already started");
    }

    auto log = make_log_context("runtime");

    state_ = RuntimeState::kStarting;
    config_ = config;
    callback_ = std::move(callback);
    cancellation_ = cancellation;
    local_role_ = config.start_as_leech ? cluster::NodeRole::kLeech
                                         : cluster::NodeRole::kParticipant;

    // 1. Identity
    identity_provider_ = security::make_ephemeral_identity_provider();
    auto id_result = identity_provider_->local_identity();
    if (id_result.is_err()) return Result<bool>::Err(id_result.error());
    local_identity_ = id_result.take_value();
    log.with_node_id(local_identity_.node_id).info("identity established");

    // 2. Create membership BEFORE starting discovery
    membership_ = cluster::make_membership_service();
    membership_->start(*identity_provider_,
        [this](const cluster::MembershipSnapshot& snap) {
          if (election_) {
            try {
              election_->update_membership(snap);
            } catch (...) { }
          }

          if (callback_) {
            RuntimeSnapshot rs;
            rs.state = state_;
            // NOTE: no lock here — start() already holds mutex_, and
            // re-acquiring it in the membership callback causes deadlock.
            rs.membership = snap;
            rs.leadership = election_ ? election_->current()
                                      : cluster::LeadershipState{};
            rs.active_plan = current_plan_;
            rs.local_models = local_models_;
            rs.local_role = local_role_;
            callback_(rs);
          }
        });
    log.info("membership started");

    // 3. Discovery
    discovery_ = discovery::make_coordinated_discovery();
    discovery::DiscoveryConfig dcfg;
    if (config_.skip_discovery) {
      dcfg.fallback_order = {discovery::DiscoveryMethod::kUdpBroadcast,
                             discovery::DiscoveryMethod::kBluetoothLowEnergy};
      log.info("mDNS skipped — using UDP broadcast + BLE");
    } else {
      dcfg.fallback_order = {discovery::DiscoveryMethod::kMdns,
                             discovery::DiscoveryMethod::kUdpBroadcast,
                             discovery::DiscoveryMethod::kBluetoothLowEnergy};
    }
    dcfg.service_name = "socrates-" + config_.cluster_id;
    dcfg.udp_port = 9876;
    dcfg.scan_timeout = std::chrono::milliseconds(1500);
    dcfg.announce_interval = std::chrono::milliseconds(3000);

    discovery::PeerAdvertisement local_peer;
    local_peer.node_id = local_identity_.node_id;
    local_peer.incarnation = 1;
    local_peer.public_key_fingerprint = local_identity_.public_key_fingerprint;

    try {
      discovery_->start(dcfg, local_peer,
          [this](const discovery::DiscoveryEvent& ev) {
            if (membership_) {
              try {
                membership_->observe(ev);
              } catch (...) { }
            }
          });
      log.info("discovery started");
    } catch (const RuntimeError& e) {
      return Result<bool>::Err(e.code, std::string(e.what()) +
                                          " [discovery.start]");
    }

    // 3b. Profiler
    profiler_ = profiler::make_capability_profiler();
    auto profile_result = profiler_->inspect(local_identity_.node_id, cancellation_);
    if (profile_result.is_ok()) {
      local_profile_ = profile_result.take_value();
      log.info("hardware profiled: " +
               std::to_string(local_profile_.total_memory_bytes / (1024 * 1024)) +
               " MB RAM, " + std::to_string(local_profile_.logical_cpu_count) +
               " CPU cores");
    } else {
      log.warn("profiling failed, using defaults");
    }

    if (local_profile_.node_id.empty()) {
      local_profile_.node_id = local_identity_.node_id;
    }

    // Register self in membership so scheduler sees the local node
    {
      discovery::DiscoveryEvent self_ev;
      self_ev.peer.node_id = local_identity_.node_id;
      self_ev.peer.incarnation = 1;
      self_ev.peer.public_key_fingerprint = local_identity_.public_key_fingerprint;
      try { membership_->observe(self_ev); } catch (...) { }
      try { membership_->promote_joining_members(); } catch (...) { }
      try { membership_->update_capability(local_profile_); } catch (...) { }
    }

    // 4. Election
    election_ = cluster::make_bully_election();
    struct InMemoryTermStore : cluster::ElectionTermStore {
      Result<std::uint64_t> load_term(const std::string&) const override {
        return Result<std::uint64_t>::Ok(0);
      }
      Result<bool> store_term(const std::string&, std::uint64_t) override {
        return true;
      }
    };
    static InMemoryTermStore term_store;

    election_->start(local_identity_.node_id, config_.cluster_id, term_store,
        [](const cluster::LeadershipState& ls) {
          auto log = make_log_context("runtime");
          if (ls.leader_id.has_value()) {
            log.with_node_id(*ls.leader_id)
                .with_term(ls.fence.term)
                .info("leader elected");

            // Compare directly with ls parameter, not is_local_leader()
            // which would call election_->current() — deadlock inside callback.
            // schedule_plan() is also NOT called here because it too calls
            // election_->current() while the election mutex is held.
            // The plan will be scheduled by join_cluster() after this returns.
          }
        });
    log.info("election started");

    // 5. Model & persistence
    manifest_repo_ = model::make_manifest_repository();
    model_manager_ = model::make_model_manager(config_.model_root);
    assignment_store_ = persistence::make_sqlite_assignment_store(
        (config_.state_directory / "assignments.db").string());

    // 6. Backend registry — populated with platform-optimized fallback chain
    try {
      backend_registry_ = inference::make_backend_registry();
      build_backend_fallback_chain(*backend_registry_);
      log.info("backends registered");
    } catch (const std::exception& e) {
      log.warn("backend init failed: " + std::string(e.what()));
      if (config_.production_mode) {
        log.warn("production mode: staying as leech — backends unavailable");
      }
    }

    // 7. Transport — TLS 1.3 binary protocol (requires protobuf)
#if SOCRATES_HAS_PROTOBUF
    transport_ = transport::make_grpc_transport();
    transport::TransportConfig tcfg;
    tcfg.listen_address = "0.0.0.0";
    tcfg.listen_port = 9876;
    tcfg.tls_certificate_pem = local_identity_.certificate_pem;
    tcfg.tls_private_key_pem = "";  // ephemeral: no client cert required
    tcfg.max_message_bytes = 64 * 1024 * 1024;
    tcfg.default_deadline = std::chrono::milliseconds(30000);
    try {
      transport_->start(tcfg);
      log.info("transport started on port 9876");
    } catch (const RuntimeError& e) {
      log.warn("transport start failed (simulated mode): " +
               std::string(e.what()));
      transport_.reset();
    }
#else
    log.info("transport disabled (protobuf not found) — distributed inference unavailable");
#endif

    // 8. Pipeline — wires transport + backends for distributed inference
    pipeline_ = pipeline::make_inference_pipeline(
            transport_.get(), backend_registry_.get(),
            model_manager_.get(), tracer_.get());

    pipeline_->set_local_node_id(local_identity_.node_id);
    pipeline_->set_production_mode(config_.production_mode);
    pipeline_->set_batching_config(config_.batching);

    // 7. Heartbeat
    heartbeat_active_ = true;
    heartbeat_worker_ = std::thread([this]() {
          while (heartbeat_active_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!heartbeat_active_) break;

            if (membership_ && running()) {
              try {
                auto snap = membership_->snapshot();
                auto now = std::chrono::system_clock::now();
                for (const auto& m : snap.members) {
                  auto age = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - m.last_seen)
                                 .count();
                  if (age > 10 && m.state == cluster::MemberState::kAlive) {
                    auto log = make_log_context("runtime");
                    log.with_node_id(m.peer.node_id)
                        .warn("member suspected, age=" + std::to_string(age) + "s");
                  }
                }
              } catch (...) { }
            }

            if (is_local_leader() && pipeline_ &&
                pipeline_->active_generation_count() == 0 && membership_) {
              try {
                membership_->promote_joining_members();
              } catch (...) { }
            }
          }
        });

    state_ = RuntimeState::kReady;
    log.info("runtime ready");
    emit_snapshot();
    return true;
  }

  Result<RuntimeSnapshot> snapshot() const override {
    std::lock_guard lock(mutex_);
    if (state_ == RuntimeState::kStopped) {
      return Result<RuntimeSnapshot>::Err(ErrorCode::kFailedPrecondition,
                                          "runtime stopped");
    }
    RuntimeSnapshot snap;
    snap.state = state_;
    if (membership_) snap.membership = membership_->snapshot();
    if (election_) snap.leadership = election_->current();
    snap.active_plan = current_plan_;
    snap.local_models = local_models_;
    snap.local_role = local_role_;
    return snap;
  }

  Result<std::unique_ptr<pipeline::GenerationHandle>> generate(
      const pipeline::InferenceRequest& request,
      pipeline::StreamCallback callback,
      CancellationToken cancellation) override {
    std::lock_guard lock(mutex_);
    if (state_ != RuntimeState::kReady && state_ != RuntimeState::kDegraded) {
      return Result<std::unique_ptr<pipeline::GenerationHandle>>::Err(
          ErrorCode::kFailedPrecondition, "runtime not ready");
    }
    if (!current_plan_.has_value()) {
      return Result<std::unique_ptr<pipeline::GenerationHandle>>::Err(
          ErrorCode::kFailedPrecondition, "no active plan");
    }
    return pipeline_->generate(request, current_plan_.value(),
                                std::move(callback), cancellation);
  }

  // ── join_cluster: leech → participant ──────────────────────────────────

  Result<bool> join_cluster(CancellationToken cancellation) override {
    auto log = make_log_context("runtime");
    if (cancellation.stop_requested()) {
      return Result<bool>::Err(ErrorCode::kCancelled, "join cancelled");
    }

    // In production mode, refuse to participate without transport + backends
    if (config_.production_mode && !transport_) {
      return Result<bool>::Err(
          ErrorCode::kUnavailable,
          "cannot join cluster: transport not available — "
          "install protobuf/grpc or disable production_mode");
    }
    if (config_.production_mode && !backend_registry_) {
      return Result<bool>::Err(
          ErrorCode::kUnavailable,
          "cannot join cluster: backend registry not available");
    }

    // Simulate downloading model shards for the current cluster
    std::vector<cluster::ModelPresence> models;
    {
      cluster::ModelPresence mp;
      mp.model_id = ModelId{"qwen3-1.8b"};
      mp.manifest_revision = "1.0";
      mp.quantization = "int4";
      mp.total_bytes = 1500000000;
      mp.downloaded_bytes = 1500000000;
      mp.ready = true;
      models.push_back(mp);
    }
    {
      cluster::ModelPresence mp;
      mp.model_id = ModelId{"qwen3-4b"};
      mp.manifest_revision = "1.0";
      mp.quantization = "int4";
      mp.total_bytes = 2800000000;
      mp.downloaded_bytes = 2800000000;
      mp.ready = true;
      models.push_back(mp);
    }

    if (membership_) {
      membership_->update_model_presence(models);
    }

    local_models_ = models;
    local_role_ = cluster::NodeRole::kParticipant;
    log.info("joined cluster as participant, " +
             std::to_string(models.size()) + " models ready");

    if (is_local_leader()) {
      schedule_plan();
    }

    emit_snapshot();
    return true;
  }

  std::vector<cluster::ModelPresence> model_download_progress() const override {
    std::lock_guard lock(mutex_);
    return local_models_;
  }

  std::vector<ModelCatalogEntry> available_models() const override {
    if (!membership_) return {};
    return runtime::available_models(membership_->snapshot());
  }

  Result<ClusterProfile> run_profiler(CancellationToken cancellation) override {
    auto log = make_log_context("runtime");
    if (cancellation.stop_requested()) {
      return Result<ClusterProfile>::Err(ErrorCode::kCancelled, "profiler cancelled");
    }
    // Simulated profiler — emits per-stage metrics for the debug model
    ClusterProfile profile;
    profile.cluster_config_name = "current";
    profile.profiler_model_id = ModelId{"socrates-debug-profiler"};
    profile.all_stages_healthy = true;

    if (membership_) {
      auto snap = membership_->snapshot();
      for (const auto& m : snap.members) {
        if (m.state != cluster::MemberState::kAlive) continue;
        StageProfile sp;
        sp.stage_id = "stage-" + m.peer.node_id.value;
        sp.node_id = m.peer.node_id;
        sp.backend = BackendKind::kLlamaCpp;
        sp.gpu_miss_rate = 0.05;   // 5% GPU miss
        sp.npu_miss_rate = 0.0;
        sp.cpu_fallback_rate = 0.02;
        sp.avg_latency = std::chrono::microseconds(12000);
        sp.p99_latency = std::chrono::microseconds(35000);
        sp.kv_cache_hit_count = 150;
        sp.kv_cache_miss_count = 10;
        sp.network_transfer_bytes = 16384;
        sp.peak_memory_bytes = 512 * 1024 * 1024;
        profile.stages.push_back(sp);
      }
      profile.total_tokens_per_second = 100.0 / (static_cast<double>(profile.stages.size()) * 0.012);
      profile.end_to_end_latency = std::chrono::microseconds(12000 * profile.stages.size());
    }

    log.info("profiler complete: " + std::to_string(profile.stages.size()) + " stages, " +
             std::to_string(static_cast<int>(profile.total_tokens_per_second)) + " tok/s");
    return profile;
  }

  void stop() noexcept override {
    do_stop(false);
  }

  void leave() noexcept override {
    do_stop(true);
  }

 private:
  bool running() const {
    return state_ == RuntimeState::kReady ||
           state_ == RuntimeState::kDegraded ||
           state_ == RuntimeState::kDiscovering ||
           state_ == RuntimeState::kElecting;
  }

  bool is_local_leader() const {
      if (!election_) return false;
      auto ls = election_->current();
      return ls.leader_id.has_value() &&
             ls.leader_id->value == local_identity_.node_id.value;
    }

    void do_stop(bool graceful) {
      auto log = make_log_context("runtime");
      {
        std::lock_guard lock(mutex_);
        if (state_ == RuntimeState::kStopped) return;
        if (state_ == RuntimeState::kStopping) return;
        state_ = RuntimeState::kStopping;
      }

      if (graceful) {
        if (membership_) {
          try { membership_->stop(); } catch (...) {}
        }
        if (election_) {
          try { election_->stop(); } catch (...) {}
        }

        if (pipeline_) {
          auto drain_start = Clock::now();
          auto drain_timeout = std::chrono::seconds(30);
          log.info("graceful leave: draining active generations...");
          while (pipeline_->active_generation_count() > 0) {
            if (Clock::now() - drain_start > drain_timeout) {
              log.warn("drain timeout reached, forcing stop");
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          log.info("drain complete, shutting down");
        }
      }

      heartbeat_active_ = false;
      if (heartbeat_worker_.joinable()) heartbeat_worker_.join();

      if (pipeline_) pipeline_.reset();
      if (transport_) { try { transport_->stop(); } catch (...) {} transport_.reset(); }
      if (election_) { try { election_->stop(); } catch (...) {} election_.reset(); }
      if (membership_) { try { membership_->stop(); } catch (...) {} membership_.reset(); }
      if (discovery_) { discovery_->stop(); discovery_.reset(); }

      {
        std::lock_guard lock(mutex_);
        state_ = RuntimeState::kStopped;
      }
      emit_snapshot();
      log.info("runtime stopped");
    }

    void schedule_plan() {
        if (!scheduler_ || !membership_ || !election_) return;

        auto log = make_log_context("runtime");
        auto snap = membership_->snapshot();
        auto ls = election_->current();

        scheduler::SchedulingInput input;
        input.membership = snap;
        input.fence = ls.fence;

        auto manifest_path = config_.model_root / "manifest.json";
        std::error_code ec;
        if (std::filesystem::exists(manifest_path, ec)) {
          auto result = manifest_repo_->publish(manifest_path);
          if (result.is_ok()) {
            auto summary = result.take_value();
            input.model_id = summary.model_id;
            input.manifest_revision = summary.revision;
            input.total_layers = summary.total_layers;
            input.shard_options = std::move(summary.shard_options);
            input.boundaries = std::move(summary.boundaries);
            log.info("loaded manifest: " + summary.model_id.value +
                     " (" + std::to_string(summary.total_layers) + " layers, " +
                     std::to_string(input.shard_options.size()) + " shards)");
          } else {
            log.warn("manifest load failed: " +
                     std::string(result.error().what()));
          }
        }

        if (input.shard_options.empty()) {
          log.info("no manifest found, auto-generating plan from " +
                   std::to_string(snap.members.size()) + " peers");
          input.model_id = ModelId{"auto-generated"};
          input.manifest_revision = "v1";
          input.total_layers = 32;

          for (const auto& m : snap.members) {
            if (m.state != cluster::MemberState::kAlive) continue;
            if (!m.capability.has_value()) continue;
            scheduler::ShardOption opt;
            opt.shard_id = ShardId{"shard-" + m.peer.node_id.value};
            opt.stage_ids = {"layer-" + m.peer.node_id.value};
            opt.stage_kind = scheduler::StageKind::kTransformerLayers;
            opt.quantization = QuantizationKind::kFp16;
            opt.peak_runtime_memory_bytes =
                m.capability->available_memory_bytes / 2;
            opt.compatible_backends = {BackendKind::kLlamaCpp};
            opt.required_compute_units = {ComputeUnit::kCpu};
            opt.estimated_decode_microseconds = 40000.0;
            input.shard_options.push_back(std::move(opt));
          }

          std::uint32_t layers_per = input.total_layers /
              std::max<std::size_t>(input.shard_options.size(), 1);
          std::uint32_t offset = 0;
          for (auto& opt : input.shard_options) {
            opt.layers = LayerRange{offset, offset + layers_per};
            offset += layers_per;
          }
          if (!input.shard_options.empty() && offset < input.total_layers) {
            input.shard_options.back().layers =
                LayerRange{input.shard_options.back().layers->start,
                           input.total_layers};
          }
        }

        auto plan_result = scheduler_->create_plan(input);
        if (plan_result.is_ok()) {
          current_plan_ = plan_result.take_value();
          if (current_plan_->model_id.value != "auto-generated") {
            assignment_store_->save_plan(current_plan_.value());
          }
          log.with_plan_id(PlanId{current_plan_->plan_id})
              .info("plan scheduled, " +
                    std::to_string(current_plan_->stages.size()) + " stages");
        } else {
          log.warn("scheduling failed: " +
                   std::string(plan_result.error().what()));
        }
      }

    void emit_snapshot() {
      if (!callback_) return;
      RuntimeSnapshot snap;
      snap.state = state_;
      if (membership_) snap.membership = membership_->snapshot();
      if (election_) snap.leadership = election_->current();
      snap.active_plan = current_plan_;
      snap.local_models = local_models_;
      snap.local_role = local_role_;
      callback_(snap);
    }

  mutable std::mutex mutex_;
  RuntimeConfig config_;
  RuntimeState state_{RuntimeState::kStopped};
  RuntimeCallback callback_;
  CancellationToken cancellation_;
  security::LocalIdentity local_identity_;
  CapabilityProfile local_profile_;
  std::vector<cluster::ModelPresence> local_models_;
  cluster::NodeRole local_role_{cluster::NodeRole::kLeech};
  std::unique_ptr<security::IdentityProvider> identity_provider_;
  std::unique_ptr<discovery::DiscoveryService> discovery_;
  std::unique_ptr<cluster::MembershipService> membership_;
  std::unique_ptr<cluster::LeaderElection> election_;
  std::unique_ptr<profiler::CapabilityProfiler> profiler_;
  std::unique_ptr<scheduler::MemoryScheduler> scheduler_{
      scheduler::make_memory_scheduler()};
  std::unique_ptr<model::ManifestRepository> manifest_repo_;
  std::unique_ptr<model::ModelManager> model_manager_;
  std::unique_ptr<persistence::AssignmentStore> assignment_store_;
  std::unique_ptr<pipeline::InferencePipeline> pipeline_;
    std::unique_ptr<transport::Transport> transport_;
    std::unique_ptr<inference::BackendRegistry> backend_registry_;
    std::unique_ptr<TraceRecorder> tracer_;

    std::optional<scheduler::PipelinePlan> current_plan_;

  std::atomic<bool> heartbeat_active_{false};
  std::thread heartbeat_worker_;
};

std::unique_ptr<EdgeRuntime> make_edge_runtime() {
  return std::make_unique<EdgeRuntimeImpl>();
}

}  // namespace socrates::runtime
