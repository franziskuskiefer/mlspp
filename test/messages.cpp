#include "test_vectors.h"
#include <doctest/doctest.h>
#include <mls/messages.h>
#include <tls/tls_syntax.h>
#include <mls_vectors/mls_vectors.h>

using namespace mls;
using namespace mls_vectors;

template<typename T, typename... Tp>
void
tls_round_trip(const bytes& vector,
               T& constructed,
               bool reproducible,
               Tp... args)
{
  auto marshaled = tls::marshal(constructed);
  if (reproducible) {
    REQUIRE(vector == marshaled);
  }

  auto unmarshaled = tls::get<T>(vector, args...);
  REQUIRE(constructed == unmarshaled);
  REQUIRE(tls::marshal(unmarshaled) == vector);
}

TEST_CASE("Extensions")
{
  auto cap0 = CapabilitiesExtension{
    { ProtocolVersion::mls10 },
    { CipherSuite::ID::P256_AES128GCM_SHA256_P256 },
    {},
  };
  auto lt0 = LifetimeExtension{ 0xA0A0A0A0A0A0A0A0, 0xB0B0B0B0B0B0B0B0 };
  auto kid0 = KeyIDExtension{ { 0, 1, 2, 3 } };
  auto ph0 = ParentHashExtension{ { 4, 5, 6, 7 } };
  auto tree0 = RatchetTreeExtension{};

  ExtensionList exts;
  exts.add(cap0);
  exts.add(lt0);
  exts.add(kid0);
  exts.add(ph0);
  exts.add(tree0);

  auto cap1 = exts.find<CapabilitiesExtension>();
  auto lt1 = exts.find<LifetimeExtension>();
  auto kid1 = exts.find<KeyIDExtension>();
  auto ph1 = exts.find<ParentHashExtension>();
  auto tree1 = exts.find<RatchetTreeExtension>();

  REQUIRE(cap0 == cap1);
  REQUIRE(lt0 == lt1);
  REQUIRE(kid0 == kid1);
  REQUIRE(ph0 == ph1);
  REQUIRE(tree0 == tree1);
}

// TODO(RLB) Verify sign/verify on:
// * KeyPackage
// * GroupInfo
// * PublicGroupState

TEST_CASE("Messages Interop")
{
  auto tv = MessagesTestVector::create();
  REQUIRE(tv.verify() == std::nullopt);
}
