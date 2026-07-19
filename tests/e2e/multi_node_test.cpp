#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "socrates/cluster/leader_election.h"
#include "socrates/cluster/membership_service.h"
#include "socrates/discovery/discovery_service.h"
#include "socrates/pipeline/inference_pipeline.h"
#include "socrates/scheduler/scheduler.h"
#include "socrates/security/identity_provider.h"

namespace {

using namespace socrates;
using namespace socrates::cluster;
using namespace socrates::discovery;
using namespace socrates::pipeline;
using namespace socrates::scheduler;
using namespace socrates::security;

struct MemStore : ElectionTermStore {
  mutable std::uint64_t v{0};
  Result<std::uint64_t> load_term(const std::string&) const override { return v; }
  Result<bool> store_term(const std::string&, std::uint64_t t) override { v = t; return true; }
};

SchedulingInput make_input(const MembershipSnapshot& snap, const LeadershipFence& fence) {
  SchedulingInput in;
  in.model_id = ModelId{"tinyllama"};
  in.manifest_revision = "1.1b";
  in.total_layers = 22;
  in.policy = SchedulingPolicy::kMemoryBased;
  in.fence = fence;
  in.membership = snap;
  for (int i = 0; i < 2; ++i) {
    auto st = static_cast<std::uint32_t>(i * 11);
    auto en = static_cast<std::uint32_t>(std::min((i + 1) * 11, 22));
    ShardOption sh;
    sh.shard_id = ShardId{"shard-" + std::to_string(i)};
    sh.stage_kind = StageKind::kTransformerLayers;
    sh.layers = LayerRange{st, en};
    sh.quantization = QuantizationKind::kInt4;
    sh.compatible_backends = {BackendKind::kLlamaCpp};
    sh.peak_runtime_memory_bytes = 6ull * 1024 * 1024 * 1024;
    sh.execution_profile_id = "p1";
    in.shard_options.push_back(std::move(sh));
  }
  return in;
}

}  // namespace

int main() {
  const char* node_id = std::getenv("NODE_ID") ? std::getenv("NODE_ID") : "node-a";
  bool is_gpu = std::getenv("NODE_GPU") && std::string(std::getenv("NODE_GPU")) == "1";

  std::cerr << "[" << node_id << "] GPU=" << (is_gpu ? "yes" : "no") << std::endl;

  auto idp = make_ephemeral_identity_provider();
  auto local = idp->local_identity();
  if (local.is_err()) { std::cerr << "id failed" << std::endl; return 1; }

  auto ms = make_membership_service();
  ms->start(*idp, [](const MembershipSnapshot&) {});

  DiscoveryEvent ev;
  ev.kind = DiscoveryEventKind::kFound;
  ev.peer.node_id = NodeId{node_id};
  ev.peer.endpoints = {"127.0.0.1:5000"};
  ev.peer.public_key_fingerprint = std::string("fp-") + node_id;
  ms->observe(ev);

  MemStore ts;
  LeadershipState ls;
  auto el = make_bully_election();
  el->start(NodeId{node_id}, "cluster", ts,
             [&](const LeadershipState& s) { ls = s; });

  cluster::MembershipSnapshot msnap;
  msnap.revision = 1;
  for (const char* n : {"node-a", "node-b", "node-c"}) {
    cluster::Member m;
    m.peer.node_id = NodeId{n};
    m.state = MemberState::kAlive;
    m.last_seen = std::chrono::system_clock::now();
    msnap.members.push_back(m);
  }
  el->update_membership(msnap);

  bool leader = ls.leader_id.has_value() && ls.leader_id->value == node_id;
  std::cerr << "[" << node_id << "] leader="
            << (ls.leader_id.has_value() ? ls.leader_id->value : "none")
            << " term=" << ls.fence.term
            << (leader ? " I_AM_LEADER" : "") << std::endl;

  if (leader) {
    auto in = make_input(msnap, ls.fence);
    auto sched = make_memory_scheduler();
    auto plan = sched->create_plan(in);
    if (plan.is_ok()) {
      auto& p = plan.value();
      std::cerr << "[" << node_id << "] plan " << p.plan_id
                << " stages=" << p.stages.size() << std::endl;
      for (const auto& st : p.stages) {
        std::cerr << "  stage " << st.ordinal << " -> " << st.node_id.value
                  << " layers=[" << (st.layers ? std::to_string(st.layers->start) : "?")
                  << "-" << (st.layers ? std::to_string(st.layers->end_exclusive) : "?")
                  << "] backend=" << static_cast<int>(st.backend) << std::endl;
      }
    } else {
      std::cerr << "schedule failed: " << plan.error().what() << std::endl;
    }
  }

  auto pipe = make_inference_pipeline();
  scheduler::PipelinePlan infp;
  infp.plan_id = "inf-plan";
  infp.model_id = ModelId{"tinyllama"};
  infp.manifest_revision = "1.1b";
  infp.fence = ls.fence;
  infp.issued_at_utc = std::chrono::system_clock::now();
  infp.valid_for = std::chrono::minutes(5);

  scheduler::StageAssignment sa;
  sa.ordinal = 0;
  sa.node_id = NodeId{node_id};
  sa.shard_id = ShardId{"s0"};
  sa.stage_ids = {"s0"};
  sa.stage_kind = StageKind::kTransformerLayers;
  sa.layers = LayerRange{0, 22};
  sa.quantization = QuantizationKind::kInt4;
  sa.backend = BackendKind::kLlamaCpp;
  sa.execution_profile_id = "p1";
  infp.stages.push_back(sa);

  InferenceRequest req;
  req.request_id = RequestId{std::string("r-") + node_id};
  req.session_id = SessionId{"s"};
  req.model_id = ModelId{"tinyllama"};
  req.prompt = "Hello TinyLlama";
  req.generation.maximum_new_tokens = 5;

  int tc = 0;
  auto h = pipe->generate(req, infp, [&](const pipeline::StreamEvent& e) {
    if (e.kind == StreamEventKind::kToken) {
      std::cerr << "[" << node_id << "] t" << e.sequence << ": " << e.text_delta << std::endl;
      tc++;
    } else if (e.kind == StreamEventKind::kCompleted) {
      std::cerr << "[" << node_id << "] done (" << tc << " tokens)" << std::endl;
    }
  }, CancellationToken{});

  if (h.is_err()) {
    std::cerr << "[" << node_id << "] inference FAILED: " << h.error().what() << std::endl;
    return 1;
  }

  el->stop();
  ms->stop();
  return 0;
}
