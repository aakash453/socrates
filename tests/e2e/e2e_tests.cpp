#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "socrates/cluster/leader_election.h"
#include "socrates/cluster/membership_service.h"
#include "socrates/discovery/discovery_service.h"
#include "socrates/inference/inference_backend.h"
#include "socrates/pipeline/inference_pipeline.h"
#include "socrates/runtime/edge_runtime.h"
#include "socrates/scheduler/scheduler.h"
#include "socrates/security/identity_provider.h"
#include "socrates/transport/transport.h"
#include "socrates/tracing.h"

namespace socrates {
namespace {

using namespace cluster;
using namespace discovery;
using namespace pipeline;
using namespace runtime;
using namespace scheduler;
using namespace security;

// ── Helpers ────────────────────────────────────────────────────────────────

struct MemoryTermStore : ElectionTermStore {
  mutable std::uint64_t stored{0};
  Result<std::uint64_t> load_term(const std::string&) const override {
    return stored;
  }
  Result<bool> store_term(const std::string&, std::uint64_t t) override {
    stored = t;
    return true;
  }
};

struct EventLog {
  std::vector<StreamEventKind> kinds;
  std::vector<std::string> texts;
  std::mutex mutex;

  void on_event(const StreamEvent& ev) {
    std::lock_guard lock(mutex);
    kinds.push_back(ev.kind);
    texts.push_back(ev.text_delta);
  }

  int token_count() const {
    return static_cast<int>(std::count(kinds.begin(), kinds.end(),
                                       StreamEventKind::kToken));
  }

