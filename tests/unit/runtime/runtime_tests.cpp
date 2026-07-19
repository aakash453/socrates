#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include "socrates/runtime/edge_runtime.h"

namespace socrates {
namespace {

using namespace runtime;

TEST(RuntimeLifecycle, StartStopBasic) {
  auto rt = make_edge_runtime();
  RuntimeConfig cfg;
  cfg.local_node = NodeId{"test-node"};
  cfg.cluster_id = "test-cluster";
  cfg.state_directory = "/tmp/socrates-test";
  cfg.model_root = "/tmp/socrates-models";
  auto result = rt->start(cfg, [](const RuntimeSnapshot&) {}, CancellationToken{});
  ASSERT_TRUE(result.is_ok());
  EXPECT_NO_THROW(rt->stop());
}

TEST(RuntimeLifecycle, DoubleStopIsIdempotent) {
  auto rt = make_edge_runtime();
  RuntimeConfig cfg;
  cfg.local_node = NodeId{"test-node"};
  cfg.cluster_id = "test-cluster";
  cfg.state_directory = "/tmp/socrates-test";
  cfg.model_root = "/tmp/socrates-models";
  rt->start(cfg, [](const RuntimeSnapshot&) {}, CancellationToken{});
  rt->stop();
  EXPECT_NO_THROW(rt->stop());
}

TEST(RuntimeLifecycle, DoubleStartFails) {
  auto rt = make_edge_runtime();
  RuntimeConfig cfg;
  cfg.local_node = NodeId{"test-node"};
  cfg.cluster_id = "test-cluster";
  cfg.state_directory = "/tmp/socrates-test";
  cfg.model_root = "/tmp/socrates-models";
  auto r1 = rt->start(cfg, [](const RuntimeSnapshot&) {}, CancellationToken{});
  ASSERT_TRUE(r1.is_ok());
  auto r2 = rt->start(cfg, [](const RuntimeSnapshot&) {}, CancellationToken{});
  EXPECT_TRUE(r2.is_err());
  rt->stop();
}

TEST(RuntimeLifecycle, SnapshotReflectsReadyState) {
  auto rt = make_edge_runtime();
  RuntimeConfig cfg;
  cfg.local_node = NodeId{"test-node"};
  cfg.cluster_id = "test-cluster";
  cfg.state_directory = "/tmp/socrates-test";
  cfg.model_root = "/tmp/socrates-models";
  rt->start(cfg, [](const RuntimeSnapshot&) {}, CancellationToken{});
  auto snap = rt->snapshot();
  ASSERT_TRUE(snap.is_ok());
  EXPECT_EQ(snap.value().state, runtime::RuntimeState::kReady);
  rt->stop();
}

TEST(RuntimeLifecycle, SnapshotFailsWhenStopped) {
  auto rt = make_edge_runtime();
  auto snap = rt->snapshot();
  EXPECT_TRUE(snap.is_err());
}

TEST(RuntimeLifecycle, GenerateFailsWhenStopped) {
  auto rt = make_edge_runtime();
  pipeline::InferenceRequest req;
  req.request_id = RequestId{"req-1"};
  req.session_id = SessionId{"sess-1"};
  req.model_id = ModelId{"test"};
  req.prompt = "hello";
  req.generation.maximum_new_tokens = 5;
  auto result = rt->generate(req, [](const pipeline::StreamEvent&) {}, CancellationToken{});
  EXPECT_TRUE(result.is_err());
}

}  // namespace
}  // namespace socrates
