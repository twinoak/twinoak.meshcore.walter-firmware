// The URL parser (task p3-02): https and nothing else, the host, the port, the path and query, the
// fragment dropped, the bounds, a redirect's Location resolved - and every refusal with its reason.
#include <doctest.h>

#include <string>

#include "core/net/url.h"

using core::net::kMaxUrlLength;
using core::net::parseUrl;
using core::net::resolveLocation;
using core::net::Url;
using core::net::UrlError;

TEST_CASE("an https URL comes apart into host, port and path") {
  Url u;
  REQUIRE(parseUrl("https://meshmaster.dk/api/v1/thing?x=1#frag", &u) == UrlError::kNone);
  CHECK(std::string(u.host) == "meshmaster.dk");
  CHECK(u.port == 443);
  CHECK(std::string(u.path) == "/api/v1/thing?x=1");

  REQUIRE(parseUrl("HTTPS://Example.COM:8443", &u) == UrlError::kNone);
  CHECK(std::string(u.host) == "example.com");  // hosts are case-insensitive: lower-cased once
  CHECK(u.port == 8443);
  CHECK(std::string(u.path) == "/");

  REQUIRE(parseUrl("https://h.example?only=query", &u) == UrlError::kNone);
  CHECK(std::string(u.path) == "/?only=query");

  REQUIRE(parseUrl("https://h.example#only-fragment", &u) == UrlError::kNone);
  CHECK(std::string(u.path) == "/");
}

TEST_CASE("every scheme but https is refused before anything else is looked at") {
  Url u;
  CHECK(parseUrl("http://meshmaster.dk/", &u) == UrlError::kNotHttps);
  CHECK(parseUrl("ftp://meshmaster.dk/", &u) == UrlError::kNotHttps);
  CHECK(parseUrl("meshmaster.dk/", &u) == UrlError::kNotHttps);
  CHECK(parseUrl("//meshmaster.dk/", &u) == UrlError::kNotHttps);
  CHECK(parseUrl("https:/meshmaster.dk/", &u) == UrlError::kNotHttps);
  CHECK(parseUrl("", &u) == UrlError::kEmpty);
  CHECK(parseUrl(nullptr, &u) == UrlError::kEmpty);
  CHECK(std::string(core::net::describe(UrlError::kNotHttps)).find("https") != std::string::npos);
}

TEST_CASE("the refusals: no host, a bad host, a bad port, a bad path, too long") {
  Url u;
  CHECK(parseUrl("https://", &u) == UrlError::kNoHost);
  CHECK(parseUrl("https:///path", &u) == UrlError::kNoHost);
  CHECK(parseUrl("https://host_name.example/", &u) == UrlError::kBadHost);
  CHECK(parseUrl("https://user@host.example/", &u) == UrlError::kBadHost);
  CHECK(parseUrl("https://host.example:0/", &u) == UrlError::kBadPort);
  CHECK(parseUrl("https://host.example:65536/", &u) == UrlError::kBadPort);
  CHECK(parseUrl("https://host.example:abc/", &u) == UrlError::kBadPort);
  CHECK(parseUrl("https://host.example:/", &u) == UrlError::kBadPort);
  CHECK(parseUrl("https://host.example/a b", &u) == UrlError::kBadPath);
  CHECK(parseUrl("https://host.example/a\tb", &u) == UrlError::kBadPath);
  std::string host(core::net::kMaxHostLength + 1, 'h');
  CHECK(parseUrl(("https://" + host + "/").c_str(), &u) == UrlError::kHostTooLong);
  std::string path(kMaxUrlLength + 1, 'p');
  CHECK(parseUrl(("https://h.example/" + path).c_str(), &u) == UrlError::kTooLong);
  // Exactly the bound is accepted: a GitHub release download's signed redirect is ~1 KB.
  std::string ok = "https://h.example/";
  ok += std::string(kMaxUrlLength - ok.size(), 'p');
  CHECK(parseUrl(ok.c_str(), &u) == UrlError::kNone);
}

TEST_CASE("a redirect's Location: absolute https taken whole, a path keeps the host, the rest refused") {
  Url base;
  REQUIRE(parseUrl("https://github.com/twinoak/releases/download/v1/m.json", &base) == UrlError::kNone);
  Url next;
  CHECK(resolveLocation(base, "https://release-assets.githubusercontent.com/x/y?sig=abc", &next) == UrlError::kNone);
  CHECK(std::string(next.host) == "release-assets.githubusercontent.com");
  CHECK(std::string(next.path) == "/x/y?sig=abc");

  CHECK(resolveLocation(base, "/elsewhere?q=1", &next) == UrlError::kNone);
  CHECK(std::string(next.host) == "github.com");
  CHECK(next.port == 443);
  CHECK(std::string(next.path) == "/elsewhere?q=1");

  // In place: the base is the output, as the client does it.
  CHECK(resolveLocation(base, "/again", &base) == UrlError::kNone);
  CHECK(std::string(base.host) == "github.com");
  CHECK(std::string(base.path) == "/again");

  CHECK(resolveLocation(base, "http://insecure.example/", &next) == UrlError::kNotHttps);
  CHECK(resolveLocation(base, "//scheme-relative.example/", &next) == UrlError::kNotHttps);
  CHECK(resolveLocation(base, "relative/path", &next) == UrlError::kNotHttps);
  CHECK(resolveLocation(base, "", &next) == UrlError::kEmpty);
  CHECK(resolveLocation(base, "/with space", &next) == UrlError::kBadPath);
}

TEST_CASE("host comparison and suffix matching are case-insensitive and label-aware") {
  using core::net::hostMatches;
  using core::net::sameHost;
  CHECK(sameHost("GitHub.com", "github.com"));
  CHECK_FALSE(sameHost("github.com", "github.co"));
  CHECK(hostMatches("github.com", "github.com"));
  CHECK(hostMatches("api.github.com", "github.com"));
  CHECK(hostMatches("API.GITHUB.COM", "github.com"));
  CHECK_FALSE(hostMatches("notgithub.com", "github.com"));
  CHECK_FALSE(hostMatches("github.com.evil.example", "github.com"));
  CHECK_FALSE(hostMatches("github.com", ""));
}
