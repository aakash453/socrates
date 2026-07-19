#include "socrates/discovery/discovery_service.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace socrates::discovery {

class UdpBroadcastDiscovery final : public DiscoveryService {
 public:
  void start(const DiscoveryConfig& config, PeerAdvertisement local_peer,
             DiscoveryCallback callback) override {
    std::lock_guard lock(mutex_);
    if (running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition, "UDP discovery already started");
    }
    config_ = config;
    local_peer_ = std::move(local_peer);
    callback_ = std::move(callback);
    running_ = true;
    port_ = config_.udp_port;
  }

  void update_advertisement(PeerAdvertisement local_peer) override {
    std::lock_guard lock(mutex_);
    local_peer_ = std::move(local_peer);
  }

  void stop() noexcept override {
    std::lock_guard lock(mutex_);
    running_ = false;
  }

  void inject_peer(const PeerAdvertisement& peer) {
    DiscoveryEvent event;
    event.kind = DiscoveryEventKind::kFound;
    event.peer = peer;

    DiscoveryCallback cb;
    {
      std::lock_guard lock(mutex_);
      cb = callback_;
    }
    if (cb) cb(event);
  }

  bool is_running() const {
    std::lock_guard lock(mutex_);
    return running_;
  }

  std::uint16_t port() const {
    std::lock_guard lock(mutex_);
    return port_;
  }

 private:
  mutable std::mutex mutex_;
  DiscoveryConfig config_;
  PeerAdvertisement local_peer_;
  DiscoveryCallback callback_;
  std::uint16_t port_{0};
  bool running_{false};
};

std::unique_ptr<DiscoveryService> make_udp_discovery() {
  return std::make_unique<UdpBroadcastDiscovery>();
}

}  // namespace socrates::discovery
