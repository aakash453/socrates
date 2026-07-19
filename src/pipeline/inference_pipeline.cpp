#include "socrates/pipeline/inference_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if __has_include(<llama.h>)
#include <llama.h>
#endif

#include "socrates/model/model_manager.h"
#include "socrates/persistence/assignment_store.h"
#include "socrates/transport/tensor_codec.h"
#include "socrates/transport/transport.h"

namespace socrates::pipeline {

// ── Internal state ───────────────────────────────────────────────────────

struct ActiveRequest {
  InferenceRequest request;
  scheduler::PipelinePlan plan;
  StreamCallback callback;
  std::atomic<bool> active{true};
  std::uint64_t tokens_emitted{0};
  std::uint64_t next_expected_position{0};
  std::vector<persistence::TokenRecord> token_history;
  std::mutex callback_mutex;
};

class GenerationHandleImpl final : public GenerationHandle {
 public:
  explicit GenerationHandleImpl(std::shared_ptr<ActiveRequest> state,
                                std::shared_ptr<CancellationToken> token)
      : state_(std::move(state)), cancel_token_(std::move(token)) {}
  void cancel() noexcept override {
    if (cancel_token_) cancel_token_->request_stop();
    state_->active = false;
  }

 private:
  std::shared_ptr<ActiveRequest> state_;
  std::shared_ptr<CancellationToken> cancel_token_;
};

// ── Queue entry ──────────────────────────────────────────────────────────

struct QueuedRequest {
  std::shared_ptr<ActiveRequest> state;
  std::shared_ptr<CancellationToken> cancel_token;
  std::unique_ptr<GenerationHandleImpl> handle;
  std::uint32_t context_window;

  bool is_long_context(const BatchingConfig& cfg) const {
    return context_window > cfg.long_context_threshold;
  }
};

// ── Stage session cache entry ────────────────────────────────────────────

struct LoadedStage {
  std::string stage_id;
  BackendKind backend;
  std::unique_ptr<inference::InferenceSession> session;
};

// ── Pipeline implementation ──────────────────────────────────────────────

class InferencePipelineImpl final : public InferencePipeline {
 public:
  InferencePipelineImpl(transport::Transport* transport,
                         inference::BackendRegistry* backends,
                         model::ModelManager* models,
                         TraceRecorder* tracer)
      : transport_(transport), backends_(backends), models_(models),
        codec_(std::make_unique<transport::TensorCodec>()) { (void)tracer;
    start_dispatcher();
  }

  ~InferencePipelineImpl() override {
    stop_dispatcher();
    {
      std::lock_guard lock(mutex_);
      for (auto& [id, state] : active_requests_) {
        std::lock_guard cb_lock(state->callback_mutex);
        state->callback = nullptr;
        state->active = false;
      }
      std::lock_guard qlock(queue_mutex_);
      for (auto& qr : pending_queue_) {
        std::lock_guard cb_lock(qr.state->callback_mutex);
        qr.state->callback = nullptr;
        qr.state->active = false;
      }
      pending_queue_.clear();
    }
    std::vector<std::thread> threads_to_join;
    {
      std::lock_guard wlock(workers_mutex_);
      for (auto& [id, t] : workers_) {
        threads_to_join.push_back(std::move(t));
      }
      workers_.clear();
    }
    for (auto& t : threads_to_join) {
      if (t.joinable()) t.join();
    }
  }

  void set_production_mode(bool enabled) override {
    production_mode_ = enabled;
  }

  void set_batching_config(const BatchingConfig& config) override {
    std::lock_guard lock(queue_mutex_);
    batching_config_ = config;
    queue_cv_.notify_one();
  }

  void set_local_node_id(NodeId id) override {
    std::lock_guard lock(mutex_);
    local_node_id_ = std::move(id);
  }

  // ── generate ──────────────────────────────────────────────────────────

  Result<std::unique_ptr<GenerationHandle>> generate(
      const InferenceRequest& request, const scheduler::PipelinePlan& plan,
      StreamCallback callback, CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<std::unique_ptr<GenerationHandle>>::Err(
          ErrorCode::kCancelled, "generation cancelled");
    }
    if (plan.stages.empty()) {
      return Result<std::unique_ptr<GenerationHandle>>::Err(
          ErrorCode::kInvalidArgument, "plan has no stages");
    }

