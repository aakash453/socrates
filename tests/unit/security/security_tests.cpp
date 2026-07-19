#include <gtest/gtest.h>

#include <memory>

#include "socrates/security/identity_provider.h"

namespace socrates {
namespace {

using namespace security;

TEST(SecurityIdentity, Ephemeral_GeneratesUniqueId) {
  auto p1 = make_ephemeral_identity_provider();
  auto p2 = make_ephemeral_identity_provider();

  auto id1 = p1->local_identity();
  auto id2 = p2->local_identity();

  ASSERT_TRUE(id1.is_ok());
  ASSERT_TRUE(id2.is_ok());
  EXPECT_NE(id1.value().node_id.value, id2.value().node_id.value);
  EXPECT_FALSE(id1.value().public_key_fingerprint.empty());
}

TEST(SecurityIdentity, Ephemeral_StableAcrossCalls) {
  auto p = make_ephemeral_identity_provider();
  auto id1 = p->local_identity();
  auto id2 = p->local_identity();

  ASSERT_TRUE(id1.is_ok());
  ASSERT_TRUE(id2.is_ok());
  EXPECT_EQ(id1.value().node_id.value, id2.value().node_id.value);
  EXPECT_EQ(id1.value().public_key_fingerprint,
            id2.value().public_key_fingerprint);
}

TEST(SecurityIdentity, Ephemeral_AuthenticatesPeer) {
  auto p = make_ephemeral_identity_provider();
  ASSERT_TRUE(p->local_identity().is_ok());

  discovery::PeerAdvertisement adv;
  adv.node_id = NodeId{"peer-123"};
  adv.public_key_fingerprint = "fp-abc";

  auto result = p->authenticate(adv, "cert-pem");
  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.value().node_id.value, "peer-123");
  EXPECT_EQ(result.value().public_key_fingerprint, "fp-abc");
}

TEST(SecurityIdentity, Pinned_RejectsNonAllowlisted) {
  auto p = make_pinned_identity_provider({"allowed-node"});
  ASSERT_TRUE(p->local_identity().is_ok());

  discovery::PeerAdvertisement adv;
  adv.node_id = NodeId{"evil-node"};
  adv.public_key_fingerprint = "fp-evil";

  auto result = p->authenticate(adv, "cert-pem");
  ASSERT_TRUE(result.is_err());
  EXPECT_EQ(result.error_code(), ErrorCode::kPermissionDenied);
}

TEST(SecurityIdentity, Pinned_AcceptsAllowlisted) {
  auto p = make_pinned_identity_provider({"good-node"});
  ASSERT_TRUE(p->local_identity().is_ok());

  discovery::PeerAdvertisement adv;
  adv.node_id = NodeId{"good-node"};
  adv.public_key_fingerprint = "fp-good";

  auto result = p->authenticate(adv, "cert-pem");
  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.value().node_id.value, "good-node");
}

TEST(SecurityIdentity, PrivateCa_AcceptsAny) {
  auto p = make_private_ca_identity_provider();
  ASSERT_TRUE(p->local_identity().is_ok());

  discovery::PeerAdvertisement adv;
  adv.node_id = NodeId{"any-node"};
  adv.public_key_fingerprint = "fp-any";

  auto result = p->authenticate(adv, "cert-pem");
  ASSERT_TRUE(result.is_ok());
}

}  // namespace
}  // namespace socrates
