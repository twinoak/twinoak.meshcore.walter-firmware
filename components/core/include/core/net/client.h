// The HTTPS client (task p3-02): a small GET or POST by URL over interfaces::Http, with the rules
// a unit on a mast needs and a browser would not: `https://` only - a URL with any other scheme
// is refused before the modem is touched (core/net/url.h); redirects followed, **at most three,
// only to https**, and an `Authorization` header **never carried to a different host**; the
// status read into a verdict - 2xx *ok*, 401 and 403 *denied* and not retried in this wake, 429
// and 5xx *later* with `Retry-After` honoured inside a cap, everything else *failed*; and a body
// larger than the caller's bound **cut off and reported**, never buffered.
//
// It is portable and tested against host_test/sim/fake_http.h; the download (download.h) and
// the fetch of a release (core/update/fetch.h) are written over it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/net/url.h"
#include "interfaces/clock.h"
#include "interfaces/http.h"
#include "interfaces/text_out.h"

namespace core {
namespace net {

// Redirects followed before giving up. A GitHub release download is one (github.com to
// release-assets.githubusercontent.com, 2026-10-14); a chain of four is a server that has lost
// its way.
constexpr std::uint8_t kMaxRedirects = 3;

// The most a `Retry-After` is believed. A server asking for a day is honoured for an hour and
// then asked again: a unit that trusts a server's clock for its own duty cycle can be put to
// sleep by a typo. A Retry-After that is not a number of seconds (an HTTP-date) is the cap.
constexpr std::uint32_t kRetryAfterCapS = 3600;

// The `Authorization` value this firmware can carry: `Bearer ` and a token of a few hundred
// characters (task p3-03's setting). Bounded here so the request head's own bound is a sentence.
constexpr std::size_t kMaxAuthorizationLength = 512;

// How long a redirect's body is read before it is cut: a 3xx carries at most a line of HTML.
constexpr std::uint64_t kRedirectBodyBound = 4096;

enum class Verdict : std::uint8_t {
  kOk = 0,        // 2xx, the body delivered whole (or cut by the bound: see kTooLarge)
  kDenied = 1,    // 401 or 403: not retried in this wake
  kLater = 2,     // 429 or 5xx: try again after `retry_after_s`
  kFailed = 3,    // everything else: another status, a transport failure, a redirect refused
  kTooLarge = 4,  // 2xx, and the body passed the caller's bound: cut off there
  kRefusedUrl = 5,  // not an https URL this firmware can send
};

const char* describe(Verdict verdict);

struct Options {
  // `Bearer <token>` or any other credential, sent as `Authorization` to the first host and
  // dropped when a redirect leaves it. nullptr for none.
  const char* authorization = nullptr;
  // The caller's own request headers (Range, If-Range ...). At most kMaxRequestHeaders minus
  // the Authorization and Content-Type the client adds itself.
  const interfaces::HttpHeader* headers = nullptr;
  std::size_t header_count = 0;
  // The response headers the caller wants (ETag, Content-Range ...). `Location` and
  // `Retry-After` are the client's own and need not be asked for.
  interfaces::HttpWantedHeader* wanted = nullptr;
  std::size_t wanted_count = 0;
  // The most body the caller will take; a longer one is cut there and the verdict is kTooLarge.
  std::uint64_t bound = 0xFFFFFFFFull;
  // Deadlines; zero is core/net/http_over_tls.h's default.
  std::uint32_t total_ms = 0;
  std::uint32_t piece_ms = 0;
  bool follow_redirects = true;
  // The Authorization value is a credential that must not outlive the request (task p4-03,
  // spec/SYSTEM.md S-12): the transport wipes the request head it wrote the moment it is on the
  // wire (interfaces/http.h, HttpRequest::sensitive). The client keeps no copy of any header -
  // `authorization` is the caller's pointer and is copied into the head alone - so this flag
  // reaches the one buffer below the caller's that holds the bytes. The caller wipes its own.
  bool sensitive = false;
  // Where the transport writes the response as it arrives, when the caller wants to watch it: the
  // status and the wanted headers are in it before the first body piece (interfaces/http.h), so
  // a sink that must decide from the status - the download's, which refuses to append a 200 -
  // points at this. nullptr: the client keeps one of its own.
  interfaces::HttpResponse* response = nullptr;
};

struct Result {
  Verdict verdict = Verdict::kFailed;
  std::uint16_t status = 0;
  interfaces::HttpOutcome outcome = interfaces::HttpOutcome::kOk;
  UrlError url_error = UrlError::kNone;
  const char* why = "";          // one sentence, static text
  std::uint8_t redirects = 0;
  std::uint32_t retry_after_s = 0;  // kLater: capped; 0 when the server did not say
  std::uint64_t bytes = 0;          // body bytes delivered to the sink, last response
  bool has_content_length = false;
  std::uint32_t content_length = 0;
  bool chunked = false;
  std::uint32_t elapsed_ms = 0;     // all requests of this call
  bool authorization_dropped = false;  // a redirect left the host: the credential stayed behind
  char final_host[kMaxHostLength + 1] = {};
};

class Client {
 public:
  Client(interfaces::Http& http, interfaces::Clock& clock);

  Result get(const char* url, const Options& options, interfaces::HttpBodySink& sink);
  Result post(const char* url, const std::uint8_t* body, std::size_t body_size,
              const char* content_type, const Options& options, interfaces::HttpBodySink& sink);

  // One line per request made - method, host, a few characters of the path, the status, the
  // bytes, the time - to this sink, when set. **Never the headers**: the token a request carries
  // is in no line here, and host_test/core/net/test_client.cpp greps for it.
  void setTrace(interfaces::TextOut* trace) { trace_ = trace; }

  std::uint32_t requests() const { return requests_; }

 private:
  Result run(interfaces::HttpMethod method, const char* url, const std::uint8_t* body,
             std::size_t body_size, const char* content_type, const Options& options,
             interfaces::HttpBodySink& sink);
  void trace(const interfaces::HttpRequest& request, const interfaces::HttpResponse& response);

  interfaces::Http& http_;
  interfaces::Clock& clock_;
  interfaces::TextOut* trace_ = nullptr;
  std::uint32_t requests_ = 0;
  // Static storage, not the stack: a Url is 1.7 KB and the main task's stack is 3.5 KB.
  Url url_;
  char first_host_[kMaxHostLength + 1] = {};
  char location_[kMaxUrlLength + 1] = {};
  char retry_after_[32] = {};
};

}  // namespace net
}  // namespace core
