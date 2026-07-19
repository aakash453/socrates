// tools/benchmark.cpp
// Performance benchmark runner with warmup and measured runs.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "socrates/profiler/capability_profiler.h"

int main(int argc, char* argv[]) {
  std::string output = "benchmark.json";
  int warmup = 3;
  int runs = 10;

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--warmup" && i + 1 < argc) warmup = std::stoi(argv[++i]);
    else if (arg == "--runs" && i + 1 < argc) runs = std::stoi(argv[++i]);
    else if (arg == "--output" && i + 1 < argc) output = argv[++i];
  }

  auto profiler = socrates::profiler::make_capability_profiler();
  auto base = profiler->inspect(socrates::NodeId{"bench-node"}, CancellationToken{});

  if (base.is_err()) {
    std::cerr << "Inspect failed: " << base.error().what() << std::endl;
    return 1;
  }

  socrates::profiler::BenchmarkConfig cfg;
  cfg.warmup_runs = static_cast<std::uint32_t>(warmup);
  cfg.measured_runs = static_cast<std::uint32_t>(runs);
  cfg.maximum_memory_bytes = base.value().available_memory_bytes;
  cfg.deadline = socrates::Clock::now() + std::chrono::minutes(5);

  auto result = profiler->benchmark(base.value(), cfg, CancellationToken{});

  if (result.is_err()) {
    std::cerr << "Benchmark failed: " << result.error().what() << std::endl;
    return 1;
  }

  auto& profile = result.value();
  std::ofstream out(output);
  out << "{\n";
  out << "  \"warmup_runs\": " << warmup << ",\n";
  out << "  \"measured_runs\": " << runs << ",\n";
  out << "  \"benchmarks\": [";
  for (std::size_t i = 0; i < profile.measured_benchmarks.size(); ++i) {
    if (i > 0) out << ",";
    auto& b = profile.measured_benchmarks[i];
    out << "\n    {";
    out << "\n      \"prefill_tps\": " << b.prefill_tokens_per_second << ",";
    out << "\n      \"decode_tps\": " << b.decode_tokens_per_second << ",";
    out << "\n      \"first_token_latency_us\": "
        << b.first_token_latency.count() << ",";
    out << "\n      \"peak_memory_bytes\": " << b.peak_memory_bytes;
    out << "\n    }";
  }
  out << "\n  ]\n}\n";

  std::cerr << "Benchmark results written to " << output << std::endl;
  return 0;
}
