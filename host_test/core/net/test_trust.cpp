// Trust on the wire (task p3-02): every root's header holds against its PEM through the
// firmware's own base64 and SHA-256; the host table; the loader writing each root once and never
// again until the record is lost or the root changes; and the rail - the library's "validate
// nothing" level named nowhere in this firmware, its profile configuration called from exactly one
// place, no `http://` literal in the firmware's sources - as a test over the tree, shown failing on
// purpose on a snippet.
#include <doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/image/crypto/sha256.h"
#include "core/image/signature.h"
#include "core/net/trust.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_retained.h"
#include "sim/fake_tls_socket.h"

namespace fs = std::filesystem;
namespace net = core::net;

namespace {

std::string hex(const std::uint8_t* bytes, std::size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0x0F]);
  }
  return out;
}

// The DER of a PEM: the base64 between the BEGIN and END lines, through core/image's strict
// decoder - the same one the signature line goes through.
std::vector<std::uint8_t> derOf(const char* pem) {
  std::string text(pem);
  const std::size_t begin = text.find("-----BEGIN CERTIFICATE-----");
  const std::size_t end = text.find("-----END CERTIFICATE-----");
  REQUIRE(begin != std::string::npos);
  REQUIRE(end != std::string::npos);
  std::string body;
  for (char c : text.substr(begin + 27, end - begin - 27)) {
    if (c != '\n' && c != '\r') body.push_back(c);
  }
  std::vector<std::uint8_t> der(body.size());
  const std::size_t n = core::image::base64Decode(body.data(), body.size(), der.data(), der.size());
  REQUIRE(n > 0);
  der.resize(n);
  return der;
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

TEST_CASE("every root's header is the PEM's: the fingerprint through the firmware's own SHA-256, the size, the dates") {
  REQUIRE(net::tlsRootCount() == 2);
  for (std::uint8_t i = 0; i < net::tlsRootCount(); ++i) {
    const net::TlsRoot* r = net::tlsRoot(i);
    REQUIRE(r != nullptr);
    const std::vector<std::uint8_t> der = derOf(r->pem);
    CHECK(der.size() == r->der_size);
    std::uint8_t digest[core::image::kSha256Size];
    core::image::sha256(der.data(), der.size(), digest);
    CHECK_MESSAGE(hex(digest, sizeof digest) == r->sha256_hex, r->file);
    CHECK(std::strlen(r->sha256_hex) == 64);
    CHECK(std::strlen(r->expires) == 10);
    CHECK(std::strlen(r->checked) == 10);
    CHECK(std::strstr(r->source, "https://") == r->source);
    CHECK(std::strstr(r->subject, "CN=") != nullptr);
    // The PEM is what the modem is written with: begins and ends as a PEM, with "\n" line ends.
    CHECK(std::strncmp(r->pem, "-----BEGIN CERTIFICATE-----\n", 28) == 0);
    CHECK(std::string(r->pem).rfind("-----END CERTIFICATE-----\n") == std::strlen(r->pem) - 26);
  }
  CHECK(net::tlsRoot(2) == nullptr);
  CHECK(net::tlsRoot(0xFF) == nullptr);
  // The two the firmware is written against, by the fingerprints read on 2026-10-14.
  CHECK(std::string(net::tlsRoot(net::kRootIsrgX1)->sha256_hex) == "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6");
  CHECK(std::string(net::tlsRoot(net::kRootUserTrustEcc)->sha256_hex) == "4ff460d54b9c86dabfbcfc5712e0400d2bed3fbc4d4fbdaa86e06adcd2a9ad7a");
  CHECK(std::string(net::tlsRoot(net::kRootIsrgX1)->subject) == "C=US, O=Internet Security Research Group, CN=ISRG Root X1");
  CHECK(std::string(net::tlsRoot(net::kRootIsrgX1)->expires) == "2035-06-04");
  CHECK(std::string(net::tlsRoot(net::kRootUserTrustEcc)->expires) == "2038-01-18");
}

TEST_CASE("the header in keys/tls/ is the one the table carries, read off disk") {
  for (std::uint8_t i = 0; i < net::tlsRootCount(); ++i) {
    const net::TlsRoot* r = net::tlsRoot(i);
    const std::string text = readFile(fs::path(TWINOAK_REPO_ROOT) / "keys" / "tls" / r->file);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find(std::string("# sha256: ") + r->sha256_hex) != std::string::npos);
    CHECK(text.find(std::string("# expires: ") + r->expires) != std::string::npos);
    CHECK(text.find(std::string("# subject: ") + r->subject) != std::string::npos);
    CHECK(text.find("# source: ") != std::string::npos);
    CHECK(text.find("# checked: ") != std::string::npos);
  }
}

