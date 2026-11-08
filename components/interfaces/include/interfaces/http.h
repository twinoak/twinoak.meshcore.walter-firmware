// An HTTP request, as core/ makes one (task p3-02; docs/ARCHITECTURE.md §9): a method, a host, a
// path, a few headers, an optional body - and back a status, the values of the response headers
// the caller named, and the body **as a stream of pieces handed to a sink**, never buffered
// whole: a radio image is 800 KB and the Walter has 341 KB of RAM. A deadline on the whole and
// one on each piece.
//
// The one implementation in the firmware is core/net/http_over_tls.h - HTTP/1.1 written and
// parsed in core/ over interfaces/tls_socket.h, which is what makes the host tests of everything
// above this interface real (host_test/sim/fake_http.h is the same code over a scripted server).
// The task's name for this file is `hal/Http`; `hal/` is `interfaces/` here (task p0-01).
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

enum class HttpMethod : std::uint8_t { kGet = 0, kPost = 1 };

inline const char* describe(HttpMethod method) {
  switch (method) {
    case HttpMethod::kGet: return "GET";
    case HttpMethod::kPost: return "POST";
  }
  return "?";
}

// A request header the caller adds (Range, If-Range, Authorization, Content-Type ...). The
// writer adds Host, Connection and Content-Length itself.
struct HttpHeader {
  const char* name = nullptr;
  const char* value = nullptr;
};

// How many a request may carry. Range, If-Range, Authorization and a content type is the most
// this firmware ever sends at once; a fifth is a mistake worth a compile error.
constexpr std::size_t kMaxRequestHeaders = 4;

struct HttpRequest {
  HttpMethod method = HttpMethod::kGet;
  const char* host = nullptr;  // the name the certificate must match, and the Host header
  std::uint16_t port = 443;
  const char* path = "/";      // the request target: path and query, already escaped
  std::uint8_t root = 0;       // which root of core/net/trust.h the chain must end at
  HttpHeader headers[kMaxRequestHeaders] = {};
  std::size_t header_count = 0;
  const std::uint8_t* body = nullptr;  // kPost only
  std::size_t body_size = 0;
  std::uint32_t total_ms = 0;  // the whole request, connect to last byte
  std::uint32_t piece_ms = 0;  // between two pieces of the body
  // A header of this request is a credential (the agent's bearer token, task p4-03): the
  // transport wipes its own copy of the request head the moment the head is on the wire, and
  // again on every way out, so that the credential outlives the send in no buffer of its own.
  // The caller's own copy is the caller's to wipe (core/net/client.h, Options::sensitive).
  bool sensitive = false;
};

// A response header the caller wants: its name, and the caller's own buffer for the value. The
// value is copied as it streams past; one longer than the buffer is cut and `truncated` says so.
struct HttpWantedHeader {
  const char* name = nullptr;
  char* value = nullptr;
  std::size_t capacity = 0;
  bool present = false;
  bool truncated = false;
};

// Where the body goes. `piece` returns false to stop: the connection is then closed, whatever is
// left of the body is never read, and the outcome says kSinkStopped - how a caller bounds a
// response without buffering it (core/net/client.h). **The response's status and the wanted
// headers are filled in before the first piece is handed over** (the headers come first on the
// wire), so a sink that holds a pointer to the HttpResponse can decide what to do with the body
// from what the status says - which is how core/net/download.h refuses to append the body of a
// 200 where a 206 was asked for.
class HttpBodySink {
 public:
  virtual ~HttpBodySink() = default;
  virtual bool piece(const std::uint8_t* data, std::size_t size) = 0;
};

enum class HttpOutcome : std::uint8_t {
  kOk = 0,            // a status line, the headers and the whole body were received
  kNoUplink = 1,      // the modem is not up: nothing was sent
  kBadRequest = 2,    // the request could not be written: a host or path too long, no host
  kConnectFailed = 3, // the host could not be reached before any TLS
  kTlsFailed = 4,     // the handshake failed: the chain, the name or the version
  kSendFailed = 5,    // the request did not go out
  kTimeout = 6,       // the whole or a piece took longer than its deadline
  kMalformed = 7,     // the response is not HTTP/1.1 this firmware reads (core/net/http1.h says what)
  kClosed = 8,        // the connection closed before the body was complete
  kSinkStopped = 9,   // the sink refused a piece: the body was cut off there
  kTooLong = 10,      // the response's headers exceed what the parser holds
  kTrustFailed = 11,  // the roots could not be loaded into the modem's slots (core/net/trust.h)
  kDialRefused = 12,  // the modem refused the connection without saying whether it was the host
                      // or the handshake (interfaces/tls_socket.h, TlsOpen::kDialRefused)
  // The socket itself failed while the response was being read: TlsRecv::kError, which on the
  // board is the modem answering an error to `AT+SQNSRECV` or not answering it at all before the
  // library's own 30 s command timeout (`WalterModem: Command time-out (TX)`). **Told apart from
  // kClosed on purpose** (task p9-02): both were reported as *the connection closed before the
  // body was complete*, and the 7 November stalls - 30935 ms and 31055 ms, each one AT command
  // that went unanswered - therefore read on the console as a server that hung up. One of the two
  // sentences is about the other end and the other is about this board's modem.
  kSocketFailed = 13,
};

inline const char* describe(HttpOutcome outcome) {
  switch (outcome) {
    case HttpOutcome::kOk: return "ok";
    case HttpOutcome::kNoUplink: return "the uplink is not up";
    case HttpOutcome::kBadRequest: return "the request could not be written (a host or path too long, or none)";
    case HttpOutcome::kConnectFailed: return "the host could not be reached";
    case HttpOutcome::kTlsFailed: return "the TLS handshake failed: the chain, the name or the version";
    case HttpOutcome::kSendFailed: return "the request did not go out";
    case HttpOutcome::kTimeout: return "the deadline passed";
    case HttpOutcome::kMalformed: return "the response is not HTTP/1.1 this firmware reads";
    case HttpOutcome::kClosed: return "the connection closed before the body was complete";
    case HttpOutcome::kSinkStopped: return "the body was cut off (the sink refused the rest)";
    case HttpOutcome::kTooLong: return "the response's headers are longer than this firmware holds";
    case HttpOutcome::kTrustFailed: return "the roots could not be loaded into the modem";
    case HttpOutcome::kDialRefused: return "the modem refused the connection: the host was unreachable or the TLS handshake failed (the library does not say which)";
    case HttpOutcome::kSocketFailed: return "the modem stopped answering for this socket before the body was complete";
  }
  return "?";
}

struct HttpResponse {
  HttpOutcome outcome = HttpOutcome::kOk;
  std::uint16_t status = 0;             // 0 until a status line was read
  bool has_content_length = false;
  std::uint32_t content_length = 0;     // when has_content_length
  bool chunked = false;                 // Transfer-Encoding: chunked was used
  std::uint64_t body_bytes = 0;         // bytes handed to the sink
  std::uint32_t elapsed_ms = 0;
};

class Http {
 public:
  virtual ~Http() = default;

  // One request. The wanted headers are filled in as the response's headers stream past; the body
  // goes to the sink in pieces. The response's outcome says what happened; kOk and a status are
  // the only combination in which the body was whole.
  virtual void request(const HttpRequest& request, HttpWantedHeader* wanted, std::size_t wanted_count,
                       HttpResponse* response, HttpBodySink& sink) = 0;
};

}  // namespace interfaces
