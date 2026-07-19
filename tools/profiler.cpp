// tools/profiler.cpp
// Standalone capability profiler CLI.

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>

#include "socrates/profiler/capability_profiler.h"
#include "socrates/cancellation.h"

int main(int argc, char* argv[]) {
  std::string output_path = "capability.json";
  if (argc > 1) output_path = argv[1];

  auto profiler = socrates::profiler::make_capability_profiler();
  auto result = profiler->inspect(socrates::NodeId{"cli-node"}, socrates::CancellationToken{});

  if (result.is_err()) {
    std::cerr << "Profiler failed: " << result.error().what() << std::endl;
    return 1;
  }

  auto profile = result.value();
  std::ofstream out(output_path);
  out << "{\n";
  out << "  \"node_id\": \"" << profile.node_id.value << "\",\n";
  out << "  \"cpu_model\": \"" << profile.cpu_model << "\",\n";
  out << "  \"logical_cpu_count\": " << profile.logical_cpu_count << ",\n";
  out << "  \"total_memory_bytes\": " << profile.total_memory_bytes << ",\n";
  out << "  \"available_memory_bytes\": " << profile.available_memory_bytes << ",\n";
  out << "  \"accelerators\": [";
  for (std::size_t i = 0; i < profile.accelerators.size(); ++i) {
    if (i > 0) out << ",";
    out << "\n    \"" << profile.accelerators[i] << "\"";
  }
  out << "\n  ],\n";
  out << "  \"backends\": [";
  for (std::size_t i = 0; i < profile.backends.size(); ++i) {
    if (i > 0) out << ",";
    out << "\n    { \"kind\": " << static_cast<int>(profile.backends[i].kind)
        << ", \"version\": \"" << profile.backends[i].version << "\" }";
  }
  out << "\n  ]\n}\n";

  std::cerr << "Capability profile written to " << output_path << std::endl;
  return 0;
}
