#include "socrates/cluster/membership_service.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace socrates::cluster {

class MembershipServiceImpl final : public MembershipService {
 public:
  MembershipServiceImpl() {
    liveness_active_ = true;
    liveness_worker_ =
        std::thread(&MembershipServiceImpl::liveness_loop, this);
  }

  ~MembershipServiceImpl() override {
    liveness_active_ = false;
    if (liveness_worker_.joinable()) {
      liveness_worker_.join();
    }
  }

  MembershipServiceImpl(const MembershipServiceImpl&) = delete;
  MembershipServiceImpl& operator=(const MembershipServiceImpl&) = delete;

  void start(security::IdentityProvider& identity_provider,
             MembershipCallback callback) override {
    std::lock_guard lock(mutex_);
    if (running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition,
                         "membership already running");
    }
    identity_provider_ = &identity_provider;
    callback_ = std::move(callback);
    running_ = true;
  }

  void observe(const discovery::DiscoveryEvent& event) override {
    std::lock_guard lock(mutex_);
    if (!running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition,
                         "membership not running");
    }
    if (event.peer.node_id.empty()) return;

    auto it = find_member(event.peer.node_id);

    if (it == members_.end()) {
      if (!identity_provider_) return;

      auto auth_result = identity_provider_->authenticate(
          event.peer, event.peer.public_key_fingerprint);
      if (auth_result.is_err()) return;

      Member m;
      m.peer = event.peer;
      m.state = MemberState::kJoining;
      m.role = NodeRole::kLeech;  // new nodes start as leech
      m.last_seen = std::chrono::system_clock::now();
      members_.push_back(std::move(m));
    } else {
      it->peer = event.peer;
      it->last_seen = std::chrono::system_clock::now();
      if (it->state == MemberState::kSuspect) {
        it->state = MemberState::kAlive;
      }
    }
    snapshot_revision_++;
    emit_snapshot();
  }

  void update_capability(const CapabilityProfile& profile) override {
    std::lock_guard lock(mutex_);
    auto it = find_member(profile.node_id);
    if (it == members_.end()) {
      throw RuntimeError(ErrorCode::kNotFound,
                         "member not found for capability update");
    }
    it->capability = profile;
    snapshot_revision_++;
    emit_snapshot();
  }

  void update_model_presence(const std::vector<ModelPresence>& models) override {
    std::lock_guard lock(mutex_);
    // Update models for all members (in production, scoped to local node).
    // The caller is responsible for providing the local node's models.
    for (auto& m : members_) {
      m.models = models;
    }
    snapshot_revision_++;
    emit_snapshot();
  }

  MembershipSnapshot snapshot() const override {
    std::lock_guard lock(mutex_);
    if (!running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition,
                         "membership not running");
    }
    return build_snapshot();
  }

  void stop() noexcept override {
    liveness_active_ = false;
    {
      std::lock_guard lock(mutex_);
      running_ = false;
    }
    if (liveness_worker_.joinable()) {
      liveness_worker_.join();
    }
  }

  void promote_joining_members() override {
    std::lock_guard lock(mutex_);
    bool changed = false;
    for (auto& m : members_) {
      if (m.state == MemberState::kJoining) {
        m.state = MemberState::kAlive;
        m.last_seen = std::chrono::system_clock::now();
        changed = true;
      }
    }
    if (changed) {
      snapshot_revision_++;
      emit_snapshot();
    }
  }

 private:
  using MemberVec = std::vector<Member>;

  MemberVec::iterator find_member(const NodeId& id) {
    return std::find_if(
        members_.begin(), members_.end(),
        [&](const Member& m) { return m.peer.node_id == id; });
  }

  MemberVec::const_iterator find_member(const NodeId& id) const {
    return std::find_if(
        members_.begin(), members_.end(),
        [&](const Member& m) { return m.peer.node_id == id; });
  }

  MembershipSnapshot build_snapshot() const {
    MembershipSnapshot snap;
    snap.revision = snapshot_revision_;
    snap.members = members_;
    return snap;
  }

  void emit_snapshot() {
    if (callback_) callback_(build_snapshot());
  }

  void liveness_loop() {
    while (liveness_active_) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      if (!liveness_active_) break;

      std::lock_guard lock(mutex_);
      auto now = std::chrono::system_clock::now();
      bool changed = false;

      for (auto& m : members_) {
        if (m.state == MemberState::kAlive) {
          auto age = std::chrono::duration_cast<std::chrono::seconds>(
                         now - m.last_seen)
                         .count();
          if (age > 30) {
            m.state = MemberState::kSuspect;
            changed = true;
          }
        } else if (m.state == MemberState::kSuspect) {
          auto age = std::chrono::duration_cast<std::chrono::seconds>(
                         now - m.last_seen)
                         .count();
          if (age > 60) {
            m.state = MemberState::kRemoved;
            changed = true;
          }
        }
      }

      // Purge removed members
      members_.erase(
          std::remove_if(members_.begin(), members_.end(),
                         [](const Member& m) {
                           return m.state == MemberState::kRemoved;
                         }),
          members_.end());

      if (changed) {
        snapshot_revision_++;
        if (callback_) callback_(build_snapshot());
      }
    }
  }

  mutable std::mutex mutex_;
  MemberVec members_;
  std::uint64_t snapshot_revision_{0};
  bool running_{false};
  security::IdentityProvider* identity_provider_{nullptr};
  MembershipCallback callback_;
  std::atomic<bool> liveness_active_{false};
  std::thread liveness_worker_;
};

std::unique_ptr<MembershipService> make_membership_service() {
  return std::make_unique<MembershipServiceImpl>();
}

}  // namespace socrates::cluster
