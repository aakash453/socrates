#include "socrates/discovery/discovery_service.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace socrates::discovery {

class CoordinatedDiscovery final : public DiscoveryService {
 public:
  void start(const DiscoveryConfig& config, PeerAdvertisement local_peer,
             DiscoveryCallback callback) override {
    std::lock_guard lock(mutex_);
    if (running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition, "coordinator already started");
    }
    config_ = config;
    local_peer_ = std::move(local_peer);
    callback_ = std::move(callback);
    running_ = true;

    for (auto method : config_.fallback_order) {
      std::unique_ptr<DiscoveryService> adapter;
      switch (method) {
        case DiscoveryMethod::kMdns:
          adapter = make_mdns_discovery();
          break;
        case DiscoveryMethod::kUdpBroadcast:
          adapter = make_udp_discovery();
          break;
        case DiscoveryMethod::kBluetoothLowEnergy:
          adapter = make_bluetooth_discovery();
          break;
      }
      if (adapter) {
        DiscoveryConfig sub = config_;
        // sub.scan_timeout applies per-adapter
        adapter->start(sub, local_peer_, callback_);
        adapters_.push_back(std::move(adapter));
      }
    }
  }

  void update_advertisement(PeerAdvertisement local_peer) override {
    std::lock_guard lock(mutex_);
    local_peer_ = std::move(local_peer);
    for (auto& a : adapters_) a->update_advertisement(local_peer_);
  }

  void stop() noexcept override {
    std::lock_guard lock(mutex_);
    if (!running_) return;
    running_ = false;
    for (auto& a : adapters_) a->stop();
    adapters_.clear();
  }

 private:
  mutable std::mutex mutex_;
  DiscoveryConfig config_;
  PeerAdvertisement local_peer_;
  DiscoveryCallback callback_;
  std::vector<std::unique_ptr<DiscoveryService>> adapters_;
  bool running_{false};
};

std::unique_ptr<DiscoveryService> make_coordinated_discovery() {
  return std::make_unique<CoordinatedDiscovery>();
}

}  // namespace socrates::discovery
