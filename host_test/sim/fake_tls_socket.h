// interfaces::TlsSocket and interfaces::TlsStore on the host (task p3-02): a socket to a scripted
// server (scripted_server.h) that behaves like the modem's - a connection costs time, bytes come
// in pieces of at most 1500 (or of any size a test sets, down to one), a connection can drop after
// N bytes of a body or stall for ever - and refuses a host whose scripted chain does not end at
// the root it was asked to verify against, which is what makes the trust rail a thing the tests
// can see rather than a sentence.
//
// **It is also the modem's socket layer** (task p8-03), which is the habit that made the second
// HTTPS request of a wake fail on the first unit: a connection is on a socket id, the server's
// close is an *event* on that id, and the library can be **late** with it - the `+SQNSH` is
// queued by the URC parser and dispatched by the event task a moment later, by which time the
// next connection has been opened. `close_event_lateness` is how late, counted in connections,
// and the socket drives the same core::net::SocketRotation the board's does. `as_v0_1_0` puts
// v0.1.0's rule back - one socket for every connection, the close flag cleared by every open,
// no debt - so that the test which shows the fault keeps showing it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/net/call_record.h"
#include "core/net/socket_rotation.h"
#include "fakes/fake_clock.h"
#include "interfaces/tls_socket.h"
#include "sim/scripted_server.h"

namespace sim {

// The two sockets the fake turns between, and the one v0.1.0 used for everything.
constexpr int kFirstSocketId = 1;
constexpr int kSecondSocketId = 2;

// How many closes can be in flight at once: one per socket, and room to see a mistake.
constexpr std::size_t kKeptCloses = 4;

class FakeTlsSocket final : public interfaces::TlsSocket {
 public:
  FakeTlsSocket(ScriptedServer& server, fakes::FakeClock& clock)
      : server_(server), clock_(clock), rotation_(clock) {
    // The modem has six sockets; the fake is not held to the one the library's own bookkeeping
    // can reach without asking (components/modem/walter_tls.h), so it takes the two the fault
    // report named. A test that wants the degraded case calls rotation().useIds(1, 0).
    rotation_.useIds(kFirstSocketId, kSecondSocketId);
  }

  // -- the script ----------------------------------------------------------------------------
  bool no_uplink = false;         // open() answers kNoUplink
  bool refuse_tls = false;        // every handshake fails, whatever the root
  bool dial_opaque = false;       // failures answer kDialRefused, as the modem's library does
  std::uint32_t connect_ms = 800; // a connect and a handshake over LTE-M
  std::uint32_t piece_ms = 20;    // what one piece costs
  std::size_t piece_bytes = 1500; // the most one receive() returns
  // The connection drops after this many *body* bytes of the request numbered `drop_at_request`
  // (0-based, counted over this socket's life; -1 for never). After the drop the request counter
  // goes on, so the retry gets a whole answer.
  int drop_at_request = -1;
  std::uint64_t drop_after_body_bytes = 0;
  // The connection drops after this many bytes of the *response* (headers included): for the
  // parser's "closed mid-headers" cases. -1 for never.
  long drop_after_response_bytes = -1;
  bool stall = false;             // nothing ever arrives: receive() runs to its deadline

  // -- the modem's socket layer (task p8-03) -------------------------------------------------
  // How late the library delivers a connection's close, counted in connections. 0 is at once -
  // a well-behaved stack, and what every test written before p8-03 gets. 1 is after the *next*
  // connection has been opened and its flags cleared, which is what the board did; 2 is after
  // the one after that, which with two ids is the same id coming round again. A drop in the
  // middle of a body is never late: that close *is* the connection ending, not this race.
  int close_event_lateness = 0;
  // v0.1.0's rule: one socket id for every connection, the close flag cleared by every open,
  // no owed closes. The second request then fails exactly as it did on the first unit on
  // 6 November 2026 - `the request did not go out`.
  bool as_v0_1_0 = false;
  // A modem that never reports a close at all: the `+SQNSH` of every connection the firmware
  // closed itself is lost. The library queues socket events eight deep with no wait and drops
  // what does not fit, and its event task takes one event per 20 ms
  // (core/net/socket_rotation.h), so a response that rings often enough loses one - and if the
  // one lost is the close, that id is owed a close that is never coming. The review's S2, and
  // what task p9-02 bounds. A drop in the middle of a body is still delivered: that close *is*
  // the connection ending, not this race.
  bool close_event_lost = false;

  // -- what the modem does to a receive (task p15-02) ----------------------------------------
  // The 8 November stall, scripted. On the board the firmware asks the modem for bytes every
  // kReceivePollMs and is answered *nothing buffered yet* until the first byte is there
  // (`+CME ERROR: 4`, or `+SQNSRECV: <id>,0`), and in a stalled call one of those commands goes
  // **unanswered for the library's whole 30 s** and the answer the server sent is never read.
  // None of that can be reached through this fake's own receive loop, because the loop that reads
  // those answers is the board's (components/modem/walter_tls.cpp, receive) and this class is its
  // peer - so these four knobs make the fake say the same things to the same record, and the
  // record is the thing under test (core/net/call_record.h).
  int not_yet_receives = 0;             // receives answered *nothing buffered yet* first
  std::uint32_t not_yet_ms = 50;        // what one of them costs (the board's kReceivePollMs)
  // The receive after them goes unanswered for this long and then fails. **It overruns the
  // deadline**, as the library's blocking call does: the firmware checks the wall before a
  // receive and not during it (interfaces/tls_socket.h), which is why a 30 s stall happens under
  // a 15 s piece deadline and comes out as kSocketFailed and not as a timeout. 0 is never.
  std::uint32_t receive_hangs_ms = 0;
  std::uint8_t receive_state = 2;       // ... with this WalterModemState (2 is its own timeout)
  int receive_cme = -1;                 // ... and this `+CME ERROR: n`, or -1 for none
  // A ring, and the close, handed over this long after the request went out instead of with the
  // response: what the stall needs, because on the board both arrive while the receive hangs.
  std::uint32_t ring_after_ms = 0;
  std::uint16_t ring_bytes = 0;         // 0 is no ring at all
  std::uint32_t close_after_ms = 0;     // 0 keeps the behaviour every other test has

