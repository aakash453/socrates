#include "socrates/discovery/discovery_service.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>

namespace socrates::discovery {

class BluetoothDiscovery final : public DiscoveryService {
 public:
  void start(const DiscoveryConfig& config, PeerAdvertisement local_peer,
             DiscoveryCallback callback) override {
    std::lock_guard lock(mutex_);
    if (running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition, "BLE discovery already started");
    }
    config_ = config;
    local_peer_ = std::move(local_peer);
    callback_ = std::move(callback);
    running_ = true;
    permission_denied_ = false;
  }

  void update_advertisement(PeerAdvertisement local_peer) override {
    std::lock_guard lock(mutex_);
    local_peer_ = std::move(local_peer);
  }

  void stop() noexcept override {
    std::lock_guard lock(mutex_);
    running_ = false;
  }

  void simulate_permission_denied() {
    std::lock_guard lock(mutex_);
    permission_denied_ = true;
  }

  bool is_permission_denied() const {
    std::lock_guard lock(mutex_);
    return permission_denied_;
  }

  void inject_peer(const PeerAdvertisement& peer) {
    DiscoveryEvent event;
    event.kind = DiscoveryEventKind::kFound;
    event.peer = peer;

    DiscoveryCallback cb;
    {
      std::lock_guard lock(mutex_);
      if (permission_denied_) return;
      cb = callback_;
    }
    if (cb) cb(event);
  }

  bool is_running() const {
    std::lock_guard lock(mutex_);
    return running_;
  }

 private:
  mutable std::mutex mutex_;
  DiscoveryConfig config_;
  PeerAdvertisement local_peer_;
  DiscoveryCallback callback_;
  bool running_{false};
  bool permission_denied_{false};
};

std::unique_ptr<DiscoveryService> make_bluetooth_discovery() {
  return std::make_unique<BluetoothDiscovery>();
}

}  // namespace socrates::discovery
