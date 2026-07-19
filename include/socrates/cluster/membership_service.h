// include/socrates/cluster/membership_service.h
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "socrates/discovery/discovery_service.h"
#include "socrates/result.h"
#include "socrates/security/identity_provider.h"
#include "socrates/types.h"

namespace socrates::cluster {

enum class MemberState { kJoining, kAlive, kSuspect, kLeft, kRemoved };

/// A node starts as kLeech (can send prompts, cannot run inference).
/// After downloading models and opting in, it becomes kParticipant.
enum class NodeRole : std::uint8_t { kLeech, kParticipant };

/// Tracks which model shards a node has downloaded.
struct ModelPresence {
  ModelId model_id;
  std::string manifest_revision;
  std::string quantization;       // e.g. "int4", "fp16"
  std::uint64_t downloaded_bytes{0};
  std::uint64_t total_bytes{0};
  bool ready{false};              // true when download complete
};

struct Member {
  discovery::PeerAdvertisement peer;
  MemberState state{MemberState::kJoining};
  NodeRole role{NodeRole::kLeech};        // starts as leech
  std::optional<CapabilityProfile> capability;
  std::vector<ModelPresence> models;       // downloaded model shards
  std::chrono::system_clock::time_point last_seen;
};

struct MembershipSnapshot {
  std::uint64_t revision{0};
  std::vector<Member> members;
};

using MembershipCallback = std::function<void(const MembershipSnapshot&)>;

class MembershipService {
 public:
  virtual ~MembershipService() = default;

  virtual void start(security::IdentityProvider& identity_provider,
                     MembershipCallback callback) = 0;

  virtual void observe(const discovery::DiscoveryEvent& event) = 0;

  virtual void update_capability(const CapabilityProfile& profile) = 0;

  /// Report which model shards the local node has downloaded.
  virtual void update_model_presence(const std::vector<ModelPresence>& models) = 0;

  virtual void promote_joining_members() = 0;

  [[nodiscard]] virtual MembershipSnapshot snapshot() const = 0;

  virtual void stop() noexcept = 0;
};

std::unique_ptr<MembershipService> make_membership_service();

}  // namespace socrates::cluster
