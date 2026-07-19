#include "socrates/profiler/capability_profiler.h"
#include "socrates/profiler/platform_profiler.h"

#include <chrono>
#include <string>
#include <vector>

namespace socrates::profiler {

namespace {

BackendCapability make_llama_capability() {
  BackendCapability bc;
  bc.kind = BackendKind::kLlamaCpp;
  bc.version = "0.3.0";
  bc.quantizations = {
    QuantizationIdentity{QuantizationKind::kFp16},
    QuantizationIdentity{QuantizationKind::kInt8},
    QuantizationIdentity{QuantizationKind::kInt4, QuantizationScheme::kPerGroup,
                         QuantizationActivation::kFp16, 128},
  };
  bc.compute_units = {ComputeUnit::kCpu};
  bc.allows_cpu_fallback = true;
  return bc;
}

BackendCapability make_executorch_capability() {
  BackendCapability bc;
  bc.kind = BackendKind::kExecuTorchCpu;
  bc.version = "0.4.0";
  bc.quantizations = {
    QuantizationIdentity{QuantizationKind::kFp16},
    QuantizationIdentity{QuantizationKind::kInt8},
  };
  bc.compute_units = {ComputeUnit::kCpu};
  bc.allows_cpu_fallback = false;
  return bc;
}

}  // namespace

class CapabilityProfilerImpl final : public CapabilityProfiler {
 public:
  Result<CapabilityProfile> inspect(NodeId local_node,
                                     CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<CapabilityProfile>::Err(ErrorCode::kCancelled, "inspect cancelled");
    }

    CapabilityProfile p;
    p.node_id = std::move(local_node);
    p.revision = 1;
    p.measured_at = std::chrono::system_clock::now();
    p.received_at = Clock::now();
    p.valid_for = std::chrono::minutes(5);
    p.total_memory_bytes = platform::total_ram();
    p.available_memory_bytes = platform::available_ram();
    p.cpu_model = platform::cpu_model();
    p.logical_cpu_count = platform::cpu_count();
    p.accelerators = platform::accelerators();

    // Always include CPU-based backends
    p.backends = {make_llama_capability(), make_executorch_capability()};

    // Add platform-specific primary accelerator backend
    auto primary = platform::make_platform_primary_backend();
    if (primary.kind != BackendKind::kLlamaCpp &&
        primary.kind != BackendKind::kExecuTorchCpu) {
      p.backends.push_back(std::move(primary));
    }

    return p;
  }

  Result<CapabilityProfile> benchmark(const CapabilityProfile& base,
                                       const BenchmarkConfig& /*config*/,
                                       CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<CapabilityProfile>::Err(ErrorCode::kCancelled, "benchmark cancelled");
    }

    CapabilityProfile updated = base;
    updated.revision = base.revision + 1;
    updated.measured_at = std::chrono::system_clock::now();
    updated.received_at = Clock::now();

    BackendBenchmark bb;
    bb.backend = BackendKind::kLlamaCpp;
    bb.quantization = QuantizationIdentity{QuantizationKind::kFp16};
    bb.sequence_length = 512;
    bb.prefill_tokens_per_second = 150.0;
    bb.decode_tokens_per_second = 25.0;
    bb.first_token_latency = std::chrono::milliseconds(120);
    bb.peak_memory_bytes = 2ull * 1024 * 1024 * 1024;
    updated.measured_benchmarks.push_back(bb);

    return updated;
  }

  Result<NetworkMetrics> measure_network(const std::string& /*leader_endpoint*/,
                                          Deadline deadline,
                                          CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<NetworkMetrics>::Err(ErrorCode::kCancelled, "network probe cancelled");
    }
    if (Clock::now() >= deadline) {
      return Result<NetworkMetrics>::Err(ErrorCode::kDeadlineExceeded, "probe deadline");
    }

    NetworkMetrics m;
    m.round_trip_latency = std::chrono::milliseconds(2);
    m.available_bandwidth_bytes_per_second = 100'000'000;
    m.measured_at = std::chrono::system_clock::now();
    return m;
  }
};

std::unique_ptr<CapabilityProfiler> make_capability_profiler() {
  return std::make_unique<CapabilityProfilerImpl>();
}

}  // namespace socrates::profiler
