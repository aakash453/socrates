#pragma once

#include <functional>
#include <memory>
#include "socrates/cancellation.h"
#include <string>
#include <vector>

#include "socrates/inference/inference_backend.h"
#include "socrates/scheduler/scheduler.h"
#include "socrates/tracing.h"

namespace socrates::transport {
class Transport;
}  // namespace socrates::transport

namespace socrates::model {
class ModelManager;
}  // namespace socrates::model

namespace socrates::pipeline {

struct GenerationOptions {
  std::uint32_t maximum_new_tokens{0};
  std::uint32_t context_window{2048};  // total tokens (prompt + generation);
                                       // used by adaptive batching to decide
                                       // batch-vs-solo dispatch
  float temperature{0.0F};
  float top_p{0.0F};
  std::optional<std::uint64_t> seed;
};

struct InferenceRequest {
  RequestId request_id;
  SessionId session_id;
  ModelId model_id;
  std::string prompt;
  GenerationOptions generation;
  Deadline deadline;
};

// ── Batching configuration ────────────────────────────────────────────────

enum class BatchingMode : std::uint8_t {
  /// Dispatch every request immediately on its own thread (current default).
  /// Best for long-context / latency-sensitive workloads.
  kImmediate,

  /// Serial dispatch — one request at a time. Prevents OOM from
  /// concurrent long-context requests competing for KV-cache memory.
  kSerial,

  /// Adaptive: queue requests, group short-context requests into batches
  /// (up to max_batch_size within batch_timeout), dispatch long-context
  /// requests solo. Balances throughput and max context window.
  kAdaptive,
};

struct BatchingConfig {
  BatchingMode mode{BatchingMode::kImmediate};

  /// Maximum requests to coalesce into one batch (kAdaptive mode only).
  std::uint32_t max_batch_size{4};

  /// How long the dispatcher waits to accumulate a batch before flushing.
  std::chrono::milliseconds batch_timeout{5};

  /// Requests with context_window > this threshold are dispatched solo
  /// even in kAdaptive mode to preserve maximum context-window headroom.
  std::uint32_t long_context_threshold{4096};
};

enum class StreamEventKind { kStarted, kToken, kReplanning, kCompleted, kFailed, kQueued };

struct StreamEvent {
  StreamEventKind kind{StreamEventKind::kStarted};
  RequestId request_id;
  std::uint64_t sequence{0};
  std::string text_delta;
  std::optional<std::int32_t> token_id;
  std::optional<RuntimeError> error;
  Attributes metadata;
};

using StreamCallback = std::function<void(const StreamEvent&)>;

class GenerationHandle {
 public:
  virtual ~GenerationHandle() = default;

  /**
   * Requests cooperative cancellation of the distributed generation.
   * Preconditions: none.
   * Postconditions: no new token work is admitted; exactly one terminal event is
   * eventually delivered unless callback ownership has ended during shutdown.
   * Throws: never.
   * Thread safety: idempotent and concurrently callable.
   * Side effects: propagates cancellation to local and remote stages.
   */
  virtual void cancel() noexcept = 0;
};

class InferencePipeline {
 public:
  virtual ~InferencePipeline() = default;

  /**
   * Starts distributed streaming generation against a fenced plan.
   * Preconditions: request IDs are unique; callback non-empty; plan is complete,
   * unexpired, matches model, and its fence is current; assigned shards exist.
   * Postconditions: success returns a handle and emits strictly increasing event
   * sequences ending in exactly one completed/failed terminal event.
   * Throws: no operational exceptions; admission failures use Result.
   * Thread safety: concurrent requests are supported; callbacks for one request
   * are serialized and invoked without pipeline locks.
   * Side effects: loads only assigned shards, opens streams, executes inference,
   * updates local KV caches, records trace events, and emits streamed tokens.
   */
  virtual Result<std::unique_ptr<GenerationHandle>> generate(
      const InferenceRequest& request,
      const scheduler::PipelinePlan& plan,
      StreamCallback callback,
      CancellationToken cancellation) = 0;

  /**
   * Replaces the plan after a membership failure and resumes when feasible.
   * Preconditions: request is active; replacement has a newer valid fence and
   * covers the same model; retained token history is available.
   * Postconditions: stale remote work is fenced; replacement stages rebuild local
   * KV by replay before new token emission; token sequence does not regress.
   * Throws: no operational exceptions; infeasible recovery uses Result and the
   * request later emits a graceful failure event.
   * Thread safety: serialized internally with request execution.
   * Side effects: cancels old stages, loads replacement shards, replays history,
   * and emits a replanning event.
   */
  virtual Result<bool> replan(const RequestId& request_id,
                              const scheduler::PipelinePlan& replacement,
                              CancellationToken cancellation) = 0;

  /**
   * Sets the local node identity used to resolve the assigned stage in a plan.
   * Preconditions: node_id is non-empty before calling generate().
   * Postconditions: subsequent generate() calls route to the correct local stage.
   * Throws: never.
   * Thread safety: safe to call once before any generate() invocation.
   * Side effects: updates the internal routing key.
   */
  virtual void set_local_node_id(NodeId node_id) = 0;

  /// Configures request-admission strategy (immediate / serial / adaptive).
  virtual void set_batching_config(const BatchingConfig& config) = 0;

  /// When true, generate() fails if transport/backends are unavailable
  /// instead of falling back to simulated token generation.
  virtual void set_production_mode(bool enabled) = 0;

  virtual std::size_t active_generation_count() const = 0;

  /// Number of requests admitted but not yet dispatched from the queue.
  virtual std::size_t queued_request_count() const = 0;
};

std::unique_ptr<InferencePipeline> make_inference_pipeline(
    transport::Transport* transport = nullptr,
    inference::BackendRegistry* backends = nullptr,
    model::ModelManager* models = nullptr,
    TraceRecorder* tracer = nullptr);

}  // namespace socrates::pipeline
