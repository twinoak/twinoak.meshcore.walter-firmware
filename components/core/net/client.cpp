#include "core/net/client.h"

#include <cstdio>
#include <cstring>

#include "core/net/trust.h"
#include "core/version.h"

namespace core {
namespace net {

namespace {

// The body of a response the client itself consumes (a redirect): dropped, and cut at a bound
// so that a server cannot make a 302 cost a megabyte.
class DropSink final : public interfaces::HttpBodySink {
 public:
  explicit DropSink(std::uint64_t bound) : bound_(bound) {}
  bool piece(const std::uint8_t*, std::size_t size) override {
    seen_ += size;
    return seen_ <= bound_;
  }

 private:
  std::uint64_t bound_;
  std::uint64_t seen_ = 0;
};

// The caller's sink behind the caller's bound: a piece that would pass it is cut there and the
// rest refused, which the transport reports as kSinkStopped and the client as kTooLarge.
class BoundedSink final : public interfaces::HttpBodySink {
 public:
  BoundedSink(interfaces::HttpBodySink& inner, std::uint64_t bound) : inner_(inner), bound_(bound) {}
  bool piece(const std::uint8_t* data, std::size_t size) override {
    if (seen_ >= bound_) {
      cut_ = true;
      return false;
    }
    const std::uint64_t room = bound_ - seen_;
    const std::size_t n = size > room ? static_cast<std::size_t>(room) : size;
    seen_ += n;
    if (!inner_.piece(data, n)) {
      inner_stopped_ = true;
      return false;
    }
    if (n < size) {
      cut_ = true;
      return false;
    }
    return true;
  }
  bool cut() const { return cut_; }
  bool innerStopped() const { return inner_stopped_; }

