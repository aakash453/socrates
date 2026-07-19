#include "socrates/discovery/discovery_service.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
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

    // Create send socket (broadcast-enabled)
    send_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sock_ >= 0) {
      int on = 1;
      setsockopt(send_sock_, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }

    // Create recv socket (bind to port)
    recv_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_sock_ >= 0) {
      int on = 1;
      setsockopt(recv_sock_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
      setsockopt(recv_sock_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
      struct sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(port_);
      if (bind(recv_sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(recv_sock_);
        recv_sock_ = -1;
      }
    }

    // Send thread: broadcast local peer periodically
    send_thread_ = std::thread([this]() {
      while (running_) {
        send_announcement();
        std::this_thread::sleep_for(config_.announce_interval);
      }
    });

    // Recv thread: listen for broadcasts
    recv_thread_ = std::thread([this]() {
      char buf[2048];
      while (running_) {
        struct sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(recv_sock_, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&from, &from_len);
        if (n <= 0) continue;
        buf[n] = '\0';

        // Parse peer advertisement from JSON
        auto adv = parse_advertisement(buf, static_cast<size_t>(n));
        if (!adv.node_id.empty()) {
          DiscoveryEvent ev;
          ev.kind = DiscoveryEventKind::kFound;
          ev.peer = adv;

          DiscoveryCallback cb;
          {
            std::lock_guard lk(mutex_);
            cb = callback_;
          }
          if (cb) cb(ev);
        }
      }
    });
  }

  void update_advertisement(PeerAdvertisement local_peer) override {
    std::lock_guard lock(mutex_);
    local_peer_ = std::move(local_peer);
  }

  void stop() noexcept override {
    running_ = false;
    if (send_sock_ >= 0) { close(send_sock_); send_sock_ = -1; }
    if (recv_sock_ >= 0) { close(recv_sock_); recv_sock_ = -1; }
    if (send_thread_.joinable()) send_thread_.join();
    if (recv_thread_.joinable()) recv_thread_.join();
  }

 private:
  void send_announcement() {
    if (send_sock_ < 0) return;

    // Serialize peer advertisement to compact JSON
    std::string json;
    {
      std::lock_guard lock(mutex_);
      json = serialize_peer(local_peer_);
    }
    if (json.empty()) return;

    struct sockaddr_in broadcast{};
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(port_);
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);  // 255.255.255.255

    ssize_t sent = sendto(send_sock_, json.c_str(), json.size(), 0,
           (struct sockaddr*)&broadcast, sizeof(broadcast));
    (void)sent;
  }

  // Compact wire format: "node_id|incarnation|fingerprint"
  static std::string serialize_peer(const PeerAdvertisement& peer) {
    return peer.node_id.value + "|" +
           std::to_string(peer.incarnation) + "|" +
           peer.public_key_fingerprint;
  }

  static PeerAdvertisement parse_advertisement(const char* data, size_t len) {
    PeerAdvertisement peer;
    std::string s(data, len);

    auto p1 = s.find('|');
    if (p1 == std::string::npos) return peer;
    auto p2 = s.find('|', p1 + 1);
    if (p2 == std::string::npos) return peer;

    peer.node_id = NodeId{s.substr(0, p1)};
    peer.incarnation = static_cast<std::uint32_t>(
        std::stoul(s.substr(p1 + 1, p2 - p1 - 1)));
    peer.public_key_fingerprint = s.substr(p2 + 1);
    return peer;
  }

  mutable std::mutex mutex_;
  DiscoveryConfig config_;
  PeerAdvertisement local_peer_;
  DiscoveryCallback callback_;
  std::uint16_t port_{9876};
  bool running_{false};

  int send_sock_{-1};
  int recv_sock_{-1};
  std::thread send_thread_;
  std::thread recv_thread_;
};

std::unique_ptr<DiscoveryService> make_udp_discovery() {
  return std::make_unique<UdpBroadcastDiscovery>();
}

}  // namespace socrates::discovery
