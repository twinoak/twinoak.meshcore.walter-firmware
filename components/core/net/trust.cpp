#include "core/net/trust.h"

#include <cstring>

#include "core/net/url.h"
// Generated at build time from keys/tls/*.pem by tools/gen_tls_roots.py (both CMake projects run
// it): generated::kTlsRoots and kTlsRootCount, every header held against its certificate.
#include "tls_roots.h"

namespace core {
namespace net {

namespace {

constexpr bool sameText(const char* a, const char* b) {
  for (;; ++a, ++b) {
    if (*a != *b) return false;
    if (*a == 0) return true;
  }
}

// The indices trust.h names, held to the files: a third PEM sorted between these two would move
// kRootUserTrustEcc, and this is what says so at compile time.
static_assert(generated::kTlsRootCount >= 2, "the two roots the firmware is written against");
static_assert(generated::kTlsRootCount <= kMaxTlsRoots, "room for three roots (trust.h)");
static_assert(sameText(generated::kTlsRoots[kRootIsrgX1].file, "isrg-root-x1.pem"),
              "kRootIsrgX1 must index keys/tls/isrg-root-x1.pem");
static_assert(sameText(generated::kTlsRoots[kRootUserTrustEcc].file, "usertrust-ecc.pem"),
              "kRootUserTrustEcc must index keys/tls/usertrust-ecc.pem");

// Which root each host suffix is held to. github.com's chain ends at Sectigo's USERTrust ECC
// root; everything else this firmware talks to - meshmaster.dk and the githubusercontent host a
// release download redirects to - ends at ISRG Root X1 (read 2026-10-14; keys/tls/*.pem's
// `chains:` lines). A host in no row falls to kRootIsrgX1.
constexpr HostRoot kHostRoots[] = {
    {"github.com", kRootUserTrustEcc},
};

}  // namespace

std::size_t tlsRootCount() { return generated::kTlsRootCount; }

const TlsRoot* tlsRoot(std::uint8_t index) {
  if (index >= generated::kTlsRootCount) return nullptr;
  return &generated::kTlsRoots[index];
}

const HostRoot* hostRoots(std::size_t* count) {
  if (count != nullptr) *count = sizeof kHostRoots / sizeof kHostRoots[0];
  return kHostRoots;
}

std::uint8_t rootForHost(const char* host) {
  for (const HostRoot& row : kHostRoots) {
    if (hostMatches(host, row.suffix)) return row.root;
  }
  return kRootIsrgX1;
}

const char* describe(TrustOutcome outcome) {
  switch (outcome) {
    case TrustOutcome::kUpToDate: return "the modem's slots already hold this build's roots: nothing written";
    case TrustOutcome::kWritten: return "roots written into the modem's slots and their TLS profiles configured";
    case TrustOutcome::kFailed: return "the modem would not take a root or a TLS profile";
    case TrustOutcome::kNotBegun: return "the trust loader has not read its record";
  }
  return "?";
}

TrustLoader::TrustLoader(interfaces::TlsStore& store, interfaces::Retained& retained,
                         interfaces::Clock& clock)
    : store_(store), retained_(retained), clock_(clock) {}

void TrustLoader::hashRoot(std::uint8_t root, std::uint8_t out[image::kSha256Size]) const {
  // The PEM, then the slot and the profile it goes to: a renumbering is a rewrite as much as a
  // new certificate is.
  const TlsRoot* r = tlsRoot(root);
  image::Sha256 hash;
  if (r != nullptr) hash.update(reinterpret_cast<const std::uint8_t*>(r->pem), std::strlen(r->pem));
  const std::uint8_t where[2] = {certificateSlotOf(root), tlsProfileOf(root)};
  hash.update(where, sizeof where);
  hash.finish(out);
}

void TrustLoader::begin() {
  begun_ = true;
  writes_ = 0;
  for (std::uint8_t i = 0; i < kMaxTlsRoots; ++i) {
    if (i < tlsRootCount()) {
      hashRoot(i, want_[i]);
    } else {
      std::memset(want_[i], 0, image::kSha256Size);
    }
  }
  TrustRecord loaded;
  if (retained_.load(interfaces::RetainedSlot::kTlsTrust, &loaded, sizeof loaded) &&
      loaded.version == kTrustRecordVersion && loaded.count <= kMaxTlsRoots) {
    record_ = loaded;
    record_lost_ = false;
  } else {
    record_ = TrustRecord{};
    record_lost_ = true;
  }
}

void TrustLoader::save() {
  retained_.store(interfaces::RetainedSlot::kTlsTrust, &record_, sizeof record_);
}

RootStatus TrustLoader::statusOf(std::uint8_t root) const {
  RootStatus s;
  if (!begun_ || root >= tlsRootCount() || root >= record_.count) return s;
  bool blank = true;
  for (std::size_t i = 0; i < image::kSha256Size; ++i) blank = blank && record_.written[root][i] == 0;
  s.recorded = !blank;
  s.matches = s.recorded && image::sha256Equal(record_.written[root], want_[root]);
  return s;
}

TrustOutcome TrustLoader::ensure(std::uint64_t deadline_ms) {
  if (!begun_) return TrustOutcome::kNotBegun;
  bool wrote = false;
  for (std::uint8_t root = 0; root < tlsRootCount(); ++root) {
    const TrustOutcome one = ensureRoot(root, deadline_ms);
    if (one == TrustOutcome::kFailed || one == TrustOutcome::kNotBegun) return one;
    if (one == TrustOutcome::kWritten) wrote = true;
  }
  return wrote ? TrustOutcome::kWritten : TrustOutcome::kUpToDate;
}

TrustOutcome TrustLoader::ensureRoot(std::uint8_t root, std::uint64_t deadline_ms) {
  if (!begun_) return TrustOutcome::kNotBegun;
  if (root >= tlsRootCount()) return TrustOutcome::kFailed;
  if (statusOf(root).matches) return TrustOutcome::kUpToDate;
  const TlsRoot* r = tlsRoot(root);
  if (clock_.nowMs() >= deadline_ms) return TrustOutcome::kFailed;
  if (!store_.writeRoot(certificateSlotOf(root), r->pem, deadline_ms)) return TrustOutcome::kFailed;
  if (!store_.configureProfile(tlsProfileOf(root), certificateSlotOf(root), deadline_ms)) {
    return TrustOutcome::kFailed;
  }
  // Both took: record it, so that the next boot writes nothing for this root.
  std::memcpy(record_.written[root], want_[root], image::kSha256Size);
  if (record_.count < root + 1) record_.count = static_cast<std::uint8_t>(root + 1);
  save();
  ++writes_;
  return TrustOutcome::kWritten;
}

void TrustLoader::forget() {
  record_ = TrustRecord{};
  save();
}

}  // namespace net
}  // namespace core
