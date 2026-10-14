// A URL, taken apart (task p3-02): `https://host[:port]/path[?query]`. **`https` is the only
// scheme this firmware accepts** - the agent firmware's rule, and S-12's "TLS is verified": a
// URL with any other scheme is refused here, before the modem is touched, and a redirect to one
// is refused by the client the same way (core/net/client.h). Nothing here allocates; the parts
// are copied into the caller's Url.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace net {

// The longest URL this firmware handles. A GitHub release download redirects to
// release-assets.githubusercontent.com with a signed query string of about a thousand
// characters (observed 2026-10-14, tasks/phase-3/reports/02.md); 1536 leaves room for it to
// grow. The buffer is static storage, never a stack's.
constexpr std::size_t kMaxUrlLength = 1536;

// The host part. The modem's own bound is 127 (WalterModem.h, WALTER_MODEM_HOSTNAME_MAX_SIZE);
// RFC 1035's is 253. The smaller one, because the modem is where the name goes.
constexpr std::size_t kMaxHostLength = 127;

enum class UrlError : std::uint8_t {
  kNone = 0,
  kEmpty,
  kTooLong,          // over kMaxUrlLength
  kNotHttps,         // any scheme but `https`, or none
  kNoHost,           // `https://` and then no host
  kHostTooLong,
  kBadHost,          // a character a host name cannot contain
  kBadPort,          // not 1..65535
  kBadPath,          // a space or a control character in the path
};

const char* describe(UrlError error);

struct Url {
  char host[kMaxHostLength + 1] = {};
  std::uint16_t port = 443;
  // The request target as it goes on the request line: the path and the query, `/` when the
  // URL had none. Points into the caller-provided storage given to parseUrl.
  char path[kMaxUrlLength + 1] = {};
};

// Takes `text` apart into `out`. Refuses what is not an https URL this firmware can send.
UrlError parseUrl(const char* text, Url* out);

// A redirect's Location against the URL it answered: absolute `https://...` is taken whole,
// `/path` keeps the host and port, and anything else - `http://`, a scheme-relative `//host`, a
// relative path - is refused. The result is written into `out`, which may be `base` itself.
UrlError resolveLocation(const Url& base, const char* location, Url* out);

// Case-insensitive host equality (RFC 3986 §3.2.2: host names are case-insensitive).
bool sameHost(const char* a, const char* b);

// Whether `host` is `suffix` or ends with `.suffix` (so `api.github.com` matches `github.com`
// and `notgithub.com` does not), case-insensitively.
bool hostMatches(const char* host, const char* suffix);

}  // namespace net
}  // namespace core
