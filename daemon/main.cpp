#include <csignal>
#include <cstdlib>
#include <iostream>
#include "socrates/cancellation.h"
#include <thread>

#include "socrates/runtime/edge_runtime.h"

namespace {

std::unique_ptr<socrates::runtime::EdgeRuntime> g_runtime;
std::atomic<bool> g_shutdown{false};

void signal_handler(int sig) {
  std::cerr << "Received signal " << sig << ", shutting down..." << std::endl;
  g_shutdown = true;
  if (g_runtime) g_runtime->stop();
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  g_runtime = socrates::runtime::make_edge_runtime();

  socrates::runtime::RuntimeConfig config;
  config.local_node = socrates::NodeId{"daemon-node"};
  config.cluster_id = "socrates-cluster";
  config.state_directory = "/var/lib/socrates";
  config.model_root = "/var/lib/socrates/models";

  auto result = g_runtime->start(
      config, [](const socrates::runtime::RuntimeSnapshot& snap) {
        std::cerr << "Runtime state: "
                  << static_cast<int>(snap.state) << std::endl;
      },
      CancellationToken{});

  if (result.is_err()) {
    std::cerr << "Failed to start runtime: " << result.error().what() << std::endl;
    return 1;
  }

  std::cerr << "Edge AI Runtime daemon started." << std::endl;

  while (!g_shutdown) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  g_runtime->stop();
  g_runtime.reset();

  return 0;
}
