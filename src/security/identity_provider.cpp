#include "socrates/security/identity_provider.h"

#include <cstring>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#if SOCRATES_HAS_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/core_names.h>
#endif

namespace socrates::security {

// ── Platform-appropriate real crypto ───────────────────────────────────────

namespace {

// Generate a SHA-256 fingerprint from a certificate PEM string.
// Returns hex-encoded string.
std::string compute_fingerprint_sha256(const std::string& cert_pem) {
#if SOCRATES_HAS_OPENSSL
  BIO* bio = BIO_new_mem_buf(cert_pem.data(),
                              static_cast<int>(cert_pem.size()));
  if (!bio) return "";

  X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!cert) return "";

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  const EVP_MD* md = EVP_sha256();
  X509_digest(cert, md, digest, &digest_len);
  X509_free(cert);

  std::ostringstream os;
  os << std::hex;
  for (unsigned int i = 0; i < digest_len; ++i) {
    os << ((digest[i] >> 4) & 0xF) << (digest[i] & 0xF);
  }
  return os.str();
#else
  // Fallback: deterministic pseudorandom fingerprint
  std::hash<std::string> hasher;
  std::ostringstream os;
  os << std::hex << hasher(cert_pem);
  return os.str();
#endif
}

// Generate a self-signed X.509 certificate and EC private key.
// Returns {certificate_pem, private_key_pem}.
std::pair<std::string, std::string> generate_self_signed_cert(
    const std::string& common_name) {
#if SOCRATES_HAS_OPENSSL
  // Generate EC P-256 key using modern EVP_PKEY API (OpenSSL 3.x compatible)
  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  if (!pctx) return {};

  if (EVP_PKEY_keygen_init(pctx) <= 0 ||
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return {};
  }

  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_keygen(pctx, &pkey) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return {};
  }
  EVP_PKEY_CTX_free(pctx);

  // Create X.509 certificate
  X509* x509 = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_getm_notBefore(x509), 0);
  X509_gmtime_adj(X509_getm_notAfter(x509), 365 * 24 * 3600);  // 1 year

  X509_set_pubkey(x509, pkey);

  X509_NAME* name = X509_get_subject_name(x509);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>(common_name.c_str()),
      -1, -1, 0);
  X509_set_issuer_name(x509, name);  // self-signed

  X509_sign(x509, pkey, EVP_sha256());

  // Serialize to PEM
  BIO* cert_bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(cert_bio, x509);
  char* cert_data = nullptr;
  long cert_len = BIO_get_mem_data(cert_bio, &cert_data);
  std::string cert_pem(cert_data, static_cast<size_t>(cert_len));
  BIO_free(cert_bio);

  BIO* key_bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  char* key_data = nullptr;
  long key_len = BIO_get_mem_data(key_bio, &key_data);
  std::string key_pem(key_data, static_cast<size_t>(key_len));
  BIO_free(key_bio);

  X509_free(x509);
  EVP_PKEY_free(pkey);

  return {cert_pem, key_pem};
#else
  return {"CERTIFICATE-PEM-PLACEHOLDER-" + common_name,
          "PRIVATE-KEY-PLACEHOLDER-" + common_name};
#endif
}

// Verify a peer certificate against a trust bundle.
bool verify_peer_cert(const std::string& peer_cert_pem,
                       const std::string& trust_bundle_pem) {
#if SOCRATES_HAS_OPENSSL
  if (trust_bundle_pem.empty()) return true;  // ephemeral mode

  BIO* cert_bio = BIO_new_mem_buf(peer_cert_pem.data(),
                                   static_cast<int>(peer_cert_pem.size()));
  X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
  BIO_free(cert_bio);
  if (!cert) return false;

  BIO* trust_bio = BIO_new_mem_buf(trust_bundle_pem.data(),
                                    static_cast<int>(trust_bundle_pem.size()));
  X509_STORE* store = X509_STORE_new();
  X509* ca = PEM_read_bio_X509(trust_bio, nullptr, nullptr, nullptr);
  while (ca) {
    X509_STORE_add_cert(store, ca);
    X509_free(ca);
    ca = PEM_read_bio_X509(trust_bio, nullptr, nullptr, nullptr);
  }
  BIO_free(trust_bio);

  X509_STORE_CTX* ctx = X509_STORE_CTX_new();
  X509_STORE_CTX_init(ctx, store, cert, nullptr);
  int result = X509_verify_cert(ctx);

  X509_STORE_CTX_free(ctx);
  X509_STORE_free(store);
  X509_free(cert);

  return result == 1;
#else
  (void)peer_cert_pem; (void)trust_bundle_pem; return true;  // ephemeral: trust everything
#endif
}

// Generate a random hex node ID
std::string make_node_id() {
#if SOCRATES_HAS_OPENSSL
  unsigned char buf[16];
  if (RAND_bytes(buf, sizeof(buf)) == 1) {
    std::ostringstream os;
    os << std::hex;
    for (auto b : buf) os << static_cast<int>(b);
    return "node-" + os.str();
  }
#endif
  // Fallback pseudo-random
  static std::mt19937_64 rng(std::random_device{}());
  std::ostringstream os;
  os << "node-" << std::hex << rng();
  return os.str();
}

}  // namespace

// ── Identity provider implementation ──────────────────────────────────────

enum class TrustPolicy { kEphemeralCluster, kPinnedAllowlist, kPrivateCa };

