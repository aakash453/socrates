// include/socrates/cluster/leader_election.h
#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "socrates/cluster/membership_service.h"

namespace socrates::cluster {

struct LeadershipState {
  std::optional<NodeId> leader_id;
  LeadershipFence fence;
  Deadline locally_valid_until;
};

using LeadershipCallback = std::function<void(const LeadershipState&)>;

class ElectionTermStore {
 public:
  virtual ~ElectionTermStore() = default;

  /**
   * Loads the last durable election term for the cluster identity.
   * Preconditions: cluster_id is non-empty.
   * Postconditions: absent state returns zero; returned terms never regress.
   * Throws: no operational exceptions; Result reports durable-store failures.
   * Thread safety: safe concurrently with store_term().
   * Side effects: reads local durable state.
   */
  [[nodiscard]] virtual Result<std::uint64_t> load_term(
      const std::string& cluster_id) const = 0;

  /**
   * Durably stores a strictly greater term before it is announced.
   * Preconditions: term exceeds the last stored term.
   * Postconditions: success survives process restart before callers publish it.
   * Throws: no operational exceptions.
   * Thread safety: writes are serialized.
   * Side effects: synchronously writes local durable state.
   */
  virtual Result<bool> store_term(const std::string& cluster_id,
                                  std::uint64_t term) = 0;
};

class LeaderElection {
 public:
  virtual ~LeaderElection() = default;

  /**
   * Starts election processing using membership snapshots.
   * Preconditions: local node is in membership and callback is non-empty.
   * Postconditions: every announced leader has a monotonic term and opaque
   * fencing token; absence of a leader is represented explicitly.
   * Throws: RuntimeError(kInvalidArgument) or kFailedPrecondition.
   * Thread safety: may receive membership updates concurrently; callback calls
   * are serialized and do not hold election locks.
   * Side effects: sends election/heartbeat messages and persists term state.
   */
  virtual void start(NodeId local_node,
                     std::string cluster_id,
                     ElectionTermStore& term_store,
                     LeadershipCallback callback) = 0;

  /**
   * Supplies a complete membership snapshot to the election state machine.
   * Preconditions: snapshot revision is not older than the last accepted one.
   * Postconditions: eligible membership changes eventually trigger convergence.
   * Throws: RuntimeError(kDataLoss) on revision rollback.
   * Thread safety: safe to call from a membership callback thread.
   * Side effects: may begin a new election round.
   */
  virtual void update_membership(const MembershipSnapshot& snapshot) = 0;

  /**
   * Returns the currently observed leader and fence.
   * Preconditions: service is started.
   * Postconditions: result is a point-in-time copy.
   * Throws: RuntimeError(kFailedPrecondition) when stopped.
   * Thread safety: safe concurrently with election callbacks.
   * Side effects: none.
   */
  [[nodiscard]] virtual LeadershipState current() const = 0;

  /**
   * Relinquishes leadership, if held, and stops election processing.
   * Preconditions: none.
   * Postconditions: leases are not renewed and callbacks are quiesced.
   * Throws: never.
   * Thread safety: idempotent.
   * Side effects: may broadcast resignation and persist the final term.
   */
  virtual void stop() noexcept = 0;
};

std::unique_ptr<LeaderElection> make_bully_election();

}  // namespace socrates::cluster
