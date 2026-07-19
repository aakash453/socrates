#include "socrates/cluster/leader_election.h"

#include <algorithm>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

namespace socrates::cluster {

namespace {
std::string make_fencing_token() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::ostringstream os;
  os << "fence-" << std::hex << rng();
  return os.str();
}
}  // namespace

class BullyElection final : public LeaderElection {
 public:
  void start(NodeId local_node, std::string cluster_id,
             ElectionTermStore& term_store, LeadershipCallback callback) override {
    std::lock_guard lock(mutex_);
    if (running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition, "election already started");
    }
    local_node_ = std::move(local_node);
    cluster_id_ = std::move(cluster_id);
    term_store_ = &term_store;
    callback_ = std::move(callback);
    running_ = true;

    auto term_result = term_store_->load_term(cluster_id_);
    if (term_result.is_ok()) {
      current_term_ = term_result.value();
    }
  }

  void update_membership(const MembershipSnapshot& snapshot) override {
    std::lock_guard lock(mutex_);
    if (!running_) return;

    if (snapshot.revision < last_snapshot_revision_) {
      throw RuntimeError(ErrorCode::kDataLoss, "membership revision rollback");
    }
    last_snapshot_revision_ = snapshot.revision;

    std::vector<NodeId> alive;
    for (const auto& m : snapshot.members) {
      if (m.state == MemberState::kAlive) alive.push_back(m.peer.node_id);
    }
    if (alive.empty()) {
      state_.leader_id.reset();
      emit_state();
      return;
    }

    NodeId highest_id = *std::max_element(alive.begin(), alive.end(),
                                           [](const NodeId& a, const NodeId& b) {
                                             return a.value < b.value;
                                           });

    bool need_election = !state_.leader_id.has_value() ||
                          state_.leader_id->value != highest_id.value;

    if (need_election) {
      current_term_++;
      fencing_token_ = make_fencing_token();
      membership_revision_ = snapshot.revision;

      if (term_store_) {
        term_store_->store_term(cluster_id_, current_term_);
      }

      state_.leader_id = highest_id;
      state_.fence.term = current_term_;
      state_.fence.token = FencingToken{fencing_token_};
      state_.fence.membership_revision = membership_revision_;
      state_.locally_valid_until = Clock::now() + std::chrono::seconds(30);
    }

    emit_state();
  }

  LeadershipState current() const override {
    std::lock_guard lock(mutex_);
    if (!running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition, "election not running");
    }
    return state_;
  }

  void stop() noexcept override {
    std::lock_guard lock(mutex_);
    running_ = false;
  }

  bool is_leader() const {
    std::lock_guard lock(mutex_);
    return state_.leader_id.has_value() &&
           state_.leader_id->value == local_node_.value;
  }

  std::uint64_t term() const {
    std::lock_guard lock(mutex_);
    return current_term_;
  }

  std::string fencing_token() const {
    std::lock_guard lock(mutex_);
    return fencing_token_;
  }

 private:
  void emit_state() {
    if (callback_) callback_(state_);
  }

  mutable std::mutex mutex_;
  NodeId local_node_;
  std::string cluster_id_;
  ElectionTermStore* term_store_{nullptr};
  LeadershipCallback callback_;
  LeadershipState state_;
  std::uint64_t current_term_{0};
  std::uint64_t membership_revision_{0};
  std::uint64_t last_snapshot_revision_{0};
  std::string fencing_token_;
  bool running_{false};
};

std::unique_ptr<LeaderElection> make_bully_election() {
  return std::make_unique<BullyElection>();
}

}  // namespace socrates::cluster
