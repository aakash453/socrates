#pragma once

#include <memory>
#include <optional>
#include "socrates/cancellation.h"
#include <vector>

#include "socrates/model/model_manager.h"
#include "socrates/result.h"
#include "socrates/types.h"

namespace socrates::inference {

struct LoadOptions {
  QuantizationKind quantization{QuantizationKind::kFp16};
  std::vector<std::string> stage_ids;
  std::optional<LayerRange> layers;
  std::string execution_profile_id;
  std::uint64_t memory_budget_bytes{0};
  Attributes backend_options;
};

struct NamedTensor {
  std::string tensor_id;
  Tensor value;
};

struct RunRequest {
  RequestId request_id;
  SessionId session_id;
  std::string stage_id;
  std::string input_boundary_id;
  std::string output_boundary_id;
  std::vector<NamedTensor> inputs;
  std::optional<LayerRange> layers;
  std::uint64_t token_position{0};
  Deadline deadline;
};

struct RunResult {
  std::vector<NamedTensor> outputs;
  std::optional<std::int32_t> token_id;
  std::chrono::microseconds elapsed{0};
};

class InferenceSession {
 public:
  virtual ~InferenceSession() = default;

  /**
   * Executes exactly one loaded graph stage or transformer layer range.
   * Preconditions: stage/range and named input boundary tensors match the loaded
   * manifest contracts; token position, session, and deadline are valid.
   * Postconditions: success returns the declared boundary tensor or a token from
   * the final stage; KV updates are committed atomically and remain local.
   * Throws: no operational exceptions; Result reports unsupported operations,
   * cancellation, timeout, malformed tensors, and backend failures.
   * Thread safety: requests for one session are serialized; distinct sessions may
   * run concurrently subject to backend limits.
   * Side effects: executes compute and updates local KV cache for owned layers.
   */
  virtual Result<RunResult> run_layers(
      const RunRequest& request,
      CancellationToken cancellation) = 0;

  /**
   * Releases request-specific KV state.
   * Preconditions: session ID belongs to this loaded backend session and no run
   * for it is active.
   * Postconditions: associated KV memory is reclaimable; other sessions survive.
   * Throws: no operational exceptions; missing sessions are idempotent success.
   * Thread safety: safe for different session IDs; serialized with run for same ID.
   * Side effects: frees backend memory.
   */
  virtual Result<bool> clear_kv_cache(const SessionId& session_id) = 0;
};

class InferenceBackend {
 public:
  virtual ~InferenceBackend() = default;

  /**
   * Returns this adapter's stable backend kind and detected capability.
   * Preconditions: none.
   * Postconditions: result does not initialize or load a model.
   * Throws: no operational exceptions.
   * Thread safety: const and concurrently callable.
   * Side effects: MAY query cached driver/runtime metadata only.
   */
  [[nodiscard]] virtual Result<BackendCapability> capability() const = 0;

  /**
   * Loads one leased, verified backend-specific shard.
   * Preconditions: lease remains valid for session lifetime; quantization/range
   * match the manifest; memory budget is sufficient; cancellation is not set.
   * Postconditions: success returns an isolated session ready for run_layers().
   * If QNN initialization fails, kUnavailable is returned; this method does not
   * silently choose CPU.
   * Throws: no operational exceptions.
   * Thread safety: concurrent loads are allowed if backend capability permits.
   * Side effects: maps model data, allocates backend/KV workspace, initializes
   * CPU/GPU/NPU delegates, and consumes memory.
   */
  virtual Result<std::unique_ptr<InferenceSession>> load_model(
      const model::ShardLease& shard,
      const LoadOptions& options,
      CancellationToken cancellation) = 0;
};

struct FallbackRequest {
  BackendKind requested{BackendKind::kExecuTorchQnn};
  std::string artifact_format;
  std::string quantization_id;
  std::string requested_profile_id;
  std::optional<std::string> cpu_fallback_profile_id;
  std::vector<BackendKind> artifact_compatible_backends;
  bool allow_cpu_fallback{false};
};

class BackendRegistry {
 public:
  virtual ~BackendRegistry() = default;

  /**
   * Resolves a registered backend adapter.
   * Preconditions: kind was compiled and registered.
   * Postconditions: returned shared adapter remains valid while registry lives.
   * Throws: no operational exceptions; kNotFound when unavailable.
   * Thread safety: safe for concurrent resolution.
   * Side effects: none.
   */
  [[nodiscard]] virtual Result<std::shared_ptr<InferenceBackend>> resolve(
      BackendKind kind) const = 0;

  /**
   * Returns scheduler-visible fallback candidates in strict preference order.
   * Preconditions: requested backend and artifact format are supplied by a
   * validated manifest.
   * Postconditions: CPU appears only when explicitly permitted and compatible.
   * Throws: no operational exceptions.
   * Thread safety: const and concurrently callable.
   * Side effects: none.
   */
  [[nodiscard]] virtual Result<std::vector<BackendKind>> fallback_chain(
        const FallbackRequest& request) const = 0;

    virtual void register_backend(BackendKind kind,
                                   std::shared_ptr<InferenceBackend> backend) = 0;
};

std::unique_ptr<BackendRegistry> make_backend_registry();

}  // namespace socrates::inference
