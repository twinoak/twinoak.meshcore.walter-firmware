// interfaces::Http over interfaces::TlsSocket (task p3-02): one request is one TLS connection -
// the roots made sure of (core/net/trust.h), the connection opened to the host with the chain
// held to the host's root, the request head written (core/net/http1.h), the body sent, the
// response parsed as it arrives in pieces of at most kReceiveBytes, the connection closed. The
// firmware's only implementation of interfaces::Http, and the host tests' too
// (host_test/sim/fake_http.h is this class over a scripted socket), so that a test of the client
// or the download runs through the same parser the board does.
//
// Deadlines: `total_ms` bounds the whole - connect, send, every receive - and `piece_ms` bounds
// the wait for the next piece of the response. Each receive is given the sooner of the two.
// Zero means the defaults below.
//
// A sensitive request (task p4-03; interfaces/http.h, HttpRequest::sensitive): the head this
// object writes carries the credential, and it is the one copy below the caller's that this
// firmware can reach - the modem's own UART buffer is the library's. So the head is wiped the
// moment it is on the wire and again on every way out of request(), and host_test/core/net/
// test_client.cpp reads this object back to see no byte of the credential left in it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/net/call_record.h"
#include "core/net/http1.h"
#include "core/net/trust.h"
#include "interfaces/clock.h"
#include "interfaces/http.h"
#include "interfaces/tls_socket.h"

namespace core {
namespace net {

// One receive from the modem is at most 1500 bytes (WalterModem.h, socketReceive: "buf_size
// ... 64-1500"); the socket implementation caps there, so a larger buffer buys nothing.
constexpr std::size_t kReceiveBytes = 1500;

// How long one request may take when the caller does not say. A small request to the server is
// a connect (seconds over LTE-M), a handshake (seconds), and a few KB; sixty is generous and is
// what bounds a `http get` on the console. TO BE SET ON THE BENCH.
constexpr std::uint32_t kDefaultTotalMs = 60000;

// How long to wait for the next piece of a body before the connection is given up. The modem
// hands bytes over as they arrive; fifteen seconds of nothing on an LTE-M link that was working a
// moment ago is a stalled connection. TO BE SET ON THE BENCH.
constexpr std::uint32_t kDefaultPieceMs = 15000;

class HttpOverTls final : public interfaces::Http {
 public:
  // `trust` may be nullptr in a test that does not care about the slots; in the firmware it is
  // the loader, and ensure() runs before every connection (and costs nothing after the first).
  HttpOverTls(interfaces::TlsSocket& socket, TrustLoader* trust, interfaces::Clock& clock);

  void request(const interfaces::HttpRequest& request, interfaces::HttpWantedHeader* wanted,
               std::size_t wanted_count, interfaces::HttpResponse* response,
               interfaces::HttpBodySink& sink) override;

  // Counters, for the console and the tests.
  std::uint32_t requests() const { return requests_; }
  std::uint32_t connects() const { return connects_; }

  // Where what the modem said in the request now running is written down (task p15-02,
  // core/net/call_record.h), or nullptr - the default - to write nothing. This object fills in
  // what it knows itself: the request going out, its bytes, and how the call ended. The socket
  // layer below fills in the receiving side and is handed **the same object**, which is the whole
  // of the wiring (main/main.cpp, and host_test/sim/fake_http.h for the tests).
  //
  // **Not a member.** The record is a hundred bytes, and a hundred bytes of `.bss` move
  // `_heap_start` and with it the heap the kept scratch is carved out of - the margin rail R22
  // holds, which has 988 bytes in it (docs/ARCHITECTURE.md §14; phase 13's follow-up says the
  // next task that adds static storage makes room first, and task p14-01 had already spent the
  // room in the scratch). So the firmware takes it from the heap at boot, as it does the
  // library's runner, and this object holds the pointer.
  void recordInto(CallRecord* record) { record_ = record; }
  CallRecord* record() const { return record_; }

 private:
  // The head's bytes to zero, through a volatile so the write is not elided (a sensitive
  // request's credential, task p4-03).
  void wipeHead();

  interfaces::TlsSocket& socket_;
  TrustLoader* trust_;
  interfaces::Clock& clock_;
  ResponseParser parser_;
  CallRecord* record_ = nullptr;
  char head_[kRequestHeadBytes];
  std::uint8_t rx_[kReceiveBytes];
  std::uint32_t requests_ = 0;
  std::uint32_t connects_ = 0;
};

}  // namespace net
}  // namespace core
