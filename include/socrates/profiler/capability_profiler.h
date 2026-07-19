#pragma once

#include <memory>
#include "socrates/cancellation.h"

#include "socrates/result.h"
#include "socrates/types.h"

namespace socrates::profiler {

struct BenchmarkConfig {
  std::filesystem::path synthetic_model_path;
  std::uint32_t warmup_runs{0};
  std::uint32_t measured_runs{0};
  std::uint64_t maximum_memory_bytes{0};
  Deadline deadline;
};

class CapabilityProfiler {
 public:
  virtual ~CapabilityProfiler() = default;

  /**
   * Captures static and dynamic hardware/backend capabilities.
   * Preconditions: local node identity is valid and cancellation is not already
   * requested.
   * Postconditions: success returns a timestamped, expiring profile with total
   * and available RAM, CPU, accelerators, quantizations, and backend/fallbacks.
   * Throws: no operational exceptions; failures are returned in Result.
   * Thread safety: one profile operation per instance at a time; concurrent calls
   * return kFailedPrecondition without mutating an active run.
   * Side effects: queries OS/runtime APIs and may initialize accelerator drivers.
   */
  virtual Result<CapabilityProfile> inspect(NodeId local_node,
                                            CancellationToken cancellation) = 0;

  /**
   * Runs bounded warm-up and measured synthetic inference benchmarks.
   * Preconditions: inspect() has succeeded; fixture exists; counts are non-zero;
   * requested memory fits the configured profiler budget.
   * Postconditions: success returns a new profile revision containing measured
   * prefill/decode throughput and first-token latency.
   * Throws: no operational exceptions; Result reports cancellation, timeout,
   * unavailable backend, and resource exhaustion.
   * Thread safety: exclusive per profiler instance.
   * Side effects: consumes compute, memory, energy, and may temporarily heat the
   * device; never writes model artifacts.
   */
  virtual Result<CapabilityProfile> benchmark(
      const CapabilityProfile& base,
      const BenchmarkConfig& config,
      CancellationToken cancellation) = 0;

  /**
   * Measures network latency and available bandwidth to the current leader.
   * Preconditions: leader endpoint is authenticated and deadline is in future.
   * Postconditions: success reports timestamped RTT and usable throughput.
   * Throws: no operational exceptions; transport failures use Result.
   * Thread safety: safe concurrently with inspect(), but not benchmark().
   * Side effects: sends bounded probe traffic to the leader.
   */
  virtual Result<NetworkMetrics> measure_network(
      const std::string& leader_endpoint,
      Deadline deadline,
      CancellationToken cancellation) = 0;
};

std::unique_ptr<CapabilityProfiler> make_capability_profiler();

}  // namespace socrates::profiler