    // In production mode, refuse to run without transport + backends
    if (production_mode_ && !has_distributed_capability()) {
      return Result<std::unique_ptr<GenerationHandle>>::Err(
          ErrorCode::kUnavailable,
          "production mode: transport and backends required — "
          "node cannot participate in inference");
    }

    auto state = std::make_shared<ActiveRequest>();
    state->request = request;
    state->plan = plan;
    state->callback = std::move(callback);
    state->active = true;
    state->next_expected_position = 0;

    auto shared_cancel = std::make_shared<CancellationToken>(cancellation);
    auto handle = std::make_unique<GenerationHandleImpl>(state, shared_cancel);

    {
      std::lock_guard lock(mutex_);
      active_requests_[request.request_id.value] = state;
    }

    BatchingMode effective_mode;
    {
      std::lock_guard lock(queue_mutex_);
      effective_mode = batching_config_.mode;
    }

    if (effective_mode == BatchingMode::kImmediate) {
      emit_started(state);
      launch_worker(state, shared_cancel);
      return std::unique_ptr<GenerationHandle>(std::move(handle));
    }

    emit_queued(state);

    {
      std::lock_guard lock(queue_mutex_);
      pending_queue_.push_back(QueuedRequest{
          state, shared_cancel, nullptr,
          request.generation.context_window});
      pending_queue_.back().handle = std::move(handle);
    }
    queue_cv_.notify_one();

    QueuedRequest* qr;
    {
      std::lock_guard lock(queue_mutex_);
      qr = &pending_queue_.back();
    }
    return std::unique_ptr<GenerationHandle>(
        new QueuedGenerationHandle(this, qr->state, qr->cancel_token));
  }

  Result<bool> replan(const RequestId& request_id,
                       const scheduler::PipelinePlan& replacement,
                       CancellationToken /*cancellation*/) override {
    std::lock_guard lock(mutex_);
    auto it = active_requests_.find(request_id.value);
    if (it == active_requests_.end()) {
      return Result<bool>::Err(ErrorCode::kNotFound, "request not found");
    }

    auto& state = it->second;
    if (replacement.fence.term <= state->plan.fence.term) {
      return Result<bool>::Err(ErrorCode::kFailedPrecondition,
                               "replacement plan has stale fence");
    }

    state->active = false;

    {
      StreamEvent ev;
      ev.kind = StreamEventKind::kReplanning;
      ev.request_id = request_id;
      ev.sequence = state->tokens_emitted;
      ev.metadata["replacement_plan_id"] = replacement.plan_id;
      std::lock_guard cb_lock(state->callback_mutex);
      if (state->callback) state->callback(ev);
    }

    state->plan = replacement;
    state->active = true;

    for (std::uint64_t i = 0;
         i < state->token_history.size() && state->active; ++i) {
      StreamEvent replay_ev;
      replay_ev.kind = StreamEventKind::kToken;
      replay_ev.request_id = request_id;
      replay_ev.sequence = i;
      replay_ev.text_delta = "replay:" + std::to_string(i);
      replay_ev.token_id = state->token_history[i].token_id;
      std::lock_guard cb_lock(state->callback_mutex);
      if (state->callback) state->callback(replay_ev);
    }

    {
      StreamEvent done_ev;
      done_ev.kind = StreamEventKind::kCompleted;
      done_ev.request_id = request_id;
      done_ev.sequence = state->tokens_emitted;
      std::lock_guard cb_lock(state->callback_mutex);
      if (state->callback) state->callback(done_ev);
    }

    return true;
  }

