#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "socrates/result.h"
#include "socrates/types.h"

namespace socrates::discovery {

enum class DiscoveryMethod { kMdns, kUdpBroadcast, kBluetoothLowEnergy };
enum class DiscoveryEventKind { kFound, kUpdated, kExpired };

struct PeerAdvertisement {
  NodeId node_id;
  std::uint64_t incarnation{0};
  std::vector<std::string> endpoints;
  std::string public_key_fingerprint;
  Attributes metadata;
};

struct DiscoveryEvent {
  DiscoveryEventKind kind{DiscoveryEventKind::kFound};
  PeerAdvertisement peer;
};

struct DiscoveryConfig {
  std::vector<DiscoveryMethod> fallback_order;
  std::string service_name;
  std::uint16_t udp_port{0};
  std::chrono::milliseconds scan_timeout{1500};
  std::chrono::milliseconds announce_interval{3000};
};

using DiscoveryCallback = std::function<void(const DiscoveryEvent&)>;

class DiscoveryService {
 public:
  virtual ~DiscoveryService() = default;

  /**
   * Starts zero-configuration discovery and local advertisement.
   * Preconditions: callback is non-empty; service is stopped; config contains
   * at least one method and a non-empty service name.
   * Postconditions: on success, candidate events may be delivered until stop()
   * returns. Duplicate wire advertisements are coalesced by node/incarnation.
   * Throws: RuntimeError(kInvalidArgument) for invalid configuration and
   * RuntimeError(kFailedPrecondition) if already started.
   * Thread safety: safe to call concurrently with update_advertisement() and
   * stop(); callback invocations are serialized and occur without service locks.
   * Side effects: opens platform discovery resources, advertises locally, and
   * may prompt for OS local-network/Bluetooth permission.
   */
  virtual void start(const DiscoveryConfig& config,
                     PeerAdvertisement local_peer,
                     DiscoveryCallback callback) = 0;

  /**
   * Replaces the advertised endpoint and metadata snapshot.
   * Preconditions: service is running; node_id and incarnation equal those
   * supplied to start().
   * Postconditions: future advertisements use the new immutable snapshot.
   * Throws: RuntimeError(kFailedPrecondition) when stopped or identity changes.
   * Thread safety: may be called concurrently with callbacks and stop().
   * Side effects: publishes an updated mDNS/UDP/BLE advertisement.
   */
  virtual void update_advertisement(PeerAdvertisement local_peer) = 0;

  /**
   * Stops discovery and waits until no future callback can begin.
   * Preconditions: none; stopping an already stopped service is valid.
   * Postconditions: discovery resources are released and callbacks quiesced.
   * Throws: never.
   * Thread safety: idempotent and safe from any thread except inside callback.
   * Side effects: withdraws advertisements and closes platform resources.
   */
  virtual void stop() noexcept = 0;
};

std::unique_ptr<DiscoveryService> make_mdns_discovery();
std::unique_ptr<DiscoveryService> make_udp_discovery();
std::unique_ptr<DiscoveryService> make_bluetooth_discovery();
std::unique_ptr<DiscoveryService> make_coordinated_discovery();

}  // namespace socrates::discovery