class IdentityProviderImpl : public IdentityProvider {
 public:
  explicit IdentityProviderImpl(TrustPolicy policy) : policy_(policy) {}

  Result<LocalIdentity> local_identity() override {
    std::unique_lock lock(mutex_);
    if (!local_) {
      auto node_id_str = make_node_id();

      // Generate a real self-signed X.509 certificate
      auto [cert_pem, key_pem] =
          generate_self_signed_cert("socrates-" + node_id_str);

      std::string fingerprint = compute_fingerprint_sha256(cert_pem);

      local_ = LocalIdentity{NodeId{node_id_str},
                              cert_pem.empty()
                                  ? "CERT-PLACEHOLDER-" + node_id_str
                                  : cert_pem,
                              fingerprint.empty()
                                  ? "fp-" + node_id_str
                                  : fingerprint};

      if (policy_ == TrustPolicy::kEphemeralCluster) {
        cluster_key_ = fingerprint;
      }
    }
    return Result<LocalIdentity>::Ok(local_.value());
  }

  Result<PeerIdentity> authenticate(
      const discovery::PeerAdvertisement& advertisement,
      const std::string& presented_certificate_pem) override {
    std::shared_lock lock(mutex_);
    if (!local_) {
      return Result<PeerIdentity>::Err(ErrorCode::kFailedPrecondition,
                                       "local identity not initialized");
    }

    // Compute real fingerprint from presented certificate.
        // Fall back to advertisement fingerprint if cert is not parseable.
        std::string peer_fingerprint =
            presented_certificate_pem.empty()
                ? advertisement.public_key_fingerprint
                : compute_fingerprint_sha256(presented_certificate_pem);
        if (peer_fingerprint.empty()) {
          peer_fingerprint = advertisement.public_key_fingerprint;
        }

    switch (policy_) {
      case TrustPolicy::kEphemeralCluster: {
        // Trust-on-first-use: accept any peer in ephemeral mode
        PeerIdentity peer{advertisement.node_id,
                          presented_certificate_pem,
                          peer_fingerprint};
        return Result<PeerIdentity>::Ok(peer);
      }

      case TrustPolicy::kPinnedAllowlist: {
        // Verify fingerprint is in the allowlist
        bool allowed = false;
        {
          std::shared_lock alock(allowlist_mutex_);
          allowed = (pinned_fingerprints_.count(peer_fingerprint) > 0) ||
                    (pinned_node_ids_.count(advertisement.node_id.value) > 0);
        }
        if (!allowed) {
          return Result<PeerIdentity>::Err(
              ErrorCode::kPermissionDenied,
              "node not in pinned allowlist: " + peer_fingerprint);
        }
        PeerIdentity peer{advertisement.node_id,
                          presented_certificate_pem,
                          peer_fingerprint};
        return Result<PeerIdentity>::Ok(peer);
      }

      case TrustPolicy::kPrivateCa: {
        // Verify certificate chain against trust bundle
        if (!trust_bundle_.empty() &&
            !verify_peer_cert(presented_certificate_pem, trust_bundle_)) {
          return Result<PeerIdentity>::Err(
              ErrorCode::kUnauthenticated,
              "peer certificate not trusted by CA bundle");
        }
        PeerIdentity peer{advertisement.node_id,
                          presented_certificate_pem,
                          peer_fingerprint};
        return Result<PeerIdentity>::Ok(peer);
      }
    }
    return Result<PeerIdentity>::Err(ErrorCode::kInternal,
                                     "unknown trust policy");
  }

  // ── Allowlist management ──────────────────────────────────────────────

  void pin_node(const std::string& node_id) {
    std::unique_lock lock(allowlist_mutex_);
    pinned_node_ids_.insert(node_id);
  }

  void pin_fingerprint(const std::string& fingerprint) {
    std::unique_lock lock(allowlist_mutex_);
    pinned_fingerprints_.insert(fingerprint);
  }

  void set_trust_bundle(const std::string& bundle_pem) {
    std::unique_lock lock(mutex_);
    trust_bundle_ = bundle_pem;
  }

  std::string cluster_key() const {
    std::shared_lock lock(mutex_);
    return cluster_key_;
  }

  std::string local_fingerprint() const {
    std::shared_lock lock(mutex_);
    return local_ ? local_->public_key_fingerprint : "";
  }

 private:
  TrustPolicy policy_;
  mutable std::shared_mutex mutex_;
  mutable std::shared_mutex allowlist_mutex_;
  std::optional<LocalIdentity> local_;
  std::string cluster_key_;
  std::string trust_bundle_;
  std::unordered_set<std::string> pinned_node_ids_;
  std::unordered_set<std::string> pinned_fingerprints_;
};

std::unique_ptr<IdentityProvider> make_ephemeral_identity_provider() {
  return std::make_unique<IdentityProviderImpl>(
      TrustPolicy::kEphemeralCluster);
}

std::unique_ptr<IdentityProvider> make_pinned_identity_provider(
    std::vector<std::string> allowlist) {
  auto p = std::make_unique<IdentityProviderImpl>(
      TrustPolicy::kPinnedAllowlist);
  for (auto& id : allowlist) p->pin_node(id);
  return p;
}

std::unique_ptr<IdentityProvider> make_private_ca_identity_provider() {
  return std::make_unique<IdentityProviderImpl>(TrustPolicy::kPrivateCa);
}

}  // namespace socrates::security