  std::size_t active_generation_count() const override {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [id, state] : active_requests_) {
      (void)id;
      if (state->active && state->tokens_emitted > 0) ++count;
    }
    std::lock_guard wlock(workers_mutex_);
    for (const auto& [id, t] : workers_) {
      (void)t;
      if (active_requests_.count(id)) ++count;
    }
    return count;
  }

  std::size_t queued_request_count() const override {
    std::lock_guard lock(queue_mutex_);
    return pending_queue_.size();
  }

  void detect_node_loss(NodeId lost_node) {
    std::lock_guard lock(mutex_);
    for (auto& [id, state] : active_requests_) {
      bool affected = false;
      for (const auto& stage : state->plan.stages) {
        if (stage.node_id == lost_node) { affected = true; break; }
      }
      if (affected) {
        StreamEvent ev;
        ev.kind = StreamEventKind::kReplanning;
        ev.request_id = state->request.request_id;
        ev.metadata["lost_node"] = lost_node.value;
        std::lock_guard cb_lock(state->callback_mutex);
        if (state->callback) state->callback(ev);
        state->active = false;
      }
    }
  }

 private:
  // ── Queued-generation handle ──────────────────────────────────────────

  class QueuedGenerationHandle final : public GenerationHandle {
   public:
    QueuedGenerationHandle(InferencePipelineImpl* pipeline,
                           std::shared_ptr<ActiveRequest> state,
                           std::shared_ptr<CancellationToken> token)
        : pipeline_(pipeline), state_(std::move(state)),
          cancel_token_(std::move(token)) {}

    void cancel() noexcept override {
      if (cancel_token_) cancel_token_->request_stop();
      state_->active = false;
      if (pipeline_) pipeline_->remove_queued(state_->request.request_id);
    }

   private:
    InferencePipelineImpl* pipeline_;
    std::shared_ptr<ActiveRequest> state_;
    std::shared_ptr<CancellationToken> cancel_token_;
  };

  void remove_queued(const RequestId& request_id) {
    std::lock_guard lock(queue_mutex_);
    auto it = std::find_if(pending_queue_.begin(), pending_queue_.end(),
        [&](const QueuedRequest& qr) {
          return qr.state->request.request_id.value == request_id.value;
        });
    if (it != pending_queue_.end()) {
      StreamEvent ev;
      ev.kind = StreamEventKind::kFailed;
      ev.request_id = it->state->request.request_id;
      ev.error = RuntimeError{ErrorCode::kCancelled, "cancelled while queued"};
      std::lock_guard cb_lock(it->state->callback_mutex);
      if (it->state->callback) it->state->callback(ev);
      pending_queue_.erase(it);
    }
  }

  // ── Event emission ────────────────────────────────────────────────────

  void emit_queued(std::shared_ptr<ActiveRequest> state) {
    StreamEvent ev;
    ev.kind = StreamEventKind::kQueued;
    ev.request_id = state->request.request_id;
    ev.sequence = 0;
    std::lock_guard cb_lock(state->callback_mutex);
    if (state->callback) state->callback(ev);
  }

  void emit_started(std::shared_ptr<ActiveRequest> state) {
    StreamEvent ev;
    ev.kind = StreamEventKind::kStarted;
    ev.request_id = state->request.request_id;
    ev.sequence = 0;
    std::lock_guard cb_lock(state->callback_mutex);
    if (state->callback) state->callback(ev);
  }

  void emit_token(std::shared_ptr<ActiveRequest> state, std::uint64_t seq,
                  std::int32_t token_id, const std::string& text) {
    StreamEvent ev;
    ev.kind = StreamEventKind::kToken;
    ev.request_id = state->request.request_id;
    ev.sequence = seq;
    ev.token_id = token_id;
    ev.text_delta = text;
    std::lock_guard cb_lock(state->callback_mutex);
    if (state->callback) state->callback(ev);
  }

  void emit_terminal(std::shared_ptr<ActiveRequest> state, bool success) {
    StreamEvent ev;
    ev.kind = success ? StreamEventKind::kCompleted : StreamEventKind::kFailed;
    ev.request_id = state->request.request_id;
    ev.sequence = state->tokens_emitted;
    std::lock_guard cb_lock(state->callback_mutex);
    if (state->callback) state->callback(ev);
  }

  // ── Worker launch ─────────────────────────────────────────────────────

  void launch_worker(std::shared_ptr<ActiveRequest> state,
                     std::shared_ptr<CancellationToken> cancel_token) {
    std::string req_id = state->request.request_id.value;
    std::thread worker([this, state, cancel_token, req_id]() {
      run_generation(state, cancel_token);
      std::lock_guard wlock(workers_mutex_);
      auto it = workers_.find(req_id);
      if (it != workers_.end()) {
        it->second.detach();
        workers_.erase(it);
      }
    });

    {
      std::lock_guard wlock(workers_mutex_);
      workers_[req_id] = std::move(worker);
    }
  }

  // ── Dispatcher ────────────────────────────────────────────────────────

  void start_dispatcher() {
    dispatcher_running_ = true;
    dispatcher_thread_ = std::thread(&InferencePipelineImpl::dispatcher_loop,
                                     this);
  }

  void stop_dispatcher() {
    {
      std::lock_guard lock(queue_mutex_);
      dispatcher_running_ = false;
    }
    queue_cv_.notify_all();
    if (dispatcher_thread_.joinable()) dispatcher_thread_.join();
  }

  void dispatcher_loop() {
    while (true) {
      std::vector<QueuedRequest> batch;
      BatchingMode mode;
      BatchingConfig cfg;

      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this] {
          return !pending_queue_.empty() || !dispatcher_running_;
        });

        if (!dispatcher_running_) break;

        mode = batching_config_.mode;
        cfg = batching_config_;

        if (mode == BatchingMode::kImmediate) continue;

        if (mode == BatchingMode::kSerial) {
          QueuedRequest qr = std::move(pending_queue_.front());
          pending_queue_.pop_front();
          lock.unlock();
          dispatch_solo(std::move(qr));
          continue;
        }

        auto& first = pending_queue_.front();
        if (first.is_long_context(cfg)) {
          QueuedRequest qr = std::move(pending_queue_.front());
          pending_queue_.pop_front();
          lock.unlock();
          dispatch_solo(std::move(qr));
          continue;
        }

        auto deadline = Clock::now() + cfg.batch_timeout;
        while (!pending_queue_.empty() && batch.size() < cfg.max_batch_size) {
          auto& next = pending_queue_.front();
          if (next.is_long_context(cfg)) break;
          if (!next.state->active) { pending_queue_.pop_front(); continue; }
          batch.push_back(std::move(pending_queue_.front()));
          pending_queue_.pop_front();
          if (batch.size() >= cfg.max_batch_size) break;
        }

        if (batch.empty()) continue;

        if (batch.size() < cfg.max_batch_size) {
          queue_cv_.wait_until(lock, deadline, [this, &cfg] {
            return !pending_queue_.empty() &&
                   !pending_queue_.front().is_long_context(cfg);
          });
          while (!pending_queue_.empty() && batch.size() < cfg.max_batch_size) {
            auto& next = pending_queue_.front();
            if (next.is_long_context(cfg)) break;
            if (!next.state->active) { pending_queue_.pop_front(); continue; }
            batch.push_back(std::move(pending_queue_.front()));
            pending_queue_.pop_front();
          }
        }
      }

      if (!batch.empty()) dispatch_batch(std::move(batch));
    }
  }

  void dispatch_solo(QueuedRequest qr) {
    if (!qr.state->active) return;
    emit_started(qr.state);
    launch_worker(qr.state, qr.cancel_token);
  }

  void dispatch_batch(std::vector<QueuedRequest> batch) {
    for (auto& qr : batch) {
      if (!qr.state->active) continue;
      emit_started(qr.state);
      launch_worker(qr.state, qr.cancel_token);
    }
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  PRODUCTION: distributed inference via transport + backends
  // ═══════════════════════════════════════════════════════════════════════

  bool has_distributed_capability() const {
    return transport_ != nullptr && backends_ != nullptr;
  }

  /// Determine if a stage is assigned to the local node.
  bool is_local_stage(const scheduler::StageAssignment& stage) const {
    return stage.node_id.value == local_node_id_.value;
  }

  /// Get or create a loaded session for a local stage.
  Result<inference::InferenceSession*> ensure_session_loaded(
      const scheduler::StageAssignment& stage,
      CancellationToken& cancel) {
    std::lock_guard lock(sessions_mutex_);

    // Check cache
    for (auto& ls : loaded_sessions_) {
      if (ls.stage_id == stage.stage_ids[0] && ls.backend == stage.backend) {
        return ls.session.get();
      }
    }

    // Resolve backend
    if (!backends_) {
      return Result<inference::InferenceSession*>::Err(
          ErrorCode::kUnavailable, "no backend registry");
    }

    auto backend_result = backends_->resolve(stage.backend);
    if (backend_result.is_err()) {
      return Result<inference::InferenceSession*>::Err(backend_result.error());
    }
    auto backend = backend_result.value();

    // Acquire model shard
    model::ShardLease* lease = nullptr;
    std::unique_ptr<model::ShardLease> lease_owner;
    if (models_) {
      auto lease_result = models_->acquire(stage, cancel.stop_requested());
      if (lease_result.is_err()) {
        return Result<inference::InferenceSession*>::Err(lease_result.error());
      }
      lease_owner = std::move(lease_result.value());
      lease = lease_owner.get();
    }

    // Load model
    inference::LoadOptions opts;
    opts.quantization = stage.quantization;
    opts.stage_ids = stage.stage_ids;
    opts.layers = stage.layers;
    opts.execution_profile_id = stage.execution_profile_id;
    opts.memory_budget_bytes = stage.reserved_memory_bytes;

    std::unique_ptr<inference::InferenceSession> session;
    if (lease) {
      auto load_result = backend->load_model(*lease, opts, cancel);
      if (load_result.is_err()) {
        return Result<inference::InferenceSession*>::Err(load_result.error());
      }
      session = std::move(load_result.value());
    } else {
      // Simulated load when no model manager
    }

    LoadedStage ls;
    ls.stage_id = stage.stage_ids[0];
    ls.backend = stage.backend;
    ls.session = std::move(session);
    loaded_sessions_.push_back(std::move(ls));

    return loaded_sessions_.back().session.get();
  }

  /// Execute a single pipeline stage locally.
  Result<inference::RunResult> execute_local_stage(
      const scheduler::StageAssignment& stage,
      const inference::RunRequest& run_req,
      CancellationToken& cancel) {

    auto session_result = ensure_session_loaded(stage, cancel);
    if (session_result.is_err()) {
      return Result<inference::RunResult>::Err(session_result.error());
    }

    auto* session = session_result.value();
    if (!session) {
      // No session (simulated mode) — return a fake result
      inference::RunResult fake;
      fake.elapsed = std::chrono::microseconds(500);
      return fake;
    }

    return session->run_layers(run_req, cancel);
  }

  /// Execute a pipeline stage on a remote node via transport.
  Result<inference::RunResult> execute_remote_stage(
      const scheduler::StageAssignment& stage,
      const inference::RunRequest& run_req,
      CancellationToken& cancel) {
    (void)cancel;

    if (!transport_) {
      return Result<inference::RunResult>::Err(
          ErrorCode::kUnavailable, "no transport configured");
    }

    // Encode the input tensors for wire transfer
    if (run_req.inputs.empty()) {
      // First stage or no inputs — remote node handles prompt ingestion
      // Send a minimal request with just metadata
      Bytes empty;
      transport::UnaryRequest ureq;
      ureq.method = "RunLayers";
      ureq.payload = empty;
      ureq.target_node_id = stage.node_id.value;
      ureq.deadline = run_req.deadline;

      // Build target address from node_id (in production this comes from discovery)
      std::string target = stage.node_id.value + ":9876";

      auto resp = transport_->send_unary(target, ureq);
      if (resp.is_err()) {
        return Result<inference::RunResult>::Err(resp.error());
      }

      // Decode response
      auto decoded = codec_->decode(resp.value().payload);
      if (decoded.is_err()) {
        return Result<inference::RunResult>::Err(decoded.error());
      }

      inference::RunResult result;
      result.outputs.push_back({"hidden_states", std::move(decoded.value())});
      result.elapsed = resp.value().elapsed;
      return result;
    }

    // Encode the first input tensor (hidden states from previous stage)
    auto encoded = codec_->encode(run_req.inputs[0].value);
    if (encoded.is_err()) {
      return Result<inference::RunResult>::Err(encoded.error());
    }

    transport::UnaryRequest ureq;
    ureq.method = "RunLayers";
    ureq.payload = std::move(encoded.value());
    ureq.target_node_id = stage.node_id.value;
    ureq.deadline = run_req.deadline;

    std::string target = stage.node_id.value + ":9876";

    auto resp = transport_->send_unary(target, ureq);
    if (resp.is_err()) {
      return Result<inference::RunResult>::Err(resp.error());
    }

    auto decoded = codec_->decode(resp.value().payload);
    if (decoded.is_err()) {
      return Result<inference::RunResult>::Err(decoded.error());
    }

    inference::RunResult result;
    result.outputs.push_back({"hidden_states", std::move(decoded.value())});
    result.elapsed = resp.value().elapsed;
    return result;
  }

  /// Execute one full pipeline pass (all stages) for one token position.
  Result<inference::RunResult> execute_pipeline_pass(
      std::shared_ptr<ActiveRequest> state,
      std::uint64_t token_position,
      std::vector<inference::NamedTensor> stage_inputs,
      std::shared_ptr<CancellationToken> cancel_token) {

    const auto& plan = state->plan; (void)plan;
    const auto& request = state->request;
    std::vector<inference::NamedTensor> current_inputs = std::move(stage_inputs);

    for (const auto& stage : plan.stages) {
      if (cancel_token->stop_requested() || !state->active) {
        return Result<inference::RunResult>::Err(
            ErrorCode::kCancelled, "cancelled mid-stage");
      }

      inference::RunRequest run_req;
      run_req.request_id = request.request_id;
      run_req.session_id = request.session_id;
      run_req.stage_id = stage.stage_ids.empty() ? "" : stage.stage_ids[0];
      run_req.inputs = current_inputs;
      run_req.layers = stage.layers;
      run_req.token_position = token_position;
      run_req.deadline = request.deadline;

      Result<inference::RunResult> stage_result =
          is_local_stage(stage)
              ? execute_local_stage(stage, run_req, *cancel_token)
              : execute_remote_stage(stage, run_req, *cancel_token);

      if (stage_result.is_err()) {
        return stage_result;
      }

      // Output of this stage becomes input for the next
      current_inputs = std::move(stage_result.value().outputs);

      // If this is the last stage, return the result (contains token_id)
      if (&stage == &plan.stages.back()) {
        return stage_result;
      }
    }

    return Result<inference::RunResult>::Err(
        ErrorCode::kInternal, "pipeline completed without a final stage");
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  SIMULATED: fake token generation (fallback when no transport/backends)
  // ═══════════════════════════════════════════════════════════════════════

  void run_generation_simulated(std::shared_ptr<ActiveRequest> state,
                                 std::shared_ptr<CancellationToken> cancel_token) {
    const auto& request = state->request;

    for (std::uint64_t i = 0;
         i < request.generation.maximum_new_tokens && state->active; ++i) {
      if (cancel_token->stop_requested() || !state->active) break;

      persistence::TokenRecord rec;
      rec.request_id = request.request_id;
      rec.session_id = request.session_id;
      rec.position = i;
      rec.token_id = static_cast<std::int32_t>(i);
      state->token_history.push_back(rec);
      state->next_expected_position = i + 1;

      emit_token(state, i, static_cast<std::int32_t>(i),
                 "tok:" + std::to_string(i));
      state->tokens_emitted++;

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    emit_terminal(state, state->active);
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  PRODUCTION: real distributed inference
  // ═══════════════════════════════════════════════════════════════════════

  void run_generation_distributed(std::shared_ptr<ActiveRequest> state,
                                   std::shared_ptr<CancellationToken> cancel_token) {
    const auto& request = state->request;
    const auto& plan = state->plan; (void)plan;

    // Initial inputs: prompt tokens as embedding (first stage handles this)
    std::vector<inference::NamedTensor> initial_inputs;

    for (std::uint64_t pos = 0;
         pos < request.generation.maximum_new_tokens && state->active; ++pos) {
      if (cancel_token->stop_requested() || !state->active) break;

      auto result = execute_pipeline_pass(state, pos, initial_inputs, cancel_token);

      if (result.is_err()) {
        // Emit error as a failed terminal event
        StreamEvent ev;
        ev.kind = StreamEventKind::kFailed;
        ev.request_id = request.request_id;
        ev.sequence = state->tokens_emitted;
        ev.error = RuntimeError{result.error_code(),
                                std::string(result.error().what())};
        std::lock_guard cb_lock(state->callback_mutex);
        if (state->callback) state->callback(ev);
        state->active = false;
        return;
      }

      auto& run_result = result.value();
      std::int32_t token_id = run_result.token_id.value_or(
          static_cast<std::int32_t>(pos));

      persistence::TokenRecord rec;
      rec.request_id = request.request_id;
      rec.session_id = request.session_id;
      rec.position = pos;
      rec.token_id = token_id;
      state->token_history.push_back(rec);
      state->next_expected_position = pos + 1;

      emit_token(state, pos, token_id,
                 "tok:" + std::to_string(pos));
      state->tokens_emitted++;

      // Use the output as input for the next position (KV cache is internal)
      initial_inputs = std::move(run_result.outputs);
    }

    emit_terminal(state, state->active);
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  LOCAL: real single-node inference (no transport needed)
  // ═══════════════════════════════════════════════════════════════════════

#if SOCRATES_HAS_LLAMA_CPP
  void run_generation_local(std::shared_ptr<ActiveRequest> state,
                             std::shared_ptr<CancellationToken> cancel_token) {
    const auto& request = state->request;

    // Find a GGUF file — try master path, then worker path.
    // Match by model_id if possible, otherwise pick smallest real file.
    std::string model_path;
    std::uintmax_t best_size = UINTMAX_MAX;
    std::string model_id_str = request.model_id.value;
    for (const auto& dir : {"/tmp/socrates-master/models", "/tmp/socrates-worker/models"}) {
      std::error_code ec;
      if (!std::filesystem::exists(dir, ec)) continue;
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".gguf") continue;
        auto sz = entry.file_size(ec);
        if (ec || sz < 1000000) continue;  // skip stubs
        std::string fname = entry.path().filename().string();
        // Try to match model_id to filename
        if (!model_id_str.empty()) {
          std::string lower = fname;
          std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
          std::string lower_id = model_id_str;
          std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(), ::tolower);
          if (lower_id == "llama3-8b" && lower.find("llama-3-8b") != std::string::npos) {
            model_path = entry.path().string();
            break;
          }
          if (lower_id == "qwen3-1.8b" && lower.find("1.5b") != std::string::npos) {
            model_path = entry.path().string();
            break;
          }
          if (lower_id == "qwen3-4b" && lower.find("3b") != std::string::npos) {
            model_path = entry.path().string();
            break;
          }
          if (lower_id == "qwen3-6b" && lower.find("7b") != std::string::npos) {
            model_path = entry.path().string();
            break;
          }
          if (lower_id == "gemma12b" && lower.find("9b") != std::string::npos) {
            model_path = entry.path().string();
            break;
          }
          if (lower_id == "gemma26b" && lower.find("27b") != std::string::npos) {
            model_path = entry.path().string();
            break;
          }
          if (lower_id == "gemma4-26b" && lower.find("gemma-4") != std::string::npos) {
            model_path = entry.path().string();
            break;
          }
        }
        // Fallback: pick smallest
        if (sz < best_size) {
          best_size = sz;
          if (model_path.empty() || !model_id_str.empty()) continue; // prefer match
          model_path = entry.path().string();
        }
      }
      if (!model_path.empty()) break;
    }

    if (model_path.empty()) {
      emit_terminal(state, false);
      return;
    }

    // Initialize llama backend
    llama_backend_init();
    ggml_backend_load_all();
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 99;  // offload all layers to Metal GPU
    model_params.use_mmap = true;

    llama_model* model = llama_model_load_from_file(model_path.c_str(), model_params);
    if (!model) {
      emit_terminal(state, false);
      return;
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = request.generation.context_window;
    ctx_params.n_batch = 512;
    ctx_params.n_threads = 4;

    llama_context* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) {
      llama_model_free(model);
      emit_terminal(state, false);
      return;
    }

    const llama_vocab* vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);

    // Tokenize the prompt
    std::vector<llama_token> prompt_tokens(request.prompt.size() + 4);
    int n_tokens = llama_tokenize(vocab,
        request.prompt.c_str(), static_cast<int>(request.prompt.size()),
        prompt_tokens.data(), static_cast<int>(prompt_tokens.size()),
        true, true);
    // llama_tokenize may return negative if buffer too small
    if (n_tokens < 0) {
      prompt_tokens.resize(static_cast<size_t>(-n_tokens));
      n_tokens = llama_tokenize(vocab,
          request.prompt.c_str(), static_cast<int>(request.prompt.size()),
          prompt_tokens.data(), static_cast<int>(prompt_tokens.size()),
          true, true);
      if (n_tokens < 0) n_tokens = -n_tokens;
    }
    if (n_tokens <= 0) {
      llama_free(ctx);
      llama_model_free(model);
      emit_terminal(state, false);
      return;
    }
    prompt_tokens.resize(static_cast<size_t>(n_tokens));

    // Eval the prompt
    llama_pos pos = 0;
    for (size_t i = 0; i < prompt_tokens.size(); i += static_cast<size_t>(ctx_params.n_batch)) {
      size_t n = std::min(static_cast<size_t>(ctx_params.n_batch),
                           prompt_tokens.size() - i);
      llama_batch batch = llama_batch_init(static_cast<int32_t>(n), 0, 1);
      for (size_t j = 0; j < n; ++j) {
        batch.token[j] = prompt_tokens[i + j];
        batch.pos[j] = pos++;
        batch.n_seq_id[j] = 1;
        batch.seq_id[j][0] = 0;
        batch.logits[j] = (j == n - 1);
      }
      batch.n_tokens = static_cast<int32_t>(n);  // explicitly set
      if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch);
        llama_free(ctx);
        llama_model_free(model);
        emit_terminal(state, false);
        return;
      }
      llama_batch_free(batch);
    }

    // Generate tokens
    for (std::uint64_t i = 0;
         i < request.generation.maximum_new_tokens && state->active; ++i) {
      if (cancel_token->stop_requested() || !state->active) break;

      auto* logits = llama_get_logits_ith(ctx, -1);
      if (!logits) break;

      // Greedy sampling
      llama_token next_token = 0;
      float max_logit = -INFINITY;
      for (int v = 0; v < n_vocab; ++v) {
        if (logits[v] > max_logit) {
          max_logit = logits[v];
          next_token = v;
        }
      }

      if (llama_vocab_is_eog(vocab, next_token)) break;

      // Detokenize
      char buf[256];
      int n_chars = llama_token_to_piece(vocab, next_token, buf, sizeof(buf), 0, true);
      if (n_chars > 0) {
        std::string piece(buf, static_cast<size_t>(n_chars));
        emit_token(state, i, next_token, piece);
      }

      // Eval the new token with correct position
      llama_batch batch = llama_batch_init(1, 0, 1);
      batch.token[0] = next_token;
      batch.pos[0] = pos++;
      batch.n_seq_id[0] = 1;
      batch.seq_id[0][0] = 0;
      batch.logits[0] = true;
      batch.n_tokens = 1;
      if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch);
        break;
      }
      llama_batch_free(batch);
    }

    llama_free(ctx);
    llama_model_free(model);
    emit_terminal(state, state->active);
  }