 private:
  interfaces::HttpBodySink& inner_;
  std::uint64_t bound_;
  std::uint64_t seen_ = 0;
  bool cut_ = false;
  bool inner_stopped_ = false;
};

bool isRedirect(std::uint16_t status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Two header names, as HTTP compares them (RFC 7230 §3.2: case-insensitive).
bool sameHeaderName(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  for (;; ++a, ++b) {
    const char x = *a >= 'A' && *a <= 'Z' ? static_cast<char>(*a - 'A' + 'a') : *a;
    const char y = *b >= 'A' && *b <= 'Z' ? static_cast<char>(*b - 'A' + 'a') : *b;
    if (x != y) return false;
    if (x == 0) return true;
  }
}

// The **file** a request was for, for a refusal's sentence: the last segment of the path, and
// never the query - a query is nobody's business in a message that reaches a job page (task
// p10-05). `/agent/v1/firmware/3/manifest.json` is `manifest.json`; a path that ends in a slash
// has no file name of its own and stands for itself.
void fileOf(const char* path, std::size_t* at, std::size_t* len) {
  *at = 0;
  *len = 0;
  if (path == nullptr || path[0] == 0) return;
  std::size_t end = 0;
  while (path[end] != 0 && path[end] != '?') ++end;
  std::size_t begin = end;
  while (begin > 0 && path[begin - 1] != '/') --begin;
  if (begin == end) {  // nothing after the last slash: the path itself
    begin = 0;
  }
  *at = begin;
  *len = end - begin;
}

// The `detail` of an RFC 7807 problem body, copied out as one printable line. Not a JSON parser:
// the member is found at the top level of the object the server sent
// (`"detail":"..."`, meshmaster.dk's `src/MeshMaster.Web/Api/ApiProblems.cs`), the two escapes
// its writer can produce are undone, every byte that is not printable ASCII becomes a space - a
// CR or a LF would otherwise forge a console line - and a value that does not fit is cut.
// False when there is no such member, or no closing quote inside what was kept.
bool problemDetail(const char* body, char* out, std::size_t cap) {
  out[0] = 0;
  if (body == nullptr || cap == 0) return false;
  static const char kMember[] = "\"detail\"";
  const char* at = std::strstr(body, kMember);
  if (at == nullptr) return false;
  at += sizeof kMember - 1;
  while (*at == ' ' || *at == '\t') ++at;
  if (*at != ':') return false;
  ++at;
  while (*at == ' ' || *at == '\t') ++at;
  if (*at != '"') return false;
  ++at;
  std::size_t n = 0;
  for (; *at != 0 && *at != '"' && n + 1 < cap; ++at) {
    char c = *at;
    if (c == '\\') {
      ++at;
      if (*at == 0) return false;
      switch (*at) {
        case 'n': case 'r': case 't': c = ' '; break;
        case 'u':  // a code point this firmware does not guess at; its four digits go with it
          c = '?';
          for (int skip = 0; skip < 4 && at[1] != 0 && at[1] != '"'; ++skip) ++at;
          break;
        default: c = *at; break;
      }
    }
    out[n++] = c >= 0x20 && c <= 0x7E ? c : ' ';
  }
  out[n] = 0;
  if (*at != '"' && n + 1 < cap) return false;  // the quote never came: not a value we read
  return n != 0;
}

// Whether a sentence the **server** wrote holds the credential this request carried. A server
// that echoed an `Authorization` header into its problem body would otherwise put the token into
// a job's posted `reason` and into a console line (spec/SYSTEM.md S-12, task p4-03's rail), so
// such a detail is dropped whole rather than trimmed. Both the whole header value and the secret
// after the scheme word are looked for.
bool holdsCredential(const char* text, const char* authorization) {
  if (text == nullptr || text[0] == 0 || authorization == nullptr || authorization[0] == 0) return false;
  if (std::strstr(text, authorization) != nullptr) return true;
  const char* space = std::strrchr(authorization, ' ');
  if (space == nullptr || space[1] == 0) return false;
  return std::strstr(text, space + 1) != nullptr;
}

// `Retry-After` as seconds: digits, or an HTTP-date which this firmware does not read and takes
// as the cap. Absent is 0: "the server did not say".
std::uint32_t retryAfterSeconds(const char* text, bool present) {
  if (!present || text == nullptr || text[0] == 0) return 0;
  std::uint64_t value = 0;
  std::size_t digits = 0;
  for (const char* p = text; *p != 0; ++p) {
    if (*p < '0' || *p > '9') return kRetryAfterCapS;
    value = value * 10 + static_cast<std::uint64_t>(*p - '0');
    if (++digits > 9) return kRetryAfterCapS;
  }
  return value > kRetryAfterCapS ? kRetryAfterCapS : static_cast<std::uint32_t>(value);
}

}  // namespace

const char* describe(Verdict verdict) {
  switch (verdict) {
    case Verdict::kOk: return "ok";
    case Verdict::kDenied: return "denied (401 or 403): not retried in this wake";
    case Verdict::kLater: return "later (429 or 5xx): try again after Retry-After, within the cap";
    case Verdict::kFailed: return "failed";
    case Verdict::kTooLarge: return "the response is larger than the bound: cut off there";
    case Verdict::kRefusedUrl: return "the URL was refused before anything was sent";
  }
  return "?";
}

Client::Client(interfaces::Http& http, interfaces::Clock& clock) : http_(http), clock_(clock) {}

Result Client::get(const char* url, const Options& options, interfaces::HttpBodySink& sink) {
  return run(interfaces::HttpMethod::kGet, url, nullptr, 0, nullptr, options, sink);
}

Result Client::post(const char* url, const std::uint8_t* body, std::size_t body_size,
                    const char* content_type, const Options& options,
                    interfaces::HttpBodySink& sink) {
  return run(interfaces::HttpMethod::kPost, url, body, body_size, content_type, options, sink);
}

void Client::trace(const interfaces::HttpRequest& request, const interfaces::HttpResponse& response) {
  if (trace_ == nullptr) return;
  // The method, the host, the start of the path, the status, the bytes and the time. No header
  // of the request is in this line, so a token a request carried is not either.
  char line[160];
  const int n = std::snprintf(line, sizeof line, "  %s %s%.40s%s -> %u %s, %lu bytes, %lu ms\n",
                              interfaces::describe(request.method), request.host, request.path,
                              std::strlen(request.path) > 40 ? "..." : "",
                              static_cast<unsigned>(response.status),
                              response.outcome == interfaces::HttpOutcome::kOk ? "ok" : interfaces::describe(response.outcome),
                              static_cast<unsigned long>(response.body_bytes),
                              static_cast<unsigned long>(response.elapsed_ms));
  if (n > 0) trace_->write(line, static_cast<std::size_t>(n) < sizeof line ? static_cast<std::size_t>(n) : sizeof line - 1);
}

void Client::refuse(Result* result, const char* path, const char* clause, const char* authorization) {
  std::size_t at = 0;
  std::size_t len = 0;
  fileOf(path, &at, &len);
  const int n = std::snprintf(refusal_, sizeof refusal_, "http %u for %.*s%s%s",
                              static_cast<unsigned>(result->status), static_cast<int>(len),
                              path != nullptr ? path + at : "", clause[0] != 0 ? ": " : "", clause);
  std::size_t used = n > 0 ? static_cast<std::size_t>(n) : 0;
  if (used >= sizeof refusal_) used = sizeof refusal_ - 1;
  // What the server said about it, when it answered problem details. A detail that holds the
  // credential is dropped whole; one that does not fit is cut where it fits.
  char detail[kMaxRefusalLength];
  if (problem_len_ != 0 && problemDetail(problem_, detail, sizeof detail) &&
      !holdsCredential(detail, authorization) && used + 3 < sizeof refusal_) {
    // Copied rather than printed: what is left of the buffer is not a constant, and a cut is
    // what is wanted here rather than a warning about one.
    const char* join = clause[0] != 0 ? "; " : ": ";
    std::size_t at = used;
    refusal_[at++] = join[0];
    refusal_[at++] = join[1];
    for (std::size_t i = 0; detail[i] != 0 && at + 1 < sizeof refusal_; ++i) refusal_[at++] = detail[i];
    refusal_[at] = 0;
  }
  // Nothing of the server's body outlives the sentence in this object.
  std::memset(problem_, 0, sizeof problem_);
  problem_len_ = 0;
  result->why = refusal_;
}

Result Client::run(interfaces::HttpMethod method, const char* url, const std::uint8_t* body,
                   std::size_t body_size, const char* content_type, const Options& options,
                   interfaces::HttpBodySink& sink) {
  Result result;
  const std::uint64_t started = clock_.nowMs();
  result.url_error = parseUrl(url, &url_);
  if (result.url_error != UrlError::kNone) {
    result.verdict = Verdict::kRefusedUrl;
    result.why = describe(result.url_error);
    return result;
  }
  if (options.authorization != nullptr && std::strlen(options.authorization) > kMaxAuthorizationLength) {
    result.verdict = Verdict::kRefusedUrl;
    result.why = "the Authorization value is longer than this firmware carries";
    return result;
  }
  std::memcpy(first_host_, url_.host, sizeof first_host_);

  // The response headers the client reads itself, beside the caller's.
  interfaces::HttpWantedHeader own[2] = {};
  own[0].name = "Location";
  own[0].value = location_;
  own[0].capacity = sizeof location_;
  own[1].name = "Retry-After";
  own[1].value = retry_after_;
  own[1].capacity = sizeof retry_after_;
  // The caller's wanted headers and the client's own go to the transport as one array; the
  // client's two are copied after the caller's, so a caller may ask for at most four.
  constexpr std::size_t kMaxCallerWanted = 4;
  interfaces::HttpWantedHeader wanted[kMaxCallerWanted + 2];
  std::size_t wanted_count = 0;
  if (options.wanted != nullptr) {
    if (options.wanted_count > kMaxCallerWanted) {
      result.why = "more wanted headers than the client carries";
      return result;
    }
    for (std::size_t i = 0; i < options.wanted_count; ++i) wanted[wanted_count++] = options.wanted[i];
  }
  wanted[wanted_count++] = own[0];
  wanted[wanted_count++] = own[1];

  for (std::uint8_t hop = 0;; ++hop) {
    interfaces::HttpRequest request;
    request.method = method;
    request.host = url_.host;
    request.port = url_.port;
    request.path = url_.path;
    request.root = rootForHost(url_.host);
    request.total_ms = options.total_ms;
    request.piece_ms = options.piece_ms;
    request.sensitive = options.sensitive;
    request.header_count = 0;
    // Which build is asking, first of all, so that no caller's header can crowd it out
    // (kVersionHeader): every request this firmware makes says it, and one whose caller named
    // the header itself with a value of its own keeps that and is not given a second.
    bool caller_says_version = false;
    for (std::size_t i = 0; options.headers != nullptr && i < options.header_count; ++i) {
      if (sameHeaderName(options.headers[i].name, kVersionHeader) &&
          options.headers[i].value != nullptr && options.headers[i].value[0] != 0) {
        caller_says_version = true;
      }
    }
    if (!caller_says_version) {
      request.headers[request.header_count++] = interfaces::HttpHeader{kVersionHeader, core::version()};
    }
    if (options.headers != nullptr) {
      for (std::size_t i = 0; i < options.header_count; ++i) {
        // A caller's version header with no value is dropped: the client's own has gone in its
        // place, and two of the same name would read at the server as "a, b" and then as no
        // version at all once it is over 64 characters (`AgentV1.VersionOf`).
        if (!caller_says_version && sameHeaderName(options.headers[i].name, kVersionHeader)) continue;
        if (request.header_count >= interfaces::kMaxRequestHeaders) {
          result.why = "more request headers than one request carries";
          return result;
        }
        request.headers[request.header_count++] = options.headers[i];
      }
    }
    // The credential goes to the host the caller named and to no other (a redirect to another
    // host is somebody else's server, whatever it says).
    const bool same_host = sameHost(url_.host, first_host_);
    if (options.authorization != nullptr && options.authorization[0] != 0) {
      if (same_host) {
        if (request.header_count >= interfaces::kMaxRequestHeaders) {
          result.why = "no room for the Authorization header";
          return result;
        }
        request.headers[request.header_count++] = interfaces::HttpHeader{"Authorization", options.authorization};
      } else {
        result.authorization_dropped = true;
      }
    }
    if (method == interfaces::HttpMethod::kPost) {
      request.body = body;
      request.body_size = body_size;
      if (content_type != nullptr && content_type[0] != 0) {
        if (request.header_count >= interfaces::kMaxRequestHeaders) {
          result.why = "no room for the Content-Type header";
          return result;
        }
        request.headers[request.header_count++] = interfaces::HttpHeader{"Content-Type", content_type};
      }
    }

    // Where the body goes: to the caller inside the bound, unless this turns out to be a
    // redirect, whose body is dropped. The transport tells the sink nothing of the status, so
    // the decision is made per piece by looking at the response the transport is filling in.
    interfaces::HttpResponse local;
    interfaces::HttpResponse& response = options.response != nullptr ? *options.response : local;
    BoundedSink bounded(sink, options.bound);
    DropSink dropped(kRedirectBodyBound);
    class Chooser final : public interfaces::HttpBodySink {
     public:
      Chooser(const interfaces::HttpResponse& r, BoundedSink& b, DropSink& d, bool follow,
              char* problem, std::size_t capacity, std::size_t* length)
          : r_(r), b_(b), d_(d), follow_(follow), problem_(problem), capacity_(capacity),
            length_(length) {}
      bool piece(const std::uint8_t* data, std::size_t size) override {
        if (follow_ && isRedirect(r_.status)) return d_.piece(data, size);
        keep(data, size);
        return b_.piece(data, size);
      }

     private:
      // The first bytes of the body of a response that is **not** a 2xx: the problem details a
      // refusal's sentence reads its `detail` out of (task p10-05). The body still goes to the
      // caller's sink exactly as before; this is a copy of its beginning, and the client wipes
      // it the moment the sentence is composed. The status is in the response before the first
      // piece (interfaces/http.h), so there is no need to keep a 2xx's.
      void keep(const std::uint8_t* data, std::size_t size) {
        if (r_.status >= 200 && r_.status < 300) return;
        while (size != 0 && *length_ + 1 < capacity_) {
          problem_[(*length_)++] = static_cast<char>(*data++);
          --size;
        }
        problem_[*length_] = 0;
      }

      const interfaces::HttpResponse& r_;
      BoundedSink& b_;
      DropSink& d_;
      bool follow_;
      char* problem_;
      std::size_t capacity_;
      std::size_t* length_;
    } chooser(response, bounded, dropped, options.follow_redirects, problem_, sizeof problem_,
              &problem_len_);

    ++requests_;
    problem_len_ = 0;
    std::memset(problem_, 0, sizeof problem_);
    http_.request(request, wanted, wanted_count, &response, chooser);
    trace(request, response);
    // The client's own two were filled in place; the caller's are copied back.
    for (std::size_t i = 0; options.wanted != nullptr && i < options.wanted_count; ++i) {
      options.wanted[i] = wanted[i];
    }
    const interfaces::HttpWantedHeader& location = wanted[wanted_count - 2];
    const interfaces::HttpWantedHeader& retry_after = wanted[wanted_count - 1];

    result.status = response.status;
    result.outcome = response.outcome;
    result.bytes = response.body_bytes;
    result.has_content_length = response.has_content_length;
    result.content_length = response.content_length;
    result.chunked = response.chunked;
    result.redirects = hop;
    result.elapsed_ms = static_cast<std::uint32_t>(clock_.nowMs() - started);
    std::memcpy(result.final_host, url_.host, sizeof result.final_host);

    // A redirect: the body was dropped (or cut at its bound, which is the same thing here).
    if (options.follow_redirects && isRedirect(response.status) &&
        (response.outcome == interfaces::HttpOutcome::kOk ||
         response.outcome == interfaces::HttpOutcome::kSinkStopped ||
         response.outcome == interfaces::HttpOutcome::kClosed ||
         response.outcome == interfaces::HttpOutcome::kSocketFailed)) {
      if (hop >= kMaxRedirects) {
        result.verdict = Verdict::kFailed;
        result.why = "more redirects than this firmware follows (three)";
        return result;
      }
      if (!location.present || location.truncated) {
        result.verdict = Verdict::kFailed;
        result.why = location.present ? "the redirect's Location is longer than this firmware holds"
                                      : "a redirect without a Location";
        return result;
      }
      const UrlError next = resolveLocation(url_, location_, &url_);
      if (next != UrlError::kNone) {
        result.verdict = Verdict::kFailed;
        result.url_error = next;
        result.why = next == UrlError::kNotHttps ? "a redirect to something other than https: refused"
                                                 : describe(next);
        return result;
      }
      // 303 is "see other": a GET. 301 and 302 after a POST are taken the way every browser
      // takes them, as a GET without the body; 307 and 308 keep the method and the body.
      if (response.status == 303 || ((response.status == 301 || response.status == 302) &&
                                     method == interfaces::HttpMethod::kPost)) {
        method = interfaces::HttpMethod::kGet;
        body = nullptr;
        body_size = 0;
        content_type = nullptr;
      }
      continue;
    }

    // Not a redirect: the transport's outcome first, then the status. A body the sink cut or
    // refused is a failed *2xx*; for any other status the body is an error page nobody wanted,
    // and the status alone decides (a 403 whose page the sink refused is still "denied").
    const bool success = response.status >= 200 && response.status < 300;
    switch (response.outcome) {
      case interfaces::HttpOutcome::kOk:
        break;
      case interfaces::HttpOutcome::kSinkStopped:
        if (success) {
          if (bounded.cut()) {
            result.verdict = Verdict::kTooLarge;
            result.why = "the body passed the bound and was cut off there";
            return result;
          }
          result.verdict = Verdict::kFailed;
          result.why = bounded.innerStopped() ? "the sink refused the body" : interfaces::describe(response.outcome);
          return result;
        }
        break;
      default:
        result.verdict = Verdict::kFailed;
        result.why = interfaces::describe(response.outcome);
        return result;
    }
    if (success) {
      result.verdict = Verdict::kOk;
      result.why = "ok";
      return result;
    }
    // A refusal says what it was: the status, the file it was for, and what the server said
    // about it (task p10-05). *a status this firmware does not act on* was what job 22841 came
    // back with on 8 November, and it named neither the 400 nor the manifest.
    if (response.status == 401 || response.status == 403) {
      result.verdict = Verdict::kDenied;
      refuse(&result, request.path,
             response.status == 401 ? "the server wants a credential this unit did not give"
                                    : "the server refused this unit",
             options.authorization);
      return result;
    }
    if (response.status == 429 || (response.status >= 500 && response.status < 600)) {
      result.verdict = Verdict::kLater;
      result.retry_after_s = retryAfterSeconds(retry_after_, retry_after.present);
      refuse(&result, request.path,
             response.status == 429 ? "too many requests; later" : "the server is not well; later",
             options.authorization);
      return result;
    }
    result.verdict = Verdict::kFailed;
    refuse(&result, request.path,
           isRedirect(response.status) ? "a redirect, and this request does not follow them" : "",
           options.authorization);
    return result;
  }
}

}  // namespace net
}  // namespace core
