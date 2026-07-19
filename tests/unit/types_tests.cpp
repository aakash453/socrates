#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "socrates/types.h"
#include "socrates/scheduler/scheduler.h"

namespace socrates {
namespace {

TEST(LayerRange, ValidRange_ReturnsTrue) {
  LayerRange r{0, 10};
  EXPECT_TRUE(r.valid());
  EXPECT_EQ(r.count(), 10u);
}

TEST(LayerRange, EmptyRange_ReturnsFalse) {
  LayerRange r{5, 5};
  EXPECT_FALSE(r.valid());
  EXPECT_EQ(r.count(), 0u);
}

TEST(LayerRange, ZeroInit_Invalid) {
  LayerRange r;
  EXPECT_FALSE(r.valid());
}

TEST(LeadershipFence, NewerThan_DetectsHigherTerm) {
  LeadershipFence a{1, FencingToken{"abc"}, 3};
  LeadershipFence b{2, FencingToken{"xyz"}, 5};
  EXPECT_TRUE(b.newer_than(a));
  EXPECT_FALSE(a.newer_than(b));
}

TEST(LeadershipFence, Equality_ChecksAllFields) {
  LeadershipFence a{1, FencingToken{"abc"}, 3};
  LeadershipFence b{1, FencingToken{"abc"}, 3};
  LeadershipFence c{1, FencingToken{"abc"}, 4};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(StrongId, EqualityWorks) {
  NodeId a{"node-a"};
  NodeId b{"node-a"};
  NodeId c{"node-b"};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(StrongId, EmptyChecks) {
  NodeId empty;
  NodeId filled{"x"};
  EXPECT_TRUE(empty.empty());
  EXPECT_FALSE(filled.empty());
}

TEST(StrongId, StringViewAccess) {
  RequestId r{"req-1"};
  EXPECT_EQ(r.sv(), "req-1");
}

TEST(StrongId, OrderingWorks) {
  ShardId a{"shard-1"};
  ShardId b{"shard-2"};
  EXPECT_LT(a, b);
  EXPECT_FALSE(b < a);
}

TEST(CapabilityProfile, NotExpired_ImmediatelyAfterCreation) {
  CapabilityProfile p;
  p.received_at = Clock::now();
  p.valid_for = std::chrono::milliseconds(5000);
  EXPECT_FALSE(p.expired());
}

TEST(CapabilityProfile, Expired_AfterDurationPasses) {
  CapabilityProfile p;
  p.received_at = Clock::now();
  p.valid_for = std::chrono::milliseconds(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(p.expired());
}

TEST(SchedulerPipelinePlan, StagesConstructed) {
  scheduler::PipelinePlan plan;
  EXPECT_TRUE(plan.stages.empty());
  scheduler::StageAssignment sa;
  sa.ordinal = 0;
  plan.stages.push_back(sa);
  EXPECT_EQ(plan.stages.size(), 1u);
}

TEST(SchedulerPipelinePlan, ValidityTiming) {
  scheduler::PipelinePlan plan;
  plan.issued_at_utc = std::chrono::system_clock::now();
  plan.valid_for = std::chrono::minutes(5);
  auto expires_at = plan.issued_at_utc + plan.valid_for;
  EXPECT_GT(expires_at, plan.issued_at_utc);
}

TEST(NetworkMetrics, Valid_WhenLatencyNonZero) {
  NetworkMetrics m;
  EXPECT_FALSE(m.valid());
  m.round_trip_latency = std::chrono::microseconds(1);
  EXPECT_TRUE(m.valid());
}

TEST(TensorShape, DefaultIsUnbounded) {
  TensorShape s;
  EXPECT_FALSE(s.dynamic_min.has_value());
  EXPECT_FALSE(s.dynamic_max.has_value());
  EXPECT_EQ(s.alignment, 0);
}

TEST(QuantizationIdentity, DefaultIsFp16PerTensor) {
  QuantizationIdentity q;
  EXPECT_EQ(q.kind, QuantizationKind::kFp16);
  EXPECT_EQ(q.scheme, QuantizationScheme::kPerTensor);
  EXPECT_EQ(q.activation, QuantizationActivation::kFp16);
  EXPECT_EQ(q.group_size, 0u);
}

}  // namespace
}  // namespace socrates