  // Where this socket writes what the modem said, as the board's does (`recordInto` of
  // components/modem/walter_tls.h). sim/fake_http.h hands it the transport's own record.
  void recordInto(core::net::CallRecord* record) { record_ = record; }

  // -- what happened -------------------------------------------------------------------------
  int opens = 0;
  int tls_failures = 0;
  int connect_failures = 0;
  int closes = 0;
  int requests = 0;        // requests that were answered (or dropped)
  std::uint64_t bytes_out = 0;  // delivered to the firmware
  std::uint8_t last_root = 0xFF;
  std::string last_host;
  std::vector<int> socket_ids;  // the id of each connection, in order
  int late_closes = 0;          // close events delivered at a later connection than their own

  // The rule the board runs, for a test to read (and to narrow to one id).
  core::net::SocketRotation& rotation() { return rotation_; }
  const core::net::SocketRotation& rotation() const { return rotation_; }

  // -- interfaces::TlsSocket -----------------------------------------------------------------
  interfaces::TlsOpen open(const char* host, std::uint16_t port, std::uint8_t root,
                           std::uint64_t deadline_ms) override;
  bool isOpen() const override { return open_; }
  bool send(const std::uint8_t* data, std::size_t size, std::uint64_t deadline_ms) override;
  interfaces::TlsRecv receive(std::uint8_t* out, std::size_t max, std::size_t* got,
                              std::uint64_t deadline_ms) override;
  void close() override;

 private:
  bool requestComplete() const;
  void answer();
  // Spends `cost` of the clock, bounded by the deadline; false when the deadline came first.
  bool spend(std::uint32_t cost, std::uint64_t deadline);

  // -- the socket layer -----------------------------------------------------------------------
  // The id this connection takes, with the rule's wait for a close the id still owes.
  int beginConnection(std::uint64_t deadline_ms);
  // Whether the connection in progress has been told its other end closed.
  bool closedNow() const;
  // The server closed: delivered now, or kept for the connection `close_event_lateness` later.
  void theServerClosed();
  // Every kept close whose connection has come.
  void deliverDueCloses();
  void deliverClose(int id);
  void endConnection();

  // -- the scripted modem (task p15-02) -------------------------------------------------------
  // The ring and the close whose moment has come, into the rotation and the record, as the
  // library's event task hands them over.
  void deliverScripted();
  // The clock forward by `cost`, in steps, delivering those as they fall due - and **past the
  // deadline** if that is what the script says, which is what the library's blocking call does.
  void hang(std::uint32_t cost);

  ScriptedServer& server_;
  fakes::FakeClock& clock_;
  core::net::SocketRotation rotation_;
  core::net::CallRecord* record_ = nullptr;
  std::uint64_t went_out_ms_ = 0;    // when the request was whole on the wire
  int not_yet_left_ = 0;             // of not_yet_receives, how many are still to be answered
  bool receive_hung_ = false;        // the one unanswered command of this connection is spent
  bool ring_done_ = false;
  bool close_scripted_ = false;      // the server's close is waiting for close_after_ms
  bool close_done_ = false;
  bool open_ = false;
  std::string request_;
  std::vector<std::uint8_t> response_;
  std::size_t read_ = 0;
  std::size_t cut_at_ = 0;  // response_ is delivered up to here, then the connection "drops"
  bool answered_ = false;
  bool dropped_ = false;
  int socket_id_ = 0;
  bool v010_closed_ = false;  // the one flag of the v0.1.0 rule
  std::uint32_t connections_ = 0;
  struct KeptClose {
    int id = 0;
    std::uint32_t due = 0;  // the connection number at which it is delivered
  };
  KeptClose kept_[kKeptCloses];
  std::size_t kept_count_ = 0;
};

class FakeTlsStore final : public interfaces::TlsStore {
 public:
  bool refuse_writes = false;
  bool refuse_profiles = false;
  int writes = 0;
  int profiles = 0;
  std::vector<std::uint8_t> written_slots;
  std::vector<std::pair<std::uint8_t, std::uint8_t>> configured;  // (profile, slot)
  std::vector<std::string> pems;

  bool writeRoot(std::uint8_t slot, const char* pem, std::uint64_t) override {
    if (refuse_writes) return false;
    ++writes;
    written_slots.push_back(slot);
    pems.push_back(pem != nullptr ? pem : "");
    return true;
  }
  bool configureProfile(std::uint8_t profile, std::uint8_t slot, std::uint64_t) override {
    if (refuse_profiles) return false;
    ++profiles;
    configured.push_back({profile, slot});
    return true;
  }
};

}  // namespace sim
