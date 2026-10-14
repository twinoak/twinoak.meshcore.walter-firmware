// A TLS connection to a server, as core/ sees it (task p3-02; docs/ARCHITECTURE.md §9): open to a
// host and port with the chain **verified against one named root and the host name checked**,
// send bytes, receive bytes, close. Behind it is the modem's own TLS over its sockets
// (components/modem/walter_tls.cpp: `walter-modem`'s socketConfigSecure, socketDial, socketSend,
// socketReceive) or, in the host tests, a scripted server (host_test/sim/fake_tls_socket.h).
//
// Why a socket and not the modem's HTTP (tasks/phase-3/reports/02.md has the evidence): the
// modem's HTTP client reports a response's status, content type and length and nothing else - no
// `Location`, no `Content-Range`, no `ETag` - so a redirect cannot be followed, a range cannot be
// checked and a changed file cannot be noticed. HTTP/1.1 itself is small; it is written and read
// in core/net/http1.h over this interface, where every line of it has a host test.
//
// **There is no way to ask for an unverified connection.** open() takes the index of a root in
// core/net/trust.h's table and nothing else about trust; the implementation maps it to a TLS
// profile that was configured - by the one function this firmware has for it - to validate the
// chain and the host name, TLS 1.2. The host tests' fake refuses a host whose scripted chain does
// not end at the root asked for, so that the client is tested against TLS failures too.
//
// A deadline is an absolute monotonic millisecond count (interfaces::Clock::nowMs() plus the
// patience), the latest moment the call may return. The real implementation can only check the
// wall before and after a blocking library call (components/modem/walter_uplink.h says why), so
// a caller treats a late return as a return, never as a corruption.
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

// What came of open().
enum class TlsOpen : std::uint8_t {
  kOpen = 0,
  kNoUplink = 1,       // the modem is not up: nothing was tried
  kNoSuchRoot = 2,     // the root index names no root this build carries
  kBusy = 3,           // a connection is already open on this object
  kConnectFailed = 4,  // DNS, TCP or the modem refused before any TLS; the host may not exist
  kTlsFailed = 5,      // the handshake failed: the chain does not end at the root, the name does
                       // not match, the version is too old, or the server broke it off
  kTimeout = 6,        // nothing by the deadline
  kDialRefused = 7,    // the modem refused the dial and did not say why: at walter-modem 1.5.0 a
                       // failed AT+SQNSD is one ERROR whether the name did not resolve, the TCP
                       // connection failed or the TLS handshake did (tasks/phase-3/reports/02.md)
};

inline const char* describe(TlsOpen outcome) {
  switch (outcome) {
    case TlsOpen::kDialRefused: return "the modem refused the connection and does not say whether the host was unreachable or the TLS handshake failed";
    case TlsOpen::kOpen: return "open";
    case TlsOpen::kNoUplink: return "the uplink is not up";
    case TlsOpen::kNoSuchRoot: return "no such root in this build's table";
    case TlsOpen::kBusy: return "a connection is already open";
    case TlsOpen::kConnectFailed: return "the host could not be reached (DNS, TCP, or the modem refused)";
    case TlsOpen::kTlsFailed: return "the TLS handshake failed: the chain does not end at the root, the name does not match, or the server broke it off";
    case TlsOpen::kTimeout: return "nothing by the deadline";
  }
  return "?";
}

// What came of receive().
enum class TlsRecv : std::uint8_t {
  kData = 0,     // `*got` bytes were written to `out` (at least one)
  kClosed = 1,   // the other end closed the connection; nothing more will come
  kTimeout = 2,  // nothing arrived by the deadline; the connection is still open
  kError = 3,    // the connection is gone, or the modem would not answer
};

inline const char* describe(TlsRecv outcome) {
  switch (outcome) {
    case TlsRecv::kData: return "data";
    case TlsRecv::kClosed: return "closed by the other end";
    case TlsRecv::kTimeout: return "nothing by the deadline";
    case TlsRecv::kError: return "the connection is gone";
  }
  return "?";
}

class TlsSocket {
 public:
  virtual ~TlsSocket() = default;

  // Connect to `host`:`port` with TLS, the chain verified against the root at `root` in
  // core/net/trust.h's table and the certificate's name checked against `host`. One connection
  // at a time per object.
  virtual TlsOpen open(const char* host, std::uint16_t port, std::uint8_t root,
                       std::uint64_t deadline_ms) = 0;

  virtual bool isOpen() const = 0;

  // Send all of `size` bytes, or fail. The modem takes at most 1500 a command; an implementation
  // splits, the caller does not care.
  virtual bool send(const std::uint8_t* data, std::size_t size, std::uint64_t deadline_ms) = 0;

  // Up to `max` bytes into `out`, waiting until the deadline for the first of them. kData with
  // `*got` set, or why there is none.
  virtual TlsRecv receive(std::uint8_t* out, std::size_t max, std::size_t* got,
                          std::uint64_t deadline_ms) = 0;

  // Close whatever is open. Never fails loudly; harmless when nothing is.
  virtual void close() = 0;
};

// The modem's certificate slots and TLS profiles, as core/net/trust.h's loader drives them:
// write a root's PEM into a numbered slot, and configure a numbered TLS profile to validate the
// chain against that slot **and** the host name. There is no argument for a validation level,
// because there is nothing to choose: the implementation's one call to the library's profile
// configuration asks for everything, every time (components/modem/walter_tls.cpp; a host test
// and tools/verify.sh hold that there is exactly one such call and that the library's other
// validation levels are named nowhere in this firmware).
class TlsStore {
 public:
  virtual ~TlsStore() = default;

  // The PEM into certificate slot `slot` of the modem's non-volatile store. Blocking; false when
  // the modem would not take it or is not started.
  virtual bool writeRoot(std::uint8_t slot, const char* pem, std::uint64_t deadline_ms) = 0;

  // TLS profile `profile` validates the chain against the certificate in `slot`, checks the host
  // name, TLS 1.2. False when the modem refused.
  virtual bool configureProfile(std::uint8_t profile, std::uint8_t slot,
                                std::uint64_t deadline_ms) = 0;
};

}  // namespace interfaces