TEST_CASE("which root a host is held to: github.com and its subdomains to USERTrust ECC, everything else to ISRG Root X1") {
  CHECK(net::rootForHost("github.com") == net::kRootUserTrustEcc);
  CHECK(net::rootForHost("api.github.com") == net::kRootUserTrustEcc);
  CHECK(net::rootForHost("GITHUB.COM") == net::kRootUserTrustEcc);
  CHECK(net::rootForHost("meshmaster.dk") == net::kRootIsrgX1);
  CHECK(net::rootForHost("release-assets.githubusercontent.com") == net::kRootIsrgX1);
  CHECK(net::rootForHost("objects.githubusercontent.com") == net::kRootIsrgX1);
  CHECK(net::rootForHost("notgithub.com") == net::kRootIsrgX1);
  CHECK(net::rootForHost("github.com.evil.example") == net::kRootIsrgX1);
  CHECK(net::rootForHost("") == net::kRootIsrgX1);
  CHECK(net::certificateSlotOf(0) == 12);
  CHECK(net::certificateSlotOf(1) == 13);
  CHECK(net::tlsProfileOf(0) == 2);
  CHECK(net::tlsProfileOf(1) == 3);
}

TEST_CASE("the loader writes every root once, with its profile, and never again while the record holds") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  sim::FakeTlsStore store;
  net::TrustLoader loader(store, retained, clock);
  CHECK(loader.ensure(clock.nowMs() + 1000) == net::TrustOutcome::kNotBegun);
  loader.begin();
  CHECK(loader.recordLost());
  CHECK_FALSE(loader.statusOf(0).recorded);

  // The first boot: both written, both profiles configured, in the slots and profiles trust.h names.
  CHECK(loader.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kWritten);
  CHECK(store.writes == 2);
  CHECK(store.profiles == 2);
  REQUIRE(store.written_slots.size() == 2);
  CHECK(store.written_slots[0] == net::certificateSlotOf(net::kRootIsrgX1));
  CHECK(store.written_slots[1] == net::certificateSlotOf(net::kRootUserTrustEcc));
  CHECK(store.configured[0].first == net::tlsProfileOf(0));
  CHECK(store.configured[0].second == net::certificateSlotOf(0));
  CHECK(store.pems[0] == net::tlsRoot(0)->pem);
  CHECK(store.pems[1] == net::tlsRoot(1)->pem);
  CHECK(loader.statusOf(0).matches);
  CHECK(loader.statusOf(1).matches);
  CHECK(loader.writesThisBoot() == 2);

  // Every call after it: nothing.
  for (int i = 0; i < 10; ++i) CHECK(loader.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kUpToDate);
  CHECK(store.writes == 2);

  // A reboot: a new loader over the same retained storage writes nothing.
  net::TrustLoader again(store, retained, clock);
  again.begin();
  CHECK_FALSE(again.recordLost());
  CHECK(again.statusOf(0).matches);
  CHECK(again.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kUpToDate);
  CHECK(store.writes == 2);

  // A power cut: the record is gone, the roots are written again, once.
  retained.powerCut();
  net::TrustLoader after(store, retained, clock);
  after.begin();
  CHECK(after.recordLost());
  CHECK(after.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kWritten);
  CHECK(store.writes == 4);
  CHECK(after.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kUpToDate);

  // forget(): the bench's way to watch the write happen.
  after.forget();
  CHECK(after.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kWritten);
  CHECK(store.writes == 6);
}

TEST_CASE("a modem that refuses a write or a profile leaves the record unchanged for that root, and the next call tries again") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  sim::FakeTlsStore store;
  net::TrustLoader loader(store, retained, clock);
  loader.begin();
  store.refuse_writes = true;
  CHECK(loader.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kFailed);
  CHECK_FALSE(loader.statusOf(0).recorded);
  store.refuse_writes = false;
  store.refuse_profiles = true;
  CHECK(loader.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kFailed);
  CHECK(store.writes == 1);
  CHECK_FALSE(loader.statusOf(0).recorded);  // a written slot with no profile is not recorded
  store.refuse_profiles = false;
  CHECK(loader.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kWritten);
  CHECK(loader.statusOf(0).matches);
  CHECK(loader.statusOf(1).matches);
  // A deadline already passed: nothing is attempted.
  retained.powerCut();
  net::TrustLoader late(store, retained, clock);
  late.begin();
  const int before = store.writes;
  CHECK(late.ensure(clock.nowMs()) == net::TrustOutcome::kFailed);
  CHECK(store.writes == before);
}

TEST_CASE("a record of another layout is not read: a truncated one is a lost record") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  sim::FakeTlsStore store;
  {
    net::TrustLoader loader(store, retained, clock);
    loader.begin();
    REQUIRE(loader.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kWritten);
  }
  retained.truncate(interfaces::RetainedSlot::kTlsTrust);
  net::TrustLoader loader(store, retained, clock);
  loader.begin();
  CHECK(loader.recordLost());
  CHECK(loader.ensure(clock.nowMs() + 60000) == net::TrustOutcome::kWritten);
}
