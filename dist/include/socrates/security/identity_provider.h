#pragma once

#include <memory>
#include <string>
#include <vector>

#include "socrates/discovery/discovery_service.h"
#include "socrates/result.h"
#include "socrates/types.h"

namespace socrates::security {

struct LocalIdentity {
  NodeId node_id;
  std::string certificate_pem;
  std::string public_key_fingerprint;
};

struct PeerIdentity {
  NodeId node_id;
  std::string certificate_pem;
  std::string public_key_fingerprint;
};

class IdentityProvider {
 public:
  virtual ~IdentityProvider() = default;

  /**
   * Returns or auto-generates the local cluster identity.
   * Preconditions: configured key storage is writable when generation is needed.
   * Postconditions: identity is stable across restart until explicit rotation.
   * Throws: no operational exceptions; Result reports key-store failures.
   * Thread safety: safe for concurrent calls and returns one coherent identity.
   * Side effects: may generate and persist a private key/certificate.
   */
  virtual Result<LocalIdentity> local_identity() = 0;

  /**
   * Authenticates a candidate under ephemeral-cluster, pinned, or private-CA policy.
   * Preconditions: advertisement fingerprint and presented certificate are supplied.
   * Postconditions: success binds node ID, certificate, and transport peer identity;
   * discovery metadata alone is never accepted as proof.
   * Throws: no operational exceptions; Result reports untrusted/expired identities.
   * Thread safety: safe for concurrent candidate admissions.
   * Side effects: may persist a trust-on-first-cluster fingerprint or audit record.
   */
  virtual Result<PeerIdentity> authenticate(
      const discovery::PeerAdvertisement& advertisement,
      const std::string& presented_certificate_pem) = 0;
};

std::unique_ptr<IdentityProvider> make_ephemeral_identity_provider();
std::unique_ptr<IdentityProvider> make_pinned_identity_provider(
    std::vector<std::string> allowlist);
std::unique_ptr<IdentityProvider> make_private_ca_identity_provider();

}  // namespace socrates::security
