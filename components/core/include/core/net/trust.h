// Trust on the wire (task p3-02; spec/SYSTEM.md S-12 "TLS is verified"): the roots this firmware
// trusts a server's certificate chain to end at, which root a host is held to, and the loader
// that puts them into the modem's certificate slots **once** rather than at every wake.
//
// -- the roots ---------------------------------------------------------------------------------
//
// keys/tls/*.pem, each with a header - subject, SHA-256 fingerprint, expiry, source, the date it
// was checked, the chains observed - that tools/gen_tls_roots.py holds against the certificate
// itself at build time and turns into tls_roots.h (trust.cpp includes it). Two today:
//
//   kRootIsrgX1         ISRG Root X1 (Let's Encrypt). meshmaster.dk's chain ends here, and so
//                       does release-assets.githubusercontent.com's - the host a GitHub release
//                       download redirects to (both read 2026-10-14).
//   kRootUserTrustEcc   USERTrust ECC Certification Authority (Sectigo). github.com's chain ends
//                       here - the first hop of that redirect.
//
// The modem validates a connection against **one** certificate slot per TLS profile
// (AT+SQNSPCFG takes one caCertificateID), so each root has a slot and a profile of its own and
// a host is held to one root: rootForHost() below. A host whose chain ends elsewhere fails its
// handshake - that is the rail, and docs/bench.md §13 tests it on the modem with a host this
// firmware does not carry a root for.
//
// -- the loader --------------------------------------------------------------------------------
//
// Writing a certificate into the modem is a write to its non-volatile memory; doing it at every
// wake would wear it for nothing. So the loader keeps, in retained storage, the SHA-256 of what
// it wrote into each slot (the PEM, the slot and the profile number together) and writes only
// what differs: a first boot writes everything, every boot after it writes nothing, a build with
// a new root writes that one. Retained storage does not survive a power cut
// (interfaces/retained.h), so a power cut costs one rewrite - task p3-03's settings store in NVS
// is where the record moves to make that zero.
//
// There is **one** function in this firmware that configures a TLS profile, and it is behind
// interfaces::TlsStore::configureProfile: it asks for the chain and the host name to be
// validated and takes no argument that could ask for less (components/modem/walter_tls.cpp;
// host_test/core/net/test_trust.cpp and tools/verify.sh hold both facts against the sources).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/image/crypto/sha256.h"
#include "interfaces/clock.h"
#include "interfaces/retained.h"
#include "interfaces/tls_socket.h"

