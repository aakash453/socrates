#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "socrates/cluster/leader_election.h"
#include "socrates/cluster/membership_service.h"
#include "socrates/discovery/discovery_service.h"
#include "socrates/security/identity_provider.h"

namespace socrates {
namespace {

using namespace cluster;
using namespace discovery;
using namespace security;

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

MembershipSnapshot make_alive_snapshot(
    std::uint64_t revision,
    std::vector<std::string> node_ids) {
  MembershipSnapshot snap;
  snap.revision = revision;
  for (auto& id : node_ids) {
    Member m;
    m.peer.node_id = NodeId{std::move(id)};
    m.state = MemberState::kAlive;
    m.last_seen = std::chrono::system_clock::now();
    snap.members.push_back(std::move(m));
  }
  return snap;
}

TEST(ClusterIntegration, JoinAndAdmitSeenInSnapshot) {
  auto id1 = make_ephemeral_identity_provider();
  ASSERT_TRUE(id1->local_identity().is_ok());

  MembershipSnapshot last_snap;
  int cb_count = 0;
  auto ms = make_membership_service();
  ms->start(*id1, [&](const MembershipSnapshot& s) {
    last_snap = s;
    cb_count++;
  });

  DiscoveryEvent ev;
  ev.kind = DiscoveryEventKind::kFound;
  ev.peer = PeerAdvertisement{NodeId{"peer-1"}, 1, {"host:5001"}, "fp1", {}};
  ms->observe(ev);

  EXPECT_GE(cb_count, 1);
  EXPECT_GE(last_snap.members.size(), 1u);
  EXPECT_GE(last_snap.revision, 1u);

  ms->stop();
}

TEST(ClusterIntegration, MultipleNodesSeenInOrder) {
  auto id1 = make_ephemeral_identity_provider();
  ASSERT_TRUE(id1->local_identity().is_ok());

  MembershipSnapshot last_snap;
  auto ms = make_membership_service();
  ms->start(*id1, [&](const MembershipSnapshot& s) { last_snap = s; });

  DiscoveryEvent ev1, ev2, ev3;
  ev1.peer = PeerAdvertisement{NodeId{"a"}, 1, {}, "fp-a", {}};
  ev2.peer = PeerAdvertisement{NodeId{"b"}, 1, {}, "fp-b", {}};
  ev3.peer = PeerAdvertisement{NodeId{"c"}, 1, {}, "fp-c", {}};

  ms->observe(ev1);
  auto rev1 = last_snap.revision;
  ms->observe(ev2);
  auto rev2 = last_snap.revision;
  ms->observe(ev3);
  auto rev3 = last_snap.revision;

  EXPECT_LT(rev1, rev2);
  EXPECT_LT(rev2, rev3);
  EXPECT_EQ(last_snap.members.size(), 3u);

  ms->stop();
}

TEST(ClusterIntegration, ElectionProducesLeader) {
  MemoryTermStore term_store;
  LeadershipState last_state;

  auto election = make_bully_election();
  election->start(NodeId{"node-low"}, "test-cluster", term_store,
                   [&](const LeadershipState& s) { last_state = s; });

  // Provide a snapshot with two alive members
  auto snap = make_alive_snapshot(1, {"node-low", "node-z-high"});
  election->update_membership(snap);

  EXPECT_TRUE(last_state.leader_id.has_value());
  EXPECT_EQ(last_state.leader_id->value, "node-z-high");
  EXPECT_GE(last_state.fence.term, 1u);
  EXPECT_FALSE(last_state.fence.token.value.empty());

  election->stop();
}

TEST(ClusterIntegration, LeaderLossTriggersNewLeader) {
  MemoryTermStore term_store;

  auto election = make_bully_election();
  election->start(NodeId{"a"}, "test-cluster", term_store,
                   [](const LeadershipState&) {});

  // Initial: 2 nodes — "z-leader" is highest, becomes leader
  election->update_membership(
      make_alive_snapshot(1, {"a", "z-leader"}));
  auto first_term = election->current().fence.term;
  auto first_leader = election->current().leader_id->value;
  EXPECT_EQ(first_leader, "z-leader");
  EXPECT_GE(first_term, 1u);

  // "z-leader" lost entirely — only "a" remains, becomes new leader
  election->update_membership(
      make_alive_snapshot(2, {"a"}));

  auto state2 = election->current();
  EXPECT_TRUE(state2.leader_id.has_value());
  EXPECT_NE(state2.leader_id->value, first_leader);
  EXPECT_GT(state2.fence.term, first_term);

  election->stop();
}

TEST(ClusterIntegration, TermIncreasesMonotonically) {
  MemoryTermStore term_store;
  std::vector<std::uint64_t> terms;

  auto election = make_bully_election();
  election->start(NodeId{"sole-node"}, "test-cluster", term_store,
                   [&](const LeadershipState& s) { terms.push_back(s.fence.term); });

  // First election
  election->update_membership(make_alive_snapshot(1, {"sole-node"}));
  EXPECT_EQ(terms.size(), 1u);

  // Second election: add a higher node, forcing re-election
  election->update_membership(make_alive_snapshot(2, {"sole-node", "z-higher"}));
  EXPECT_EQ(terms.size(), 2u);

  // Remove higher node, sole-node becomes leader again
  election->update_membership(make_alive_snapshot(3, {"sole-node"}));
  EXPECT_GE(terms.size(), 3u);

  EXPECT_LT(terms[0], terms[1]);
  EXPECT_LT(terms[1], terms[2]);

  election->stop();
}

TEST(ClusterIntegration, FencingTokenChangesWithElection) {
  MemoryTermStore term_store;

  auto election = make_bully_election();
  election->start(NodeId{"n1"}, "test-cluster", term_store,
                   [](const LeadershipState&) {});

  // Initial: n1 is the only alive node, becomes leader
  election->update_membership(make_alive_snapshot(1, {"n1"}));
  auto token1 = election->current().fence.token.value;

  // New higher node joins — triggers re-election, new fencing token
  election->update_membership(make_alive_snapshot(2, {"n1", "n2-higher"}));
  auto token2 = election->current().fence.token.value;

  EXPECT_NE(token1, token2);

  // n2 leaves — n1 becomes leader again, another new fencing token
  election->update_membership(make_alive_snapshot(3, {"n1"}));
  auto token3 = election->current().fence.token.value;

  EXPECT_NE(token2, token3);

  election->stop();
}

}  // namespace
}  // namespace socrates
