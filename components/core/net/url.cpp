#include "core/net/url.h"

#include <cstring>

namespace core {
namespace net {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isHostChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.';
}

// `https://` at the start, case-insensitively (RFC 3986 §3.1: schemes are case-insensitive).
bool hasHttpsScheme(const char* text) {
  static const char kScheme[] = "https://";
  for (std::size_t i = 0; kScheme[i] != 0; ++i) {
    if (lower(text[i]) != kScheme[i]) return false;
  }
  return true;
}

// The authority and path of a URL whose scheme has been checked: `rest` is what follows `//`.
UrlError parseAuthorityAndPath(const char* rest, Url* out) {
  const char* p = rest;
  std::size_t host_len = 0;
  while (p[host_len] != 0 && p[host_len] != ':' && p[host_len] != '/' && p[host_len] != '?' &&
         p[host_len] != '#') {
    if (!isHostChar(p[host_len])) return UrlError::kBadHost;
    ++host_len;
  }
  if (host_len == 0) return UrlError::kNoHost;
  if (host_len > kMaxHostLength) return UrlError::kHostTooLong;
  for (std::size_t i = 0; i < host_len; ++i) out->host[i] = lower(p[i]);
  out->host[host_len] = 0;
  p += host_len;

  out->port = 443;
  if (*p == ':') {
    ++p;
    std::uint32_t port = 0;
    std::size_t digits = 0;
    while (*p >= '0' && *p <= '9') {
      port = port * 10 + static_cast<std::uint32_t>(*p - '0');
      if (++digits > 5 || port > 65535) return UrlError::kBadPort;
      ++p;
    }
    if (digits == 0 || port == 0) return UrlError::kBadPort;
    if (*p != 0 && *p != '/' && *p != '?' && *p != '#') return UrlError::kBadPort;
    out->port = static_cast<std::uint16_t>(port);
  }

  // The path and the query, up to a fragment (which is never sent, RFC 7230 §5.1).
  std::size_t n = 0;
  if (*p == 0 || *p == '#') {
    out->path[0] = '/';
    out->path[1] = 0;
    return UrlError::kNone;
  }
  if (*p == '?') {
    out->path[n++] = '/';
  }
  for (; *p != 0 && *p != '#'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c <= 0x20 || c >= 0x7F) return UrlError::kBadPath;
    if (n >= kMaxUrlLength) return UrlError::kTooLong;
    out->path[n++] = *p;
  }
  out->path[n] = 0;
  return UrlError::kNone;
}

// A Url is 1.7 KB: it lives in static storage (core/net/client.h's members) and is cleared field
// by field here, never through a temporary that would land on the main task's 3.5 KB stack.
void clear(Url* out) {
  out->host[0] = 0;
  out->port = 443;
  out->path[0] = 0;
}

}  // namespace

const char* describe(UrlError error) {
  switch (error) {
    case UrlError::kNone: return "ok";
    case UrlError::kEmpty: return "the URL is empty";
    case UrlError::kTooLong: return "the URL is longer than this firmware handles";
    case UrlError::kNotHttps: return "only https URLs are accepted (the chain is verified; nothing else is)";
    case UrlError::kNoHost: return "the URL names no host";
    case UrlError::kHostTooLong: return "the host name is longer than the modem accepts";
    case UrlError::kBadHost: return "the host name has a character a host name cannot";
    case UrlError::kBadPort: return "the port is not a number between 1 and 65535";
    case UrlError::kBadPath: return "the path has a space or a control character";
  }
  return "?";
}

UrlError parseUrl(const char* text, Url* out) {
  if (out == nullptr) return UrlError::kEmpty;
  clear(out);
  if (text == nullptr || text[0] == 0) return UrlError::kEmpty;
  const std::size_t length = std::strlen(text);
  if (length > kMaxUrlLength) return UrlError::kTooLong;
  if (!hasHttpsScheme(text)) return UrlError::kNotHttps;
  return parseAuthorityAndPath(text + 8, out);
}

UrlError resolveLocation(const Url& base, const char* location, Url* out) {
  if (out == nullptr) return UrlError::kEmpty;
  if (location == nullptr || location[0] == 0) return UrlError::kEmpty;
  if (location[0] == '/') {
    if (location[1] == '/') return UrlError::kNotHttps;  // scheme-relative: the scheme is not said
    const std::size_t length = std::strlen(location);
    if (length > kMaxUrlLength) return UrlError::kTooLong;
    // The host and port stay; `out` may be `base` itself, so nothing of base.path is read.
    if (out != &base) {
      std::memcpy(out->host, base.host, sizeof out->host);
      out->port = base.port;
    }
    std::size_t n = 0;
    for (const char* p = location; *p != 0 && *p != '#'; ++p) {
      const unsigned char c = static_cast<unsigned char>(*p);
      if (c <= 0x20 || c >= 0x7F) {
        out->path[0] = '/';
        out->path[1] = 0;
        return UrlError::kBadPath;
      }
      out->path[n++] = *p;
    }
    out->path[n] = 0;
    return UrlError::kNone;
  }
  // Absolute: `location` is never inside `out` (it is the wanted header's own buffer), so parsing
  // straight into `out` is safe; a refusal leaves `out` cleared, which the caller treats as "no
  // URL to follow".
  return parseUrl(location, out);
}

bool sameHost(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  for (;; ++a, ++b) {
    if (lower(*a) != lower(*b)) return false;
    if (*a == 0) return true;
  }
}

bool hostMatches(const char* host, const char* suffix) {
  if (host == nullptr || suffix == nullptr) return false;
  const std::size_t h = std::strlen(host);
  const std::size_t s = std::strlen(suffix);
  if (s == 0 || h < s) return false;
  if (h > s && host[h - s - 1] != '.') return false;
  return sameHost(host + (h - s), suffix);
}

}  // namespace net
}  // namespace core
