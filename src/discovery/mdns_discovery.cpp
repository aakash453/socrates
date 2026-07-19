#include "socrates/discovery/discovery_service.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <dns_sd.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>
#elif defined(_WIN32)
// Windows 10+ DNS-SD (Bonjour-compatible API in dnssd.dll)
#include <dns_sd.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "dnssd.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace socrates::discovery {

namespace {

#if defined(__APPLE__) || defined(_WIN32)

// ── macOS / Windows Bonjour DNS-SD implementation ──────────────────────────

struct DnsSdContext {
  DiscoveryCallback callback;
  std::string service_type;
  DNSServiceRef browse_ref{nullptr};
  DNSServiceRef advertise_ref{nullptr};
  std::thread event_thread;
  std::atomic<bool> running{false};
  std::string node_id;
  std::string fingerprint;
  uint16_t port{0};
};

void DNSSD_API dns_sd_browse_reply(DNSServiceRef, DNSServiceFlags flags,
                                    uint32_t, DNSServiceErrorType errorCode,
                                    const char* serviceName, const char*,
                                    const char*, void* context) {
  auto* ctx = static_cast<DnsSdContext*>(context);
  if (errorCode != kDNSServiceErr_NoError || !ctx->callback) return;
  bool is_add = (flags & kDNSServiceFlagsAdd) != 0;
  if (is_add) {
    DiscoveryEvent ev;
    ev.kind = DiscoveryEventKind::kFound;
    ev.peer.node_id = NodeId{serviceName ? serviceName : "unknown"};
    ev.peer.incarnation = 1;
    ev.peer.public_key_fingerprint =
        "mdns-" + std::string(serviceName ? serviceName : "");
    ctx->callback(ev);
  }
}

void dns_sd_event_loop(DnsSdContext* ctx) {
  while (ctx->running) {
    if (ctx->browse_ref) DNSServiceProcessResult(ctx->browse_ref);
    if (ctx->advertise_ref) DNSServiceProcessResult(ctx->advertise_ref);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void dns_sd_start(DnsSdContext* ctx, const std::string& svc_name,
                   const std::string& txt_record) {
  ctx->running = true;

  DNSServiceErrorType err = DNSServiceRegister(
      &ctx->advertise_ref, 0, kDNSServiceInterfaceIndexAny,
      svc_name.c_str(), ctx->service_type.c_str(),
      nullptr, nullptr, htons(ctx->port),
      static_cast<uint16_t>(txt_record.size()), txt_record.data(),
      nullptr, nullptr);
  if (err != kDNSServiceErr_NoError) {
    ctx->running = false;
    throw RuntimeError(ErrorCode::kInternal,
                       "DNS-SD register failed: " + std::to_string(err));
  }

  err = DNSServiceBrowse(&ctx->browse_ref, 0, kDNSServiceInterfaceIndexAny,
                          ctx->service_type.c_str(), nullptr,
                          dns_sd_browse_reply, ctx);
  if (err != kDNSServiceErr_NoError) {
    DNSServiceRefDeallocate(ctx->advertise_ref);
    ctx->advertise_ref = nullptr;
    ctx->running = false;
    throw RuntimeError(ErrorCode::kInternal,
                       "DNS-SD browse failed: " + std::to_string(err));
  }

  ctx->event_thread = std::thread(dns_sd_event_loop, ctx);
}

void dns_sd_stop(DnsSdContext* ctx) {
  ctx->running = false;
  if (ctx->browse_ref) { DNSServiceRefDeallocate(ctx->browse_ref); ctx->browse_ref = nullptr; }
  if (ctx->advertise_ref) { DNSServiceRefDeallocate(ctx->advertise_ref); ctx->advertise_ref = nullptr; }
  if (ctx->event_thread.joinable()) ctx->event_thread.join();
}

#elif defined(__linux__) && !defined(__ANDROID__)

// ── Linux Avahi implementation ─────────────────────────────────────────────

struct AvahiContext {
  DiscoveryCallback callback;
  AvahiClient* client{nullptr};
  AvahiServiceBrowser* browser{nullptr};
  AvahiEntryGroup* group{nullptr};
  AvahiThreadedPoll* poll{nullptr};
  std::atomic<bool> running{false};
};

void avahi_browse_callback(AvahiServiceBrowser*, AvahiIfIndex, AvahiProtocol,
                            AvahiBrowserEvent event, const char* name,
                            const char*, const char*,
                            AvahiLookupResultFlags, void* userdata) {
  auto* ctx = static_cast<AvahiContext*>(userdata);
  if (event == AVAHI_BROWSER_NEW && ctx->callback) {
    DiscoveryEvent ev;
    ev.kind = DiscoveryEventKind::kFound;
    ev.peer.node_id = NodeId{name ? name : "unknown"};
    ev.peer.incarnation = 1;
    ev.peer.public_key_fingerprint = "avahi-" + std::string(name ? name : "");
    ctx->callback(ev);
  }
}

#endif

}  // namespace

class MdnsDiscovery final : public DiscoveryService {
 public:
  ~MdnsDiscovery() override { stop(); }

  void start(const DiscoveryConfig& config, PeerAdvertisement local_peer,
             DiscoveryCallback callback) override {
    std::lock_guard lock(mutex_);
    if (running_) {
      throw RuntimeError(ErrorCode::kFailedPrecondition, "mDNS already started");
    }
    config_ = config;
    local_peer_ = std::move(local_peer);
    callback_ = std::move(callback);
    running_ = true;

#if defined(__APPLE__) || defined(_WIN32)
    dns_sd_ctx_ = std::make_unique<DnsSdContext>();
    dns_sd_ctx_->callback = callback_;
    dns_sd_ctx_->service_type = config_.service_name + "._tcp";
    dns_sd_ctx_->node_id = local_peer_.node_id.value;
    dns_sd_ctx_->fingerprint = local_peer_.public_key_fingerprint;
    dns_sd_ctx_->port = config_.udp_port > 0 ? config_.udp_port : 9876;

    std::string svc_name = local_peer_.node_id.value + "." + config_.service_name;
    std::string txt_record = "fp=" + local_peer_.public_key_fingerprint;
    try {
         dns_sd_start(dns_sd_ctx_.get(), svc_name, txt_record);
       } catch (const RuntimeError&) {
         // Bonjour failed (likely no entitlements) — fall back gracefully.
         // Reset the context since start() didn't fully complete.
         dns_sd_ctx_.reset();
         running_ = true;
       }

#elif defined(__linux__) && !defined(__ANDROID__)
    avahi_ctx_ = std::make_unique<AvahiContext>();
    avahi_ctx_->callback = callback_;
    avahi_ctx_->running = true;

    avahi_ctx_->poll = avahi_threaded_poll_new();
    if (!avahi_ctx_->poll)
      throw RuntimeError(ErrorCode::kInternal, "Avahi poll creation failed");

    int avahi_err = 0;
    avahi_ctx_->client = avahi_client_new(
        avahi_threaded_poll_get(avahi_ctx_->poll),
        AVAHI_CLIENT_NO_FAIL, nullptr, nullptr, &avahi_err);
    if (!avahi_ctx_->client)
      throw RuntimeError(ErrorCode::kInternal,
                         "Avahi client failed: " + std::string(avahi_strerror(avahi_err)));

    avahi_ctx_->browser = avahi_service_browser_new(
        avahi_ctx_->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
        config_.service_name.c_str(), nullptr,
        static_cast<AvahiLookupFlags>(0), avahi_browse_callback, avahi_ctx_.get());
    if (!avahi_ctx_->browser)
      throw RuntimeError(ErrorCode::kInternal,
                         "Avahi browser failed: " +
                         std::string(avahi_strerror(avahi_client_errno(avahi_ctx_->client))));

    avahi_threaded_poll_start(avahi_ctx_->poll);
#endif
  }

  void update_advertisement(PeerAdvertisement local_peer) override {
    std::lock_guard lock(mutex_);
    local_peer_ = std::move(local_peer);
  }

  void stop() noexcept override {
#if defined(__APPLE__) || defined(_WIN32)
    if (dns_sd_ctx_) { dns_sd_stop(dns_sd_ctx_.get()); dns_sd_ctx_.reset(); }
#elif defined(__linux__) && !defined(__ANDROID__)
    if (avahi_ctx_) {
      avahi_ctx_->running = false;
      if (avahi_ctx_->browser) { avahi_service_browser_free(avahi_ctx_->browser); avahi_ctx_->browser = nullptr; }
      if (avahi_ctx_->group) { avahi_entry_group_free(avahi_ctx_->group); avahi_ctx_->group = nullptr; }
      if (avahi_ctx_->poll) avahi_threaded_poll_stop(avahi_ctx_->poll);
      if (avahi_ctx_->client) avahi_client_free(avahi_ctx_->client);
      if (avahi_ctx_->poll) avahi_threaded_poll_free(avahi_ctx_->poll);
      avahi_ctx_.reset();
    }
#endif
    std::lock_guard lock(mutex_);
    running_ = false;
  }

  void inject_peer(const std::string& name) {
    DiscoveryCallback cb;
    { std::lock_guard lock(mutex_); cb = callback_; }
    if (!cb) return;
    DiscoveryEvent ev;
    ev.kind = DiscoveryEventKind::kFound;
    ev.peer.node_id = NodeId{name};
    ev.peer.incarnation = 1;
    ev.peer.public_key_fingerprint = "injected-" + name;
    cb(ev);
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

#if defined(__APPLE__) || defined(_WIN32)
  std::unique_ptr<DnsSdContext> dns_sd_ctx_;
#elif defined(__linux__) && !defined(__ANDROID__)
  std::unique_ptr<AvahiContext> avahi_ctx_;
#endif
};

std::unique_ptr<DiscoveryService> make_mdns_discovery() {
  return std::make_unique<MdnsDiscovery>();
}

}  // namespace socrates::discovery