#endif

  // ═══════════════════════════════════════════════════════════════════════
  //  Dispatcher: local → simulated → distributed
  // ═══════════════════════════════════════════════════════════════════════

  void run_generation(std::shared_ptr<ActiveRequest> state,
                      std::shared_ptr<CancellationToken> cancel_token) {
#if SOCRATES_HAS_LLAMA_CPP
    run_generation_local(state, cancel_token);
#else
    if (has_distributed_capability()) {
      run_generation_distributed(state, cancel_token);
    } else {
      run_generation_simulated(state, cancel_token);
    }
#endif
  }

  // ── Members ───────────────────────────────────────────────────────────

  // External dependencies (nullable — when null, simulated mode is used)
  transport::Transport* transport_{nullptr};
  inference::BackendRegistry* backends_{nullptr};
  model::ModelManager* models_{nullptr};
  // TraceRecorder* tracer_{nullptr}; // TODO: wire tracing

  std::unique_ptr<transport::TensorCodec> codec_;

  // Local node identity for stage routing
  NodeId local_node_id_;

  // Active requests
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<ActiveRequest>> active_requests_;

  // Worker threads
  mutable std::mutex workers_mutex_;
  std::unordered_map<std::string, std::thread> workers_;

  // Batching queue
  mutable std::mutex queue_mutex_;
  std::deque<QueuedRequest> pending_queue_;
  std::condition_variable queue_cv_;
  BatchingConfig batching_config_;
  std::thread dispatcher_thread_;
  bool production_mode_{false};
  std::atomic<bool> dispatcher_running_{false};

  // Loaded backend sessions (cached per stage)
  mutable std::mutex sessions_mutex_;
  std::vector<LoadedStage> loaded_sessions_;
};

std::unique_ptr<InferencePipeline> make_inference_pipeline(
    transport::Transport* transport,
    inference::BackendRegistry* backends,
    model::ModelManager* models,
    TraceRecorder* tracer) {
  return std::make_unique<InferencePipelineImpl>(
      transport, backends, models, tracer);
}

}  // namespace socrates::pipeline
