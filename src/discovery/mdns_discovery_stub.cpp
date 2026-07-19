#include "socrates/discovery/discovery_service.h"

namespace socrates::discovery {

std::unique_ptr<DiscoveryService> make_mdns_discovery() {
  // mDNS (Bonjour) is not supported on this platform.
  // Use UDP broadcast or BLE as fallback.
  return nullptr;
}

}  // namespace socrates::discovery
