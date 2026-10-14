// interfaces::TlsSocket and interfaces::TlsStore on the host (task p3-02): a socket to a scripted
// server (scripted_server.h) that behaves like the modem's - a connection costs time, bytes come
// in pieces of at most 1500 (or of any size a test sets, down to one), a connection can drop after
// N bytes of a body or stall for ever - and refuses a host whose scripted chain does not end at
// the root it was asked to verify against, which is what makes the trust rail a thing the tests
// can see rather than a sentence.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fakes/fake_clock.h"
#include "interfaces/tls_socket.h"
#include "sim/scripted_server.h"

namespace sim {

class FakeTlsSocket final : public interfaces::TlsSocket {
 public:
  FakeTlsSocket(ScriptedServer& server, fakes::FakeClock& clock) : server_(server), clock_(clock) {}

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

  // -- what happened -------------------------------------------------------------------------
  int opens = 0;
  int tls_failures = 0;
  int connect_failures = 0;
  int closes = 0;
  int requests = 0;        // requests that were answered (or dropped)
  std::uint64_t bytes_out = 0;  // delivered to the firmware
  std::uint8_t last_root = 0xFF;
  std::string last_host;

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

  ScriptedServer& server_;
  fakes::FakeClock& clock_;
  bool open_ = false;
  std::string request_;
  std::vector<std::uint8_t> response_;
  std::size_t read_ = 0;
  std::size_t cut_at_ = 0;  // response_ is delivered up to here, then the connection "drops"
  bool answered_ = false;
  bool dropped_ = false;
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
