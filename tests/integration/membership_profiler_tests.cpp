#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "socrates/cluster/membership_service.h"
#include "socrates/discovery/discovery_service.h"
#include "socrates/profiler/capability_profiler.h"
#include "socrates/security/identity_provider.h"

namespace socrates {
namespace {

TEST(CapabilityIntegration, ProfileRequestReturnsValidData) {
  auto id = security::make_ephemeral_identity_provider();
  ASSERT_TRUE(id->local_identity().is_ok());
  auto node_id = id->local_identity().value().node_id;

  auto profiler = profiler::make_capability_profiler();
  auto result = profiler->inspect(node_id, CancellationToken{});
  ASSERT_TRUE(result.is_ok());

  auto profile = result.value();
  EXPECT_EQ(profile.node_id.value, node_id.value);
  EXPECT_GT(profile.total_memory_bytes, 0u);
  EXPECT_GT(profile.available_memory_bytes, 0u);
  EXPECT_LE(profile.available_memory_bytes, profile.total_memory_bytes);
  EXPECT_FALSE(profile.cpu_model.empty());
  EXPECT_GT(profile.logical_cpu_count, 0u);
  EXPECT_FALSE(profile.backends.empty());
  EXPECT_FALSE(profile.expired());
}

TEST(CapabilityIntegration, ProfileExpiresAfterValidityLapses) {
  CapabilityProfile p;
  p.node_id = NodeId{"n1"};
  p.received_at = Clock::now();
  p.valid_for = std::chrono::milliseconds(1);

  EXPECT_FALSE(p.expired());
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(p.expired());
}

TEST(CapabilityIntegration, NetworkMeasurementIsPopulated) {
  auto profiler = profiler::make_capability_profiler();
  auto deadline = Clock::now() + std::chrono::seconds(10);

  auto result = profiler->measure_network("localhost:5000", deadline,
                                           CancellationToken{});
  ASSERT_TRUE(result.is_ok());
  auto m = result.value();
  EXPECT_GT(m.round_trip_latency.count(), 0);
  EXPECT_GT(m.available_bandwidth_bytes_per_second, 0u);
  EXPECT_TRUE(m.valid());
}

TEST(CapabilityIntegration, BenchmarkProducesHigherRevision) {
  auto id = security::make_ephemeral_identity_provider();
  ASSERT_TRUE(id->local_identity().is_ok());
  auto node_id = id->local_identity().value().node_id;

  auto profiler = profiler::make_capability_profiler();
  auto base = profiler->inspect(node_id, CancellationToken{});
  ASSERT_TRUE(base.is_ok());

  profiler::BenchmarkConfig cfg;
  cfg.warmup_runs = 1;
  cfg.measured_runs = 3;
  cfg.maximum_memory_bytes = 4ull * 1024 * 1024 * 1024;
  cfg.deadline = Clock::now() + std::chrono::seconds(30);

  auto bench = profiler->benchmark(base.value(), cfg, CancellationToken{});
  ASSERT_TRUE(bench.is_ok());

  EXPECT_GT(bench.value().revision, base.value().revision);
  EXPECT_FALSE(bench.value().measured_benchmarks.empty());
}

}  // namespace
}  // namespace socrates
