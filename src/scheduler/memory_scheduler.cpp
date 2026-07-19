#include "socrates/scheduler/scheduler.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace socrates::scheduler {

namespace {

std::string make_plan_id() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::ostringstream os;
  os << "plan-" << std::hex << rng();
  return os.str();
}

// ---------------------------------------------------------------------------
// Backend compatibility
// ---------------------------------------------------------------------------

/**
 * Checks whether @p shard is compatible with @p cap and returns the chosen
 * BackendKind if so.  An empty `compatible_backends` list means "any backend
 * is acceptable"; an empty `required_compute_units` list means "any compute
 * unit is acceptable".
 */
std::optional<BackendKind> find_compatible_backend(
    const ShardOption& shard, const CapabilityProfile& cap) {
  const bool any_backend_ok = shard.compatible_backends.empty();
  const bool any_unit_ok = shard.required_compute_units.empty();

  for (const auto& cap_be : cap.backends) {
    // Must match one of the shard's compatible backends (or accept any).
    if (!any_backend_ok) {
      bool found = false;
      for (const auto& shard_be : shard.compatible_backends) {
        if (cap_be.kind == shard_be) {
          found = true;
          break;
        }
      }
      if (!found) continue;
    }

    // Must support at least one of the shard's required compute units.
    if (any_unit_ok) return cap_be.kind;

    for (const auto& req_cu : shard.required_compute_units) {
      for (const auto& cu : cap_be.compute_units) {
        if (cu == req_cu) return cap_be.kind;
      }
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Time estimate
// ---------------------------------------------------------------------------

/**
 * Derives a per-stage time estimate in microseconds.  Prefers the node's
 * BackendBenchmark (matching backend + quantization); falls back to the
 * shard's own estimated_decode_microseconds.
 */
double compute_time_estimate(const ShardOption& shard,
                             const CapabilityProfile& cap,
                             BackendKind backend) {
  for (const auto& bm : cap.measured_benchmarks) {
    if (bm.backend == backend && bm.quantization.kind == shard.quantization) {
      if (bm.decode_tokens_per_second > 0.0) {
        // Convert tokens / second → microseconds per token.
        return 1'000'000.0 / bm.decode_tokens_per_second;
      }
      if (bm.first_token_latency.count() > 0) {
        return static_cast<double>(bm.first_token_latency.count());
      }
    }
  }
  return shard.estimated_decode_microseconds;
}

// ---------------------------------------------------------------------------
// Node bookkeeping
// ---------------------------------------------------------------------------

struct NodeSlot {
  NodeId node_id;
  const CapabilityProfile* capability;  // non-null
  std::uint64_t remaining_memory_bytes;
};

/**
 * Builds a vector of NodeSlot from the membership snapshot, keeping only
 * alive members that have a capability profile.
 */
std::vector<NodeSlot> build_node_slots(const SchedulingInput& input) {
  std::vector<NodeSlot> slots;
  for (const auto& m : input.membership.members) {
    if (m.state != cluster::MemberState::kAlive) continue;
    if (!m.capability.has_value()) continue;
    slots.push_back(NodeSlot{
        .node_id = m.peer.node_id,
        .capability = &m.capability.value(),
        .remaining_memory_bytes = m.capability->available_memory_bytes,
    });
  }
  return slots;
}

// ---------------------------------------------------------------------------
// Comparator helpers
// ---------------------------------------------------------------------------

bool sensitivity_descending(const ShardOption& a, const ShardOption& b) {
  return a.sensitivity_score > b.sensitivity_score;
}

bool by_layer_start(const StageAssignment& a, const StageAssignment& b) {
  if (a.layers.has_value() && b.layers.has_value()) {
    return a.layers->start < b.layers->start;
  }
  // Push non-layer stages to the front.
  return a.layers.has_value() ? false : (b.layers.has_value() ? true : false);
}

bool by_remaining_memory_desc(const NodeSlot& a, const NodeSlot& b) {
  return a.remaining_memory_bytes > b.remaining_memory_bytes;
}

// ---------------------------------------------------------------------------
// Shared scheduling core
// ---------------------------------------------------------------------------

/**
 * Core placement engine.  @p node_slots is passed by value so that callers
 * can pre-order the nodes however they wish (round-robin / sensitivity).
 *
 * @param sort_nodes_before_each_shard  When true, re-sorts node_slots by
 *                                      remaining memory descending before
 *                                      every shard assignment so that the
 *                                      highest-sensitivity shards always get
 *                                      the most resource-rich nodes.
 */
Result<PipelinePlan> schedule_core(const SchedulingInput& input,
                                    std::vector<NodeSlot> node_slots,
                                    bool sort_nodes_before_each_shard) {
  // -- 1. Validate inputs ---------------------------------------------------
  if (input.shard_options.empty()) {
    return Result<PipelinePlan>::Err(ErrorCode::kInvalidArgument,
                                     "no shard options");
  }
  if (node_slots.empty()) {
    return Result<PipelinePlan>::Err(ErrorCode::kFailedPrecondition,
                                     "no live members with capabilities");
  }

  PipelinePlan plan;
  plan.plan_id = make_plan_id();
  plan.model_id = input.model_id;
  plan.manifest_revision = input.manifest_revision;
  plan.fence = input.fence;
  plan.issued_at_utc = std::chrono::system_clock::now();
  plan.valid_for = std::chrono::minutes(5);
  plan.boundaries = input.boundaries;

  // -- 2. Order shards: highest sensitivity first ----------------------------
  std::vector<ShardOption> ordered_shards = input.shard_options;
  std::stable_sort(ordered_shards.begin(), ordered_shards.end(),
                   sensitivity_descending);

  const bool use_network_affinity = !input.network_links.empty();
  const std::uint64_t per_device_reserve = input.per_device_memory_reserve_bytes;

  std::uint32_t ordinal = 0;
  std::optional<NodeId> last_node;  // for network-aware favouring
  std::unordered_set<std::string> assigned_layer_ranges;

  // -- 3. Place each shard ---------------------------------------------------
  for (const auto& shard : ordered_shards) {
    // Detect duplicate / overlapping layer ranges.
    if (shard.layers.has_value()) {
      std::string range_key = std::to_string(shard.layers->start) + "-" +
                              std::to_string(shard.layers->end_exclusive);
      if (assigned_layer_ranges.count(range_key)) {
        return Result<PipelinePlan>::Err(
            ErrorCode::kInvalidArgument,
            "duplicate layer range [" + range_key + ")");
      }
      assigned_layer_ranges.insert(range_key);
    }

    // Optionally re-sort nodes so the richest ones are tried first.
    if (sort_nodes_before_each_shard) {
      std::sort(node_slots.begin(), node_slots.end(),
                by_remaining_memory_desc);
    }

    std::uint64_t required_bytes =
        shard.peak_runtime_memory_bytes + per_device_reserve;

    bool assigned = false;

    // If network affinity is enabled and we placed a shard previously, try
    // that same node first (it may still be a candidate after re-sorting).
    auto try_slot = [&](NodeSlot& slot) -> bool {
      if (slot.remaining_memory_bytes < required_bytes) return false;

      auto compat = find_compatible_backend(shard, *slot.capability);
      if (!compat.has_value()) return false;

      // Accept this node.
      slot.remaining_memory_bytes -= required_bytes;

      StageAssignment sa;
      sa.ordinal = ordinal++;
      sa.node_id = slot.node_id;
      sa.shard_id = shard.shard_id;
      sa.stage_ids = shard.stage_ids;
      sa.input_boundary_ids = shard.input_boundary_ids;
      sa.output_boundary_ids = shard.output_boundary_ids;
      sa.stage_kind = shard.stage_kind;
      sa.layers = shard.layers;
      sa.quantization = shard.quantization;
      sa.backend = compat.value();
      sa.execution_profile_id = shard.execution_profile_id;
      sa.artifact = shard.artifact;
      sa.reserved_memory_bytes = required_bytes;
      sa.estimated_stage_microseconds =
          compute_time_estimate(shard, *slot.capability, compat.value());

      plan.stages.push_back(std::move(sa));
      last_node = slot.node_id;
      return true;
    };

    // Network-affinity fast-path: try the last-used node.
    if (use_network_affinity && last_node.has_value()) {
      for (auto& slot : node_slots) {
        if (slot.node_id == *last_node) {
          if (try_slot(slot)) {
            assigned = true;
            break;
          }
          break;  // no point scanning further for the same id
        }
      }
    }

    // Fall-back: scan every node.
    if (!assigned) {
      for (auto& slot : node_slots) {
        if (use_network_affinity && last_node.has_value() &&
            slot.node_id == *last_node) {
          continue;  // already tried above
        }
        if (try_slot(slot)) {
          assigned = true;
          break;
        }
      }
    }

    if (!assigned) {
      return Result<PipelinePlan>::Err(
          ErrorCode::kResourceExhausted,
          "no compatible node with sufficient memory for shard " +
              shard.shard_id.value);
    }
  }

  // -- 4. Layer-range contiguity check (only for shards that carry layers) --
  std::vector<StageAssignment*> layer_stages;
  for (auto& sa : plan.stages) {
    if (sa.layers.has_value()) layer_stages.push_back(&sa);
  }

  if (!layer_stages.empty()) {
    std::sort(layer_stages.begin(), layer_stages.end(),
              [](const StageAssignment* a, const StageAssignment* b) {
                return a->layers->start < b->layers->start;
              });

    // Must cover [0, total_layers) without gaps or overlaps.
    std::uint32_t expected = 0;
    for (const auto* sa : layer_stages) {
      if (sa->layers->start != expected) {
        return Result<PipelinePlan>::Err(
            ErrorCode::kInvalidArgument,
            "layer gap or overlap: expected start " +
                std::to_string(expected) + " but got " +
                std::to_string(sa->layers->start));
      }
      expected = sa->layers->end_exclusive;
    }

    if (expected != input.total_layers) {
      return Result<PipelinePlan>::Err(
          ErrorCode::kInvalidArgument,
          "layer range does not cover all layers: ends at " +
              std::to_string(expected) + " but total is " +
              std::to_string(input.total_layers));
    }
  }

  // -- 5. Final ordering -----------------------------------------------------
  std::sort(plan.stages.begin(), plan.stages.end(), by_layer_start);

  return Result<PipelinePlan>::Ok(plan);
}

}  // namespace

// ============================================================================
// MemorySchedulerImpl
// ============================================================================

class MemorySchedulerImpl final : public MemoryScheduler {
 public:
  Result<PipelinePlan> create_plan(const SchedulingInput& input) const override {
      // Sort nodes by remaining memory before each shard so work is naturally
      // distributed across the cluster rather than piled onto the first node.
      return schedule_core(input, build_node_slots(input),
                           /*sort_nodes_before_each_shard=*/true);
    }

  Result<PipelinePlan> refine_plan(
      const SchedulingInput& input,
      const PipelinePlan& /*base*/,
      const std::vector<CalibrationSample>& /*samples*/) const override {
    return create_plan(input);
  }
};

// ============================================================================
// SensitivitySchedulerImpl
// ============================================================================

class SensitivitySchedulerImpl final : public SensitivityScheduler {
 public:
  Result<PipelinePlan> create_plan(const SchedulingInput& input) const override {
    // Before each shard the node list is re-sorted by remaining memory
    // descending so that high-sensitivity shards always pick the richest node.
    return schedule_core(input, build_node_slots(input),
                         /*sort_nodes_before_each_shard=*/true);
  }

  Result<PipelinePlan> refine_plan(
      const SchedulingInput& input,
      const PipelinePlan& /*base*/,
      const std::vector<CalibrationSample>& /*samples*/) const override {
    return create_plan(input);
  }
};

// ============================================================================
// Factory functions
// ============================================================================

std::unique_ptr<MemoryScheduler> make_memory_scheduler() {
  return std::make_unique<MemorySchedulerImpl>();
}

std::unique_ptr<SensitivityScheduler> make_sensitivity_scheduler() {
  return std::make_unique<SensitivitySchedulerImpl>();
}

}  // namespace socrates::scheduler