namespace core {
namespace net {

struct TlsRoot {
  const char* file;         // keys/tls/<file>
  const char* name;         // the header's first line
  const char* subject;      // as openssl prints it: C=..., O=..., CN=...
  const char* sha256_hex;   // over the DER, 64 lowercase hex characters
  const char* expires;      // YYYY-MM-DD, the certificate's notAfter
  const char* source;       // where the PEM was fetched from
  const char* checked;      // YYYY-MM-DD, when the chains were read
  const char* chains;       // the chains observed to end here, on that date
  std::uint32_t der_size;
  const char* pem;          // the PEM text with "\n" line ends: what the modem's slot is written with
};

// The indices into the table, held to the file names by static_asserts in trust.cpp so that a
// new file under keys/tls/ cannot silently move one.
constexpr std::uint8_t kRootIsrgX1 = 0;
constexpr std::uint8_t kRootUserTrustEcc = 1;

// Room for three roots, like the release-key table (spec/FIRMWARE-MANIFEST.md §3): a root is
// replaced by shipping a build that carries both, then one that carries the new alone.
constexpr std::size_t kMaxTlsRoots = 3;

// The modem's certificate slots and TLS profiles the roots go to. The library's own HTTPS example
// says "certificate indexes 0-10 are reserved for Sequans and BlueCherry internal usage" and
// uses 12; TLS profile 1 is BlueCherry's. So root i is written to certificate slot 12 + i and
// validated by TLS profile 2 + i. Named here and nowhere else.
constexpr std::uint8_t kCertificateSlotBase = 12;
constexpr std::uint8_t kTlsProfileBase = 2;
constexpr std::uint8_t certificateSlotOf(std::uint8_t root) {
  return static_cast<std::uint8_t>(kCertificateSlotBase + root);
}
constexpr std::uint8_t tlsProfileOf(std::uint8_t root) {
  return static_cast<std::uint8_t>(kTlsProfileBase + root);
}

// The table this build carries.
std::size_t tlsRootCount();
const TlsRoot* tlsRoot(std::uint8_t index);  // nullptr past the end

// Which root a host's chain must end at. A table of host suffixes (`github.com` matches
// `github.com` and `api.github.com`); a host in no row is held to kRootIsrgX1, the root
// meshmaster.dk and the githubusercontent hosts share. Changing a row is a release, which is
// also what a CA change at the host would need.
struct HostRoot {
  const char* suffix;
  std::uint8_t root;
};
std::uint8_t rootForHost(const char* host);
const HostRoot* hostRoots(std::size_t* count);

// -- the loader --------------------------------------------------------------------------------

// What the loader keeps in retained storage. Plain data (interfaces/retained.h).
constexpr std::uint8_t kTrustRecordVersion = 1;
struct TrustRecord {
  std::uint8_t version = kTrustRecordVersion;
  std::uint8_t count = 0;                        // roots recorded as written
  std::uint8_t spare[2] = {};
  std::uint8_t written[kMaxTlsRoots][image::kSha256Size] = {};  // per root: the hash written
};
static_assert(sizeof(TrustRecord) <= interfaces::kRetainedRecordMax, "the record has to fit a slot");

// How long writing one root and configuring its profile may take: two commands to the modem,
// the first carrying ~2 KB of PEM at 115200 baud (0.2 s of UART). TO BE SET ON THE BENCH
// against `trust tls` on a first boot.
constexpr std::uint32_t kTrustWriteDeadlineMs = 20000;

enum class TrustOutcome : std::uint8_t {
  kUpToDate = 0,  // every slot holds what the record says it holds: nothing was written
  kWritten = 1,   // one or more roots were written and their profiles configured
  kFailed = 2,    // the modem would not take a write or a profile; the record is unchanged for it
  kNotBegun = 3,  // begin() has not been called
};

const char* describe(TrustOutcome outcome);

struct RootStatus {
  bool recorded = false;  // the record says this root was written once
  bool matches = false;   // ... and what it says was written is this build's PEM, slot and profile
};

class TrustLoader {
 public:
  TrustLoader(interfaces::TlsStore& store, interfaces::Retained& retained, interfaces::Clock& clock);

  // Reads the record and hashes this build's roots. Costs nothing of the modem.
  void begin();
  bool begun() const { return begun_; }

  // Make the modem's slots and profiles hold this build's roots, writing only what the record
  // says differs. Called before a connection is opened (core/net/http_over_tls.h); on every call
  // after the first successful one it compares a few hashes and returns kUpToDate.
  TrustOutcome ensure(std::uint64_t deadline_ms);

  // The same for one root: the one a connection is about to be verified against. A request needs
  // its own root and no other, so a root the modem refuses blocks only the hosts that use it
  // (core/net/http_over_tls.cpp). On the first board (23 October 2026) root 1 would not load while
  // root 0 had, and with ensure() in front of every request no request to any host got through.
  TrustOutcome ensureRoot(std::uint8_t root, std::uint64_t deadline_ms);

  RootStatus statusOf(std::uint8_t root) const;
  std::uint32_t writesThisBoot() const { return writes_; }
  bool recordLost() const { return record_lost_; }

  // The record forgotten: the next ensure() writes everything again. The bench's way to see the
  // write happen; nothing automatic calls it.
  void forget();

 private:
  void hashRoot(std::uint8_t root, std::uint8_t out[image::kSha256Size]) const;
  void save();

  interfaces::TlsStore& store_;
  interfaces::Retained& retained_;
  interfaces::Clock& clock_;
  bool begun_ = false;
  bool record_lost_ = false;
  TrustRecord record_{};
  std::uint8_t want_[kMaxTlsRoots][image::kSha256Size] = {};
  std::uint32_t writes_ = 0;
};

}  // namespace net
}  // namespace core
