#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "socrates/cluster/membership_service.h"
#include "socrates/result.h"
#include "socrates/types.h"

namespace socrates::scheduler {

enum class SchedulingPolicy { kMemoryBased, kSensitivityAware };
enum class StageKind { kTokenEmbedding, kTransformerLayers, kOutputNormalization, kLmHead };
enum class CalibrationPhase { kPrefill, kDecode, kTransfer };

struct ArtifactDescriptor {
  std::string uri;
  std::string sha256_hex;
  std::string format;
  std::string format_version;
  std::uint64_t file_bytes{0};
};

struct BoundaryTensorContract {
  std::string tensor_id;
  ElementType element_type{ElementType::kFloat32};
  std::vector<std::int64_t> maximum_shape;
  std::string layout;
  std::uint64_t maximum_encoded_bytes{0};
};

struct BoundaryContract {
  std::string boundary_id;
  std::string producer_stage_id;
  std::string consumer_stage_id;
  std::vector<BoundaryTensorContract> tensors;
  bool transferable{false};
  bool reliable_ordered_required{true};
};

struct ShardOption {
  ShardId shard_id;
  std::vector<std::string> stage_ids;
  std::vector<std::string> input_boundary_ids;
  std::vector<std::string> output_boundary_ids;
  StageKind stage_kind{StageKind::kTransformerLayers};
  std::optional<LayerRange> layers;
  QuantizationKind quantization{QuantizationKind::kFp16};
  ArtifactDescriptor artifact;
  std::string execution_profile_id;
  std::uint64_t peak_runtime_memory_bytes{0};
  std::uint64_t estimated_kv_bytes_per_token{0};
  std::vector<BackendKind> compatible_backends;
  std::vector<ComputeUnit> required_compute_units;
  std::vector<std::string> required_operator_ids;
  double estimated_prefill_microseconds{0.0};
  double estimated_decode_microseconds{0.0};
  double sensitivity_score{0.0};
};

struct NetworkLink {
  NodeId source;
  NodeId destination;
  std::chrono::microseconds latency{0};
  std::uint64_t available_bandwidth_bytes_per_second{0};
};

struct SchedulingInput {
  ModelId model_id;
  std::string manifest_revision;
  std::uint32_t total_layers{0};
  std::vector<ShardOption> shard_options;
  std::vector<BoundaryContract> boundaries;
  cluster::MembershipSnapshot membership;
  std::vector<NetworkLink> network_links;
  LeadershipFence fence;
  SchedulingPolicy policy{SchedulingPolicy::kMemoryBased};
  std::uint32_t maximum_context_tokens{0};
  std::uint32_t maximum_generation_tokens{0};
  std::uint64_t per_device_memory_reserve_bytes{0};
  double maximum_quality_loss_score{0.0};
  double compute_balance_weight{1.0};
  double transfer_latency_weight{1.0};
  bool use_calibrated_costs{false};
};

struct StageAssignment {
  std::uint32_t ordinal{0};
  NodeId node_id;
  ShardId shard_id;
  std::vector<std::string> stage_ids;
  std::vector<std::string> input_boundary_ids;
  std::vector<std::string> output_boundary_ids;
  StageKind stage_kind{StageKind::kTransformerLayers};
  std::optional<LayerRange> layers;
  QuantizationKind quantization{QuantizationKind::kFp16};
  BackendKind backend{BackendKind::kLlamaCpp};
  std::string execution_profile_id;
  ArtifactDescriptor artifact;
  std::uint64_t reserved_memory_bytes{0};
  double estimated_stage_microseconds{0.0};
};

struct PipelinePlan {
  std::string plan_id;
  ModelId model_id;
  std::string manifest_revision;
  LeadershipFence fence;
  std::vector<StageAssignment> stages;
  std::vector<BoundaryContract> boundaries;
  std::chrono::system_clock::time_point issued_at_utc;
  std::chrono::milliseconds valid_for{0};
};

struct CalibrationSample {
  NodeId node_id;
  ShardId shard_id;
  BackendKind backend{BackendKind::kLlamaCpp};
  CalibrationPhase phase{CalibrationPhase::kDecode};
  std::uint32_t batch_size{0};
  std::uint32_t sequence_length{0};
  std::uint64_t transferred_bytes{0};
  std::chrono::microseconds measured_duration{0};
  std::uint64_t peak_memory_bytes{0};
};

class Scheduler {
 public:
  virtual ~Scheduler() = default;

  /**
   * Produces an ordered, complete, non-overlapping pipeline plan.
   * Preconditions: manifest revision is validated; membership and capabilities
   * are immutable for this call; fence is current; layer/shard ranges are valid.
   * Postconditions: success covers every required stage exactly once and every
   * assignment fits memory, quantization, artifact, backend, and compute-unit
   * constraints. Input objects are unchanged.
   * Throws: no operational exceptions; Result reports infeasible or invalid
   * plans. Implementations MUST be deterministic for equivalent normalized input.
   * Thread safety: const and safe for concurrent calls.
   * Side effects: none; scheduling performs no I/O or backend initialization.
   */
  [[nodiscard]] virtual Result<PipelinePlan> create_plan(
      const SchedulingInput& input) const = 0;

  /**
   * Refines a prior plan from bounded calibration measurements.
   * Preconditions: base plan fence and manifest match input; every sample refers
   * to an assigned node/shard/backend and has a finite positive duration.
   * Postconditions: success returns a newly identified valid plan; base is not
   * modified. Refinement may retain the original plan.
   * Throws: no operational exceptions; invalid samples use Result.
   * Thread safety: const and concurrently callable.
   * Side effects: none.
   */
  [[nodiscard]] virtual Result<PipelinePlan> refine_plan(
      const SchedulingInput& input,
      const PipelinePlan& base,
      const std::vector<CalibrationSample>& samples) const = 0;
};

class MemoryScheduler : public Scheduler {};
class SensitivityScheduler : public Scheduler {};

std::unique_ptr<MemoryScheduler> make_memory_scheduler();
std::unique_ptr<SensitivityScheduler> make_sensitivity_scheduler();

}  // namespace socrates::scheduler
