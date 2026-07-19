#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "socrates/scheduler/scheduler.h"

namespace socrates {
namespace {

using namespace scheduler;

SchedulingInput make_input(int num_nodes, int num_layers,
                            int num_shards, SchedulingPolicy policy) {
  SchedulingInput input;
  input.model_id = ModelId{"test-model"};
  input.manifest_revision = "1.0";
  input.total_layers = static_cast<std::uint32_t>(num_layers);
  input.policy = policy;
  input.fence = LeadershipFence{1, FencingToken{"test-fence"}, 1};

  for (int i = 0; i < num_nodes; ++i) {
    cluster::Member m;
    m.peer.node_id = NodeId{"node-" + std::to_string(i)};
        m.state = cluster::MemberState::kAlive;
        m.capability = CapabilityProfile{};
        m.capability->available_memory_bytes = 8ull * 1024 * 1024 * 1024;  // 8 GB
        m.capability->backends.push_back(BackendCapability{
            BackendKind::kLlamaCpp, "0.3.0",
            {QuantizationIdentity{QuantizationKind::kFp16}},
            {ComputeUnit::kCpu}, true});
        input.membership.members.push_back(m);
  }

  int layers_per_shard = std::max(1, num_layers / std::max(1, num_shards));
  for (int i = 0; i < num_shards; ++i) {
    ShardOption shard;
    shard.shard_id = ShardId{"shard-" + std::to_string(i)};
    shard.stage_kind = StageKind::kTransformerLayers;
    auto end = (i == num_shards - 1) 
               ? num_layers 
               : std::min((i + 1) * layers_per_shard, num_layers);
    shard.layers = LayerRange{
        static_cast<std::uint32_t>(i * layers_per_shard),
        static_cast<std::uint32_t>(end),
    };
    shard.quantization = QuantizationKind::kFp16;
    shard.compatible_backends = {BackendKind::kLlamaCpp};
    shard.peak_runtime_memory_bytes = 100000000;
    shard.execution_profile_id = "profile-1";
    input.shard_options.push_back(std::move(shard));
  }
  return input;
}

TEST(SchedulerProperty, CompleteLayerCoverage) {
  std::mt19937 rng(42);
  for (int trial = 0; trial < 20; ++trial) {
    int nodes = 2 + (rng() % 4);
    int layers = 10 + (rng() % 30);
    int shards = 2 + (rng() % 5);

    auto input = make_input(nodes, layers, shards, SchedulingPolicy::kMemoryBased);
    auto scheduler = make_memory_scheduler();
    auto result = scheduler->create_plan(input);

    ASSERT_TRUE(result.is_ok()) << "Trial " << trial << " failed";
    auto& plan = result.value();

    // Check all layers covered
    std::vector<bool> covered(static_cast<std::size_t>(layers), false);
    for (const auto& stage : plan.stages) {
      if (stage.layers.has_value()) {
        for (std::uint32_t l = stage.layers->start;
             l < stage.layers->end_exclusive && static_cast<int>(l) < layers; ++l) {
          covered[l] = true;
        }
      }
    }
    for (std::size_t l = 0; l < static_cast<std::size_t>(layers); ++l) {
      EXPECT_TRUE(covered[l])
          << "Layer " << l << " not covered in trial " << trial;
    }
  }
}

TEST(SchedulerProperty, NoLayerOverlap) {
  auto input = make_input(3, 22, 4, SchedulingPolicy::kMemoryBased);
  auto scheduler = make_memory_scheduler();
  auto result = scheduler->create_plan(input);
  ASSERT_TRUE(result.is_ok());

  auto& plan = result.value();
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
  for (const auto& stage : plan.stages) {
    if (stage.layers.has_value()) {
      ranges.emplace_back(stage.layers->start, stage.layers->end_exclusive);
    }
  }

  std::sort(ranges.begin(), ranges.end());
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    EXPECT_GE(ranges[i].first, ranges[i - 1].second)
        << "Overlap at [" << ranges[i - 1].first << ","
        << ranges[i - 1].second << ") and [" << ranges[i].first << ","
        << ranges[i].second << ")";
  }
}

TEST(SchedulerProperty, EveryStageAssignedNode) {
  auto input = make_input(2, 16, 3, SchedulingPolicy::kMemoryBased);
  auto scheduler = make_memory_scheduler();
  auto result = scheduler->create_plan(input);
  ASSERT_TRUE(result.is_ok());

  for (const auto& stage : result.value().stages) {
    EXPECT_FALSE(stage.node_id.empty());
    EXPECT_FALSE(stage.shard_id.empty());
    EXPECT_NE(stage.backend, BackendKind::kExecuTorchQnn);  // must not be UNSPECIFIED-like
  }
}

TEST(SchedulerProperty, DeterministicOutputForSameInput) {
  auto input = make_input(4, 20, 5, SchedulingPolicy::kMemoryBased);
  auto scheduler = make_memory_scheduler();

  auto r1 = scheduler->create_plan(input);
  auto r2 = scheduler->create_plan(input);

  ASSERT_TRUE(r1.is_ok());
  ASSERT_TRUE(r2.is_ok());

  EXPECT_EQ(r1.value().stages.size(), r2.value().stages.size());
  for (std::size_t i = 0; i < r1.value().stages.size(); ++i) {
    EXPECT_EQ(r1.value().stages[i].ordinal, r2.value().stages[i].ordinal);
    EXPECT_EQ(r1.value().stages[i].node_id.value,
              r2.value().stages[i].node_id.value);
    EXPECT_EQ(r1.value().stages[i].shard_id.value,
              r2.value().stages[i].shard_id.value);
  }
}

TEST(SchedulerProperty, StaleFenceProducesNewPlanId) {
  auto input = make_input(2, 12, 2, SchedulingPolicy::kMemoryBased);
  auto scheduler = make_memory_scheduler();

  auto r1 = scheduler->create_plan(input);
  ASSERT_TRUE(r1.is_ok());
  auto plan1_id = r1.value().plan_id;

  input.fence.term = 2;
  input.fence.token = FencingToken{"new-fence"};

  auto r2 = scheduler->create_plan(input);
  ASSERT_TRUE(r2.is_ok());
  auto plan2_id = r2.value().plan_id;

  EXPECT_NE(plan1_id, plan2_id);
}

TEST(SchedulerProperty, EmptyShardOptionsFails) {
  SchedulingInput input;
  input.model_id = ModelId{"test"};
  input.manifest_revision = "1.0";
  input.fence = LeadershipFence{1, FencingToken{"f"}, 1};
  input.membership.members.push_back(
      {discovery::PeerAdvertisement{NodeId{"n"}, 1, {}, "fp", {}},
       cluster::MemberState::kAlive, cluster::NodeRole::kParticipant, {}, {}, std::chrono::system_clock::now()});

  auto scheduler = make_memory_scheduler();
  auto result = scheduler->create_plan(input);
  EXPECT_TRUE(result.is_err());
}

TEST(SchedulerProperty, EmptyMembershipFails) {
  auto input = make_input(0, 12, 2, SchedulingPolicy::kMemoryBased);
  input.membership.members.clear();

  auto scheduler = make_memory_scheduler();
  auto result = scheduler->create_plan(input);
  EXPECT_TRUE(result.is_err());
}

}  // namespace
}  // namespace socrates
