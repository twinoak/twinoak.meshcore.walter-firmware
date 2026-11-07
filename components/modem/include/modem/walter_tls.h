// interfaces::TlsSocket and interfaces::TlsStore over `walter-modem` (task p3-02): the modem's own
// TLS over one of its sockets, and its certificate slots and TLS profiles. Thin on purpose, like
// walter_uplink.cpp: each method is the library calls and the mapping of their results, because
// nothing here can be tested without a SIM and a tower. HTTP itself is core/net/http1.h over this
// socket, tested on the host.
//
// -- the library's sockets, read at 1.5.0 -----------------------------------------------------
//
//   socketConfig(id, pdp, mtu, exchange_timeout, conn_timeout, send_delay)   AT+SQNSCFG
//   socketConfigExtended(id, ring_mode, recv_mode, ...)                      AT+SQNSCFGEXT
//   socketConfigSecure(id, true, tls_profile)                                AT+SQNSSCFG
//   socketDial(id, TCP, port, host)                                          AT+SQNSD (command mode)
//   socketSend(id, buf, n, rai)                                              AT+SQNSSENDEXT, n <= 1500
//   socketReceive(id, buf, n)                                                AT+SQNSRECV, n <= 1500
//   socketClose(id)                                                          AT+SQNSH
//   setSocketEventHandler                                                    +SQNSRING, +SQNSH as events
//
// Every one of them blocks the way walter_uplink.h describes (a condition variable with no
// timeout of its own; the command task gives up after CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS), so a
// deadline is checked before each call and an overrun is a late return, never a corruption.
//
// **Two sockets, turn and turn about, and the second one has to be asked for** (task p8-03).
// Consecutive connections must not share a socket id, because a close belonging to the
// connection before this one arrives after this one has been opened (core/net/socket_rotation.h
// has the fault and the rule). The modem has six sockets and the library's
// WALTER_MODEM_MAX_SOCKETS is 6 - but the new API can reach only one of them without asking:
// WalterModemSocket (WalterModem.h) default-initialises `id = 1` for all six entries of
// `_socketSet` and `_socketGet(id)` returns the first entry whose id matches, so socketConfig(1)
// finds entry 0 and socketConfig(2..6) find nothing (NO_FREE_SOCKET, returned without a command
// going out). The only thing that renumbers an entry is `_socketReserve()`
// (proto/WalterSocket.cpp), which takes the topmost FREE entry and gives it `id = i + 1`, and
// the only public way to it is the **deprecated** socketConfig(rsp, cb, args, ...) overload with
// no id. Which id it handed out cannot be read back - the wrapper writes it into
// `rsp->data.profileId` and `_queueModemCMD` then does `*(cmd->rsp) = {}` - so the id is found
// by asking for 6 down to 2 with the new overload and taking the first that is accepted, which
// costs nothing for the ids no entry carries. One reserve per modem start; when the library will
// give none, the rotation runs on one id and its owed-close rule alone.
//
// **A failed dial does not say why.** AT+SQNSD answers ERROR whether the name did not resolve,
// the TCP connection was refused or the TLS handshake failed; the library's WMURCSocketConnRC
// (TLS_ERROR and friends) is declared and never set at 1.5.0. So open() answers kDialRefused and
// the console says so in those words; docs/bench.md §13 is where a host with a wrong root is
// shown to be refused.
//
// **Received bytes are binary-safe.** `+SQNSRECV: <id>,<len>` is followed by exactly `len` raw
// bytes and the library's parser reads them by count (WalterModem.cpp, _parseRxData's
// "payload with a known total size"), so a CR LF inside an image does not end anything. The
// modem's HTTP receive (AT+SQNHTTPRCV) is the one that is *not*: the library omits the size (a
// "known bug: CME ERROR 4") and then reads until "\r\nOK\r\n", which a binary body can contain.
// One more reason the download is over sockets.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/net/socket_rotation.h"
#include "interfaces/clock.h"
#include "interfaces/tls_socket.h"
#include "modem/walter_uplink.h"