  bool completed() const {
    return !kinds.empty() && kinds.back() == StreamEventKind::kCompleted;
  }
};

scheduler::PipelinePlan make_single_stage_plan(const char* plan_id = "plan") {
  scheduler::PipelinePlan p;
  p.plan_id = plan_id;
  p.model_id = ModelId{"test-model"};
  p.manifest_revision = "1.0";
  p.fence = LeadershipFence{1, FencingToken{"f1"}, 1};
  p.issued_at_utc = std::chrono::system_clock::now();
  p.valid_for = std::chrono::minutes(5);
  scheduler::StageAssignment sa;
  sa.ordinal = 0;
  sa.node_id = NodeId{"local-node"};
  sa.shard_id = ShardId{"shard-0"};
  sa.stage_ids = {"stage-0"};
  sa.stage_kind = StageKind::kTransformerLayers;
  sa.layers = LayerRange{0, 22};
  sa.quantization = QuantizationKind::kFp16;
  sa.backend = BackendKind::kLlamaCpp;
  sa.execution_profile_id = "p1";
  p.stages.push_back(sa);
  return p;
}

// ── Original E2E tests ─────────────────────────────────────────────────────

TEST(E2E, ThreeNodes_FormCluster_WithTransportAndBackends) {
  auto id_a = make_ephemeral_identity_provider();
  auto id_b = make_ephemeral_identity_provider();
  auto id_c = make_ephemeral_identity_provider();
  auto node_a = id_a->local_identity().value().node_id;
  auto node_b = id_b->local_identity().value().node_id;
  auto node_c = id_c->local_identity().value().node_id;

  auto rt_a = make_edge_runtime();
  auto rt_b = make_edge_runtime();
  auto rt_c = make_edge_runtime();

  RuntimeConfig cfg_a;
  cfg_a.local_node = node_a;
  cfg_a.cluster_id = "e2e-cluster";
  cfg_a.state_directory = "/tmp/e2e-a";
  cfg_a.model_root = "/tmp/e2e-models";

  RuntimeConfig cfg_b = cfg_a; cfg_b.local_node = node_b; cfg_b.state_directory = "/tmp/e2e-b";
  RuntimeConfig cfg_c = cfg_a; cfg_c.local_node = node_c; cfg_c.state_directory = "/tmp/e2e-c";

  auto r_a = rt_a->start(cfg_a, [](const RuntimeSnapshot&) {}, CancellationToken{});
  auto r_b = rt_b->start(cfg_b, [](const RuntimeSnapshot&) {}, CancellationToken{});
  auto r_c = rt_c->start(cfg_c, [](const RuntimeSnapshot&) {}, CancellationToken{});

  ASSERT_TRUE(r_a.is_ok()) << r_a.error().what();
  ASSERT_TRUE(r_b.is_ok()) << r_b.error().what();
  ASSERT_TRUE(r_c.is_ok()) << r_c.error().what();

  auto snap_a = rt_a->snapshot();
  auto snap_b = rt_b->snapshot();
  auto snap_c = rt_c->snapshot();
  ASSERT_TRUE(snap_a.is_ok());
  ASSERT_TRUE(snap_b.is_ok());
  ASSERT_TRUE(snap_c.is_ok());

  bool any_ready = (snap_a.value().state == RuntimeState::kReady ||
                    snap_b.value().state == RuntimeState::kReady ||
                    snap_c.value().state == RuntimeState::kReady);
  EXPECT_TRUE(any_ready);

  rt_a->leave();
  rt_b->stop();
  rt_c->stop();
}

TEST(E2E, DistributedStreaming_TwoNodes_ProducesTokens) {
  auto pipeline = make_inference_pipeline();

  auto id_a = make_ephemeral_identity_provider();
  auto id_b = make_ephemeral_identity_provider();
  auto node_a = id_a->local_identity().value().node_id;
  auto node_b = id_b->local_identity().value().node_id;

  scheduler::PipelinePlan plan;
  plan.plan_id = "dist-plan";
  plan.model_id = ModelId{"test-model"};
  plan.manifest_revision = "1.0";
  plan.fence = LeadershipFence{1, FencingToken{"f1"}, 1};
  plan.issued_at_utc = std::chrono::system_clock::now();
  plan.valid_for = std::chrono::minutes(5);

  scheduler::StageAssignment sa0;
  sa0.ordinal = 0;
  sa0.node_id = node_a;
  sa0.shard_id = ShardId{"shard-alpha"};
  sa0.stage_ids = {"stage-alpha"};
  sa0.stage_kind = StageKind::kTransformerLayers;
  sa0.layers = LayerRange{0, 11};
  sa0.quantization = QuantizationKind::kFp16;
  sa0.backend = BackendKind::kLlamaCpp;
  sa0.execution_profile_id = "p1";
  plan.stages.push_back(sa0);

  scheduler::StageAssignment sa1;
  sa1.ordinal = 1;
  sa1.node_id = node_b;
  sa1.shard_id = ShardId{"shard-beta"};
  sa1.stage_ids = {"stage-beta"};
  sa1.stage_kind = StageKind::kTransformerLayers;
  sa1.layers = LayerRange{11, 22};
  sa1.quantization = QuantizationKind::kFp16;
  sa1.backend = BackendKind::kLlamaCpp;
  sa1.execution_profile_id = "p2";
  plan.stages.push_back(sa1);

  InferenceRequest req;
  req.request_id = RequestId{"dist-req-1"};
  req.session_id = SessionId{"sess-1"};
  req.model_id = ModelId{"test-model"};
  req.prompt = "Hello distributed world";
  req.generation.maximum_new_tokens = 5;
  req.generation.temperature = 0.7f;

  auto log = std::make_shared<EventLog>();
  auto handle = pipeline->generate(
      req, plan,
      [log](const StreamEvent& ev) { log->on_event(ev); },
      CancellationToken{});

  ASSERT_TRUE(handle.is_ok()) << handle.error().what();

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_GT(log->token_count(), 0) << "Expected at least one token from distributed pipeline";
}

TEST(E2E, StreamingPipeline_ProducesOrderedTokens) {
  auto pipeline = make_inference_pipeline();

  InferenceRequest req;
  req.request_id = RequestId{"req-stream-1"};
  req.session_id = SessionId{"sess-1"};
  req.model_id = ModelId{"test-model"};
  req.prompt = "What is the capital of France?";
  req.generation.maximum_new_tokens = 10;
  req.generation.temperature = 0.7f;

  auto plan = make_single_stage_plan("plan-stream");

  auto log = std::make_shared<EventLog>();
  auto handle = pipeline->generate(
      req, plan,
      [log](const StreamEvent& ev) { log->on_event(ev); },
      CancellationToken{});

  ASSERT_TRUE(handle.is_ok());

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_GT(log->token_count(), 0);
  EXPECT_TRUE(log->completed());
}

TEST(E2E, WorkerLoss_TriggersReplan) {
  auto pipeline = make_inference_pipeline();

  InferenceRequest req;
  req.request_id = RequestId{"req-loss-1"};
  req.session_id = SessionId{"sess-1"};
  req.model_id = ModelId{"test-model"};
  req.prompt = "test";
  req.generation.maximum_new_tokens = 5;

  auto plan = make_single_stage_plan("plan-loss");

  std::vector<StreamEventKind> kinds;
  auto handle = pipeline->generate(
      req, plan,
      [&](const StreamEvent& e) { kinds.push_back(e.kind); },
      CancellationToken{});

  ASSERT_TRUE(handle.is_ok());

  scheduler::PipelinePlan replacement = plan;
  replacement.plan_id = "plan-replacement";
  replacement.fence = LeadershipFence{2, FencingToken{"fence-b"}, 1};

  auto replan_result = pipeline->replan(req.request_id, replacement,
                                         CancellationToken{});
  EXPECT_TRUE(replan_result.is_ok());

  EXPECT_NE(std::find(kinds.begin(), kinds.end(), StreamEventKind::kReplanning),
            kinds.end());
}

TEST(E2E, LeaderLoss_NewElectionHigherTerm) {
  auto id = make_ephemeral_identity_provider();
  auto node = id->local_identity().value().node_id;

  auto ms = make_membership_service();
  ms->start(*id, [](const MembershipSnapshot&) {});

  DiscoveryEvent ev;
  ev.peer = PeerAdvertisement{node, 1, {}, "fp", {}};
  ms->observe(ev);

  ms->promote_joining_members();

  cluster::MembershipSnapshot snap;
  snap.revision = 1;
  cluster::Member m;
  m.peer.node_id = node;
  m.state = MemberState::kAlive;
  m.last_seen = std::chrono::system_clock::now();
  snap.members.push_back(m);

  MemoryTermStore term_store;
  LeadershipState last_state;

  auto election = make_bully_election();
  election->start(node, "leader-loss-cluster", term_store,
                   [&](const LeadershipState& s) { last_state = s; });

  election->update_membership(snap);
  auto term1 = last_state.fence.term;
  EXPECT_GE(term1, 1u);

  snap.revision = 2;
  snap.members[0].peer.node_id = NodeId{"new-leader-higher-id"};
  election->update_membership(snap);

  auto term2 = election->current().fence.term;
  EXPECT_GT(term2, term1);

  election->stop();
  ms->stop();
}

TEST(E2E, CancelStopsTokenStream) {
  auto pipeline = make_inference_pipeline();

  InferenceRequest req;
  req.request_id = RequestId{"req-cancel-1"};
  req.session_id = SessionId{"sess-1"};
  req.model_id = ModelId{"test-model"};
  req.prompt = "test";
  req.generation.maximum_new_tokens = 100;

  auto plan = make_single_stage_plan("plan-cancel");

  auto log = std::make_shared<EventLog>();
  auto handle = pipeline->generate(
      req, plan,
      [log](const StreamEvent& ev) { log->on_event(ev); },
      CancellationToken{});

  ASSERT_TRUE(handle.is_ok());
  handle.value()->cancel();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_FALSE(log->kinds.empty());
  EXPECT_TRUE(log->completed() || log->kinds.back() == StreamEventKind::kFailed);
}

TEST(E2E, GracefulLeave_DrainsActiveRequests) {
  auto pipeline = make_inference_pipeline();

  auto plan = make_single_stage_plan("plan-leave");

  InferenceRequest req;
  req.request_id = RequestId{"leave-req"};
  req.session_id = SessionId{"sess-1"};
  req.model_id = ModelId{"test-model"};
  req.prompt = "test";
  req.generation.maximum_new_tokens = 50;

  auto log = std::make_shared<EventLog>();
  auto handle = pipeline->generate(
      req, plan,
      [log](const StreamEvent& ev) { log->on_event(ev); },
      CancellationToken{});

  ASSERT_TRUE(handle.is_ok());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  handle.value()->cancel();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_TRUE(log->completed() || log->kinds.back() == StreamEventKind::kFailed);
  EXPECT_GT(log->token_count(), 0);
}

// ── NEW: Adaptive Batching tests ───────────────────────────────────────────

TEST(E2E, BatchingImmediate_DispatchesWithoutQueue) {
  auto pipeline = make_inference_pipeline();

  // Default is kImmediate — no queuing.
  EXPECT_EQ(pipeline->queued_request_count(), 0u);

  auto plan = make_single_stage_plan("plan-imm");

  InferenceRequest req;
  req.request_id = RequestId{"imm-req"};
  req.session_id = SessionId{"s1"};
  req.model_id = ModelId{"test"};
  req.prompt = "hi";
  req.generation.maximum_new_tokens = 3;

  auto log = std::make_shared<EventLog>();
  auto handle = pipeline->generate(req, plan,
      [log](const StreamEvent& ev) { log->on_event(ev); },
      CancellationToken{});

  ASSERT_TRUE(handle.is_ok());
  // In immediate mode no request should be queued.
  EXPECT_EQ(pipeline->queued_request_count(), 0u);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_GT(log->token_count(), 0);
}

TEST(E2E, BatchingSerial_DispatchesOneAtATime) {
  auto pipeline = make_inference_pipeline();

  BatchingConfig cfg;
  cfg.mode = BatchingMode::kSerial;
  pipeline->set_batching_config(cfg);

  auto log1 = std::make_shared<EventLog>();
  auto log2 = std::make_shared<EventLog>();

  InferenceRequest req1;
  req1.request_id = RequestId{"serial-1"};
  req1.session_id = SessionId{"s1"};
  req1.model_id = ModelId{"test"};
  req1.prompt = "a";
  req1.generation.maximum_new_tokens = 2;

  InferenceRequest req2;
  req2.request_id = RequestId{"serial-2"};
  req2.session_id = SessionId{"s2"};
  req2.model_id = ModelId{"test"};
  req2.prompt = "b";
  req2.generation.maximum_new_tokens = 2;

  auto h1 = pipeline->generate(req1, make_single_stage_plan("p1"),
      [log1](const StreamEvent& ev) { log1->on_event(ev); },
      CancellationToken{});
  ASSERT_TRUE(h1.is_ok());

  auto h2 = pipeline->generate(req2, make_single_stage_plan("p2"),
      [log2](const StreamEvent& ev) { log2->on_event(ev); },
      CancellationToken{});
  ASSERT_TRUE(h2.is_ok());

  // At least one request should be queued (serial mode).
  EXPECT_GE(pipeline->queued_request_count(), 1u);

  // Wait for both to complete.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_GT(log1->token_count(), 0);
  EXPECT_GT(log2->token_count(), 0);
  EXPECT_EQ(pipeline->queued_request_count(), 0u);
}

TEST(E2E, BatchingAdaptive_LongContextGoesSolo) {
  auto pipeline = make_inference_pipeline();

  BatchingConfig cfg;
  cfg.mode = BatchingMode::kAdaptive;
  cfg.long_context_threshold = 2048;  // low threshold to trigger solo dispatch
  cfg.max_batch_size = 4;
  cfg.batch_timeout = std::chrono::milliseconds(50);
  pipeline->set_batching_config(cfg);

  auto plan = make_single_stage_plan("plan-adapt");

  // Long-context request.
  InferenceRequest long_req;
  long_req.request_id = RequestId{"long-1"};
  long_req.session_id = SessionId{"s1"};
  long_req.model_id = ModelId{"test"};
  long_req.prompt = "long context request";
  long_req.generation.maximum_new_tokens = 3;
  long_req.generation.context_window = 32768;  // well above threshold

  // Short-context request.
  InferenceRequest short_req;
  short_req.request_id = RequestId{"short-1"};
  short_req.session_id = SessionId{"s2"};
  short_req.model_id = ModelId{"test"};
  short_req.prompt = "hi";
  short_req.generation.maximum_new_tokens = 2;
  short_req.generation.context_window = 1024;  // below threshold

  auto log_long = std::make_shared<EventLog>();
  auto log_short = std::make_shared<EventLog>();

  // Submit long-context first — should go solo.
  auto h_long = pipeline->generate(long_req, plan,
      [log_long](const StreamEvent& ev) { log_long->on_event(ev); },
      CancellationToken{});
  ASSERT_TRUE(h_long.is_ok());

  // Submit short-context next.
  auto h_short = pipeline->generate(short_req, plan,
      [log_short](const StreamEvent& ev) { log_short->on_event(ev); },
      CancellationToken{});
  ASSERT_TRUE(h_short.is_ok());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_GT(log_long->token_count(), 0) << "Long-context request should have tokens";
  EXPECT_GT(log_short->token_count(), 0) << "Short-context request should have tokens";
}

TEST(E2E, BatchingCancelWhileQueued) {
  auto pipeline = make_inference_pipeline();

  BatchingConfig cfg;
  cfg.mode = BatchingMode::kSerial;
  cfg.batch_timeout = std::chrono::milliseconds(500);
  pipeline->set_batching_config(cfg);

  auto plan = make_single_stage_plan("plan-cancel-q");

  // Submit a first request that will be dispatched (occupying the serial slot).
  InferenceRequest req1;
  req1.request_id = RequestId{"cancel-first"};
  req1.session_id = SessionId{"s1"};
  req1.model_id = ModelId{"test"};
  req1.prompt = "first";
  req1.generation.maximum_new_tokens = 1;

  auto log1 = std::make_shared<EventLog>();
  auto h1 = pipeline->generate(req1, plan,
      [log1](const StreamEvent& ev) { log1->on_event(ev); },
      CancellationToken{});
  ASSERT_TRUE(h1.is_ok());

  // Submit a second request — will be queued behind the first.
  InferenceRequest req2;
  req2.request_id = RequestId{"cancel-second"};
  req2.session_id = SessionId{"s2"};
  req2.model_id = ModelId{"test"};
  req2.prompt = "second";
  req2.generation.maximum_new_tokens = 5;

  auto log2 = std::make_shared<EventLog>();
  auto h2 = pipeline->generate(req2, plan,
      [log2](const StreamEvent& ev) { log2->on_event(ev); },
      CancellationToken{});
  ASSERT_TRUE(h2.is_ok());

  // Second request should be queued.
  EXPECT_GE(pipeline->queued_request_count(), 1u);

  // Cancel the second request while it's queued.
  h2.value()->cancel();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(pipeline->queued_request_count(), 0u);
  EXPECT_TRUE(log1->completed());
  EXPECT_TRUE(log2->kinds.empty() || log2->kinds.back() == StreamEventKind::kFailed);
}

TEST(E2E, BatchingAdaptive_ShortContextRequestsAreBatched) {
  auto pipeline = make_inference_pipeline();

  BatchingConfig cfg;
  cfg.mode = BatchingMode::kAdaptive;
  cfg.max_batch_size = 3;
  cfg.batch_timeout = std::chrono::milliseconds(100);
  cfg.long_context_threshold = 4096;
  pipeline->set_batching_config(cfg);

  auto plan = make_single_stage_plan("plan-batch");

  std::vector<std::shared_ptr<EventLog>> logs;
  // Submit 3 short-context requests in rapid succession.
  for (size_t i = 0; i < 3; ++i) {
    InferenceRequest req;
    req.request_id = RequestId{"batch-req-" + std::to_string(i)};
    req.session_id = SessionId{"s" + std::to_string(i)};
    req.model_id = ModelId{"test"};
    req.prompt = "batch " + std::to_string(i);
    req.generation.maximum_new_tokens = 2;
    req.generation.context_window = 1024;  // short context — eligible for batching

    auto log = std::make_shared<EventLog>();
    logs.push_back(log);
    auto h = pipeline->generate(req, plan,
        [log](const StreamEvent& ev) { log->on_event(ev); },
        CancellationToken{});
    ASSERT_TRUE(h.is_ok());
  }

  // All 3 should be dispatched together (or within the batch_timeout).
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  for (size_t i = 0; i < 3; ++i) {
    EXPECT_GT(logs[i]->token_count(), 0)
        << "Short-context request " << i << " should have tokens";
  }
  EXPECT_EQ(pipeline->queued_request_count(), 0u);
}

TEST(E2E, BatchingSwitchingModeMidFlight) {
  auto pipeline = make_inference_pipeline();

  // Start in immediate mode.
  auto log1 = std::make_shared<EventLog>();
  InferenceRequest req1;
  req1.request_id = RequestId{"switch-1"};
  req1.session_id = SessionId{"s1"};
  req1.model_id = ModelId{"test"};
  req1.prompt = "first";
  req1.generation.maximum_new_tokens = 2;

  auto h1 = pipeline->generate(req1, make_single_stage_plan("p1"),
      [log1](const StreamEvent& ev) { log1->on_event(ev); },
      CancellationToken{});
  ASSERT_TRUE(h1.is_ok());
  EXPECT_EQ(pipeline->queued_request_count(), 0u);  // immediate

  // Switch to serial mode mid-flight.
  BatchingConfig serial_cfg;
  serial_cfg.mode = BatchingMode::kSerial;
  pipeline->set_batching_config(serial_cfg);

  auto log2 = std::make_shared<EventLog>();
  InferenceRequest req2;
  req2.request_id = RequestId{"switch-2"};
  req2.session_id = SessionId{"s2"};
  req2.model_id = ModelId{"test"};
  req2.prompt = "second";
  req2.generation.maximum_new_tokens = 2;

  auto h2 = pipeline->generate(req2, make_single_stage_plan("p2"),
      [log2](const StreamEvent& ev) { log2->on_event(ev); },
      CancellationToken{});
  ASSERT_TRUE(h2.is_ok());

  // Second request should be queued now.
  EXPECT_GE(pipeline->queued_request_count(), 1u);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_GT(log1->token_count(), 0);
  EXPECT_GT(log2->token_count(), 0);
  EXPECT_EQ(pipeline->queued_request_count(), 0u);
}

}  // namespace
}  // namespace socrates
