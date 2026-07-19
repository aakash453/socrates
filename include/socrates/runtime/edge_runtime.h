#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include "socrates/cancellation.h"
#include <string>
#include <vector>

#include "socrates/cluster/leader_election.h"
#include "socrates/cluster/membership_service.h"
#include "socrates/pipeline/inference_pipeline.h"
#include "socrates/result.h"
#include "socrates/scheduler/scheduler.h"
#include "socrates/types.h"
#include "socrates/runtime/model_catalog.h"

namespace socrates::runtime {

using RuntimeState = socrates::RuntimeState;

enum class TrustMode { kEphemeralLocalCluster, kPinnedAllowlist, kPrivateCertificateAuthority };

struct IdentityConfig {
  TrustMode trust_mode{TrustMode::kEphemeralLocalCluster};
  std::filesystem::path certificate;
  std::filesystem::path private_key;
  std::filesystem::path trust_bundle;
  std::vector<std::string> allowed_peer_fingerprints;
  bool auto_generate_local_identity{true};
};

struct RuntimeConfig {
  NodeId local_node;
  std::string cluster_id;
  std::filesystem::path state_directory;
  std::filesystem::path model_root;
  IdentityConfig identity;
  std::uint32_t recovery_token_retention{0};
  bool tracing_enabled{false};
  /// When true, simulated inference is disabled — nodes without
  /// transport+backends stay as leech and cannot participate.
  bool production_mode{false};
  bool skip_discovery{false};
  pipeline::BatchingConfig batching;
  /// Start as leech (default: true). Set false for dedicated inference nodes.
  bool start_as_leech{true};
};

struct RuntimeSnapshot {
  RuntimeState state{RuntimeState::kStopped};
  cluster::MembershipSnapshot membership;
  cluster::LeadershipState leadership;
  std::optional<scheduler::PipelinePlan> active_plan;
  std::vector<cluster::ModelPresence> local_models;
  cluster::NodeRole local_role{cluster::NodeRole::kLeech};
};

using RuntimeCallback = std::function<void(const RuntimeSnapshot&)>;

class EdgeRuntime {
 public:
  virtual ~EdgeRuntime() = default;

  virtual Result<bool> start(const RuntimeConfig& config,
                             RuntimeCallback callback,
                             CancellationToken cancellation) = 0;

  [[nodiscard]] virtual Result<RuntimeSnapshot> snapshot() const = 0;

  virtual Result<std::unique_ptr<pipeline::GenerationHandle>> generate(
      const pipeline::InferenceRequest& request,
      pipeline::StreamCallback callback,
      CancellationToken cancellation) = 0;

  virtual void stop() noexcept = 0;

  /// Leech → participant: download required model shards and opt into inference.
  /// Preconditions: runtime started, node is currently a leech.
  /// Postconditions: models downloaded, role transitions to kParticipant,
  ///   scheduler can assign stages to this node.
  virtual Result<bool> join_cluster(CancellationToken cancellation) = 0;

  /// Returns current model download progress for this node.
  [[nodiscard]] virtual std::vector<cluster::ModelPresence> model_download_progress() const = 0;

  [[nodiscard]] virtual std::vector<ModelCatalogEntry> available_models() const = 0;

  virtual Result<ClusterProfile> run_profiler(CancellationToken cancellation) = 0;

  virtual void leave() noexcept = 0;
};

std::unique_ptr<EdgeRuntime> make_edge_runtime();

}  // namespace socrates::runtime