namespace modem {

// The socket every entry of the library's set carries by default, and so the one that needs no
// reserve (see above). The first of the two the rotation turns between.
constexpr int kSocketId = 1;

// The PDP context the sockets live in: the one core/uplink defined (walter_uplink.cpp).
constexpr int kSocketPdpContext = 1;

// Where the second id is looked for: the ids `_socketReserve()` can have handed out, highest
// first (it takes the topmost FREE entry, so a fresh set yields WALTER_MODEM_MAX_SOCKETS). Each
// one the library does not carry costs a local false and no AT command.
constexpr int kSecondSocketHighest = 6;
constexpr int kSecondSocketLowest = 2;

// AT+SQNSCFG's parameters. The packet size the modem's stack sends with: 1500 is the Ethernet
// MTU and the most the command allows; our requests are a few hundred bytes. The exchange
// timeout (seconds of silence before the modem closes the socket itself) is 0, disabled: the
// deadlines are this firmware's (core/net/http_over_tls.h). The connect timeout is the modem's
// own bound on AT+SQNSD, in seconds; 30 so that the library's command timeout (30 s) is not what
// ends a dial. The send delay is how long the modem waits to coalesce writes; 0 ms, send at once.
constexpr std::uint16_t kSocketPacketBytes = 1500;
constexpr std::uint16_t kSocketExchangeTimeoutS = 0;
constexpr std::uint16_t kSocketConnectTimeoutS = 30;
constexpr std::uint16_t kSocketSendDelayMs = 0;

// How often the modem is asked for received bytes when it has not rung. A `+SQNSRING` is the
// cue; this is the net under it, so that a missed ring costs a poll and not the deadline.
constexpr std::uint32_t kReceivePollMs = 50;

// The most one socketReceive returns (WalterModem.h: "64-1500").
constexpr std::size_t kSocketReceiveMax = 1500;

class WalterTlsSocket final : public interfaces::TlsSocket {
 public:
  WalterTlsSocket(const WalterUplink& uplink, interfaces::Clock& clock)
      : uplink_(uplink), clock_(clock), rotation_(clock) {
    rotation_.useIds(kSocketId, 0);
  }

  interfaces::TlsOpen open(const char* host, std::uint16_t port, std::uint8_t root,
                           std::uint64_t deadline_ms) override;
  bool isOpen() const override { return open_; }
  bool send(const std::uint8_t* data, std::size_t size, std::uint64_t deadline_ms) override;
  interfaces::TlsRecv receive(std::uint8_t* out, std::size_t max, std::size_t* got,
                              std::uint64_t deadline_ms) override;
  void close() override;

  // The rotation, for the console's counters and so that the library's event handler can reach
  // it: the handler is given `this` as its argument and nothing here is a global any more.
  core::net::SocketRotation& rotation() { return rotation_; }
  const core::net::SocketRotation& rotation() const { return rotation_; }

 private:
  bool before(std::uint64_t deadline_ms) const { return clock_.nowMs() < deadline_ms; }

  // AT+SQNSCFG for `id` with this firmware's parameters. False when the library carries no entry
  // with that id (no command goes out) or the modem refused.
  bool configure(int id);

  // A second socket id out of the library, once per modem start (the header). 0 when it would
  // not give one. Leaves the rotation holding whatever it got.
  void askForASecondSocket(std::uint64_t deadline_ms);

  const WalterUplink& uplink_;
  interfaces::Clock& clock_;
  core::net::SocketRotation rotation_;
  bool handler_set_ = false;
  bool asked_for_a_second_ = false;
  bool open_ = false;
  // Whether the other end has given this connection a byte: what decides, at close(), whether a
  // close is on its way and so whether this id owes one (core/net/socket_rotation.h).
  bool received_ = false;
};

class WalterTlsStore final : public interfaces::TlsStore {
 public:
  WalterTlsStore(const WalterUplink& uplink, interfaces::Clock& clock)
      : uplink_(uplink), clock_(clock) {}

  bool writeRoot(std::uint8_t slot, const char* pem, std::uint64_t deadline_ms) override;

  // **The one call in this firmware that configures a TLS profile.** It validates the chain and
  // the host name (the library's URL_AND_CA level), TLS 1.2, against the certificate in `slot`,
  // and takes no argument that could ask for less. host_test/core/net/test_trust_rail.cpp and
  // tools/verify.sh hold that this is the only place.
  bool configureProfile(std::uint8_t profile, std::uint8_t slot, std::uint64_t deadline_ms) override;

 private:
  const WalterUplink& uplink_;
  interfaces::Clock& clock_;
};

}  // namespace modem
