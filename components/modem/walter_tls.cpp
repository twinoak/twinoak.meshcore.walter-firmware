#include "modem/walter_tls.h"

#include "esp_log.h"

#include "WalterModem.h"

#include "core/net/trust.h"

namespace modem {

namespace {

// What the library's event task tells us about a socket: a ring (bytes arrived) and a close. It
// is handed the socket object, so the flags live in that object's rotation
// (core/net/socket_rotation.h) - per id, and a close for the id not in use changes nothing about
// the one in use. That is task p8-03's whole business: before it there was one flag for socket
// 1, cleared by every open, and the previous connection's close landed on the next connection.
void socketEvent(WMSocketEventType event, const WMSocketEventData* data, void* args) {
  WalterTlsSocket* self = static_cast<WalterTlsSocket*>(args);
  if (self == nullptr || data == nullptr) return;
  if (event == WALTER_MODEM_SOCKET_EVENT_RING) self->rotation().onRing(data->conn_id);
  if (event == WALTER_MODEM_SOCKET_EVENT_DISCONNECTED) self->rotation().onClose(data->conn_id);
}

}  // namespace

// ------------------------------------------------------------------------------------ the socket

bool WalterTlsSocket::configure(int id) {
  // The socket's parameters, every time: a previous life of the modem may have left others.
  return WalterModem::socketConfig(id, kSocketPdpContext, kSocketPacketBytes,
                                   kSocketExchangeTimeoutS, kSocketConnectTimeoutS,
                                   kSocketSendDelayMs);
}

void WalterTlsSocket::askForASecondSocket(std::uint64_t deadline_ms) {
  asked_for_a_second_ = true;
  // The deprecated overload with no id is the only public way to `_socketReserve()`, which is
  // the only thing that gives an entry of the library's set an id other than 1 (walter_tls.h).
  // Deprecated and warned about since 1.5.0; there is no supported call that does this, and
  // without it consecutive connections could only share socket 1.
  //
  // The overload also logs `DEPRECATION: Use socketConfig(socket_id, pdp_ctx_id, mtu,
  // exchange_timeout, conn_timeout, send_delay_ms, rsp, cb, args) instead` at warning level
  // (WalterSocket.cpp), which is what the first unit's console printed on the first connection of
  // a boot and what the review of phase 8 asked about: it reads as a mistake and is not one -
  // every socketConfig this firmware makes **is** the current signature, and this one call is a
  // reserve and not a config. Every other deprecated call of the library is one this firmware
  // does not make, so the tag can go, for the right reason and in the place that has it.
  esp_log_level_set("DEPRECATION", ESP_LOG_NONE);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  const bool reserved =
      WalterModem::socketConfig(nullptr, nullptr, nullptr, kSocketPdpContext, kSocketPacketBytes,
                                kSocketExchangeTimeoutS, kSocketConnectTimeoutS, kSocketSendDelayMs);
#pragma GCC diagnostic pop
  if (!reserved) return;
  // Which id it was. `rsp->data.profileId` is zeroed by `_queueModemCMD` before the command even
  // goes out, so it is found by asking: an id no entry carries is a local false, and the first
  // one accepted is the entry just reserved.
  for (int id = kSecondSocketHighest; id >= kSecondSocketLowest; --id) {
    if (!before(deadline_ms)) return;
    if (configure(id)) {
      rotation_.useIds(kSocketId, id);
      return;
    }
  }
}

interfaces::TlsOpen WalterTlsSocket::open(const char* host, std::uint16_t port, std::uint8_t root,
                                          std::uint64_t deadline_ms) {
  if (open_) return interfaces::TlsOpen::kBusy;
  if (host == nullptr || host[0] == 0) return interfaces::TlsOpen::kConnectFailed;
  if (uplink_.state() != interfaces::UplinkState::kUp) return interfaces::TlsOpen::kNoUplink;
  if (root >= core::net::tlsRootCount()) return interfaces::TlsOpen::kNoSuchRoot;
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  if (!handler_set_) {
    WalterModem::setSocketEventHandler(socketEvent, this);
    handler_set_ = true;
  }
  if (!asked_for_a_second_) askForASecondSocket(deadline_ms);
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;

  // The id this connection takes, and its parameters. A configure that fails on the second id is
  // a modem whose socket set has been reset under us (its ids are all 1 again), so the second is
  // given up and the first - which every entry carries - is used instead; the next modem start
  // asks again.
  int id = rotation_.nextId();
  if (!configure(id)) {
    if (id == kSocketId) return interfaces::TlsOpen::kConnectFailed;
    asked_for_a_second_ = false;
    rotation_.useIds(kSocketId, 0);
    id = kSocketId;
    if (!configure(id)) return interfaces::TlsOpen::kConnectFailed;
  }
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  // The rule: wait out a close this id still owes, then start with its flags clear.
  if (rotation_.begin(id, deadline_ms) != id) return interfaces::TlsOpen::kConnectFailed;
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  // Rings that say how much arrived, bytes as bytes, no listening, bytes sent as bytes.
  if (!WalterModem::socketConfigExtended(id, WALTER_MODEM_SOCKET_RING_MODE_DATA_AMOUNT,
                                         WALTER_MODEM_SOCKET_RECV_MODE_TEXT, 0,
                                         WALTER_MODEM_SOCKET_LISTEN_MODE_DISABLED,
                                         WALTER_MODEM_SOCKET_SEND_MODE_TEXT)) {
    rotation_.cancel();
    return interfaces::TlsOpen::kConnectFailed;
  }
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  // TLS on, with the profile that validates the chain against this root (core/net/trust.h:
  // tlsProfileOf). The profile itself was configured by WalterTlsStore::configureProfile.
  if (!WalterModem::socketConfigSecure(id, true, core::net::tlsProfileOf(root))) {
    rotation_.cancel();
    return interfaces::TlsOpen::kConnectFailed;
  }
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  // The dial: DNS, TCP and the TLS handshake, in command mode. One ERROR for all three failures
  // (walter_tls.h), hence kDialRefused.
  if (!WalterModem::socketDial(id, WALTER_MODEM_SOCKET_PROTO_TCP, port, host)) {
    // Nothing answered, so nothing will close: cancel(). A `+SQNSH` the modem may yet send for
    // a half-opened connection sets this id's flag and the next begin() on it clears it again.
    rotation_.cancel();
    return interfaces::TlsOpen::kDialRefused;
  }
  received_ = false;
  open_ = true;
  return interfaces::TlsOpen::kOpen;
}

bool WalterTlsSocket::send(const std::uint8_t* data, std::size_t size, std::uint64_t deadline_ms) {
  if (!open_ || data == nullptr) return false;
  const int id = rotation_.currentId();
  while (size > 0) {
    if (!before(deadline_ms) || rotation_.closedSeen()) return false;
    const std::size_t n = size > kSocketReceiveMax ? kSocketReceiveMax : size;
    // The library takes a non-const pointer and does not write through it (it hands the bytes to
    // uart_write_bytes after the modem's prompt).
    if (!WalterModem::socketSend(id, const_cast<std::uint8_t*>(data), static_cast<std::uint16_t>(n),
                                 WALTER_MODEM_RAI_NO_INFO)) {
      return false;
    }
    data += n;
    size -= n;
  }
  return true;
}

interfaces::TlsRecv WalterTlsSocket::receive(std::uint8_t* out, std::size_t max, std::size_t* got,
                                             std::uint64_t deadline_ms) {
  if (got != nullptr) *got = 0;
  if (!open_ || out == nullptr || got == nullptr || max == 0) return interfaces::TlsRecv::kError;
  const int id = rotation_.currentId();
  const std::size_t want = max > kSocketReceiveMax ? kSocketReceiveMax : max;
  for (;;) {
    if (!before(deadline_ms)) return interfaces::TlsRecv::kTimeout;
    // Ask. The ring flag is only a cue. "Nothing buffered" comes back in one of two ways:
    // `+SQNSRECV: 1,0`, or - what the GM02SP on the first board answered every time (23 October
    // 2026, the library's own trace) - `+CME ERROR: 4`, *operation not supported*, to an
    // `AT+SQNSRECV` sent before the server's first byte had arrived. Read as an error, it made this
    // loop hang up 10 ms after every request, before any server could answer. So that one code is
    // "not yet", like n = 0; any other error is an error, and a closed socket with nothing left to
    // read is the end.
    rotation_.clearRing();
    WalterModemRsp rsp = {};
    if (!WalterModem::socketReceive(id, out, want, &rsp)) {
      if (rotation_.closedSeen()) return interfaces::TlsRecv::kClosed;
      const bool not_yet = rsp.type == WALTER_MODEM_RSP_DATA_TYPE_CME_ERROR &&
                           rsp.data.cmeError == WALTER_MODEM_CME_OPERATION_NOT_SUPPORTED;
      if (!not_yet) return interfaces::TlsRecv::kError;
    } else if (rsp.type == WALTER_MODEM_RSP_DATA_TYPE_SOCKET &&
               rsp.data.socketResponse.bytesReceived > 0) {
      std::size_t n = rsp.data.socketResponse.bytesReceived;
      if (n > want) n = want;  // the library copies what the modem said; never more than asked
      *got = n;
      received_ = true;
      return interfaces::TlsRecv::kData;
    }
    // Nothing buffered. The other end closed, and everything it sent has been read: the end.
    if (rotation_.closedSeen()) return interfaces::TlsRecv::kClosed;
    // Wait for a ring or the poll interval, whichever comes first, inside the deadline.
    const std::uint64_t now = clock_.nowMs();
    std::uint64_t until = now + kReceivePollMs;
    if (until > deadline_ms) until = deadline_ms;
    while (clock_.nowMs() < until && !rotation_.ringSeen() && !rotation_.closedSeen()) {
      clock_.delayMs(5);
    }
  }
}

void WalterTlsSocket::close() {
  if (!open_) return;
  open_ = false;
  // AT+SQNSH. A socket the other end already closed answers ERROR, which is fine: it is closed.
  WalterModem::socketClose(rotation_.currentId());
  // What this id owes is settled after the close command, not before it: the server's `+SQNSH`
  // may well have been dispatched while that command was in flight, and then nothing is owed.
  //
  // And it is owed only when the other end answered. A response arrives under `Connection:
  // close` and the server's FIN follows the last byte of it, so a connection that gave us bytes
  // has a close on its way; a handshake with no request behind it (core/update/tls_check.h) is
  // closed by this end alone and has none, so owing one for it would be a debt nobody can pay.
  // A debt that is never paid now costs one wait and one write-off and no more
  // (core/net/socket_rotation.h, *A debt lives for one connection*), but not making it in the
  // first place is still better than bounding it.
  if (received_) {
    rotation_.end();
  } else {
    rotation_.cancel();
  }
}

// ------------------------------------------------------------------------------------- the store

bool WalterTlsStore::writeRoot(std::uint8_t slot, const char* pem, std::uint64_t deadline_ms) {
  if (pem == nullptr || pem[0] == 0 || !uplink_.started()) return false;
  if (clock_.nowMs() >= deadline_ms) return false;
  // AT+SQNSNVW="certificate",<slot>,<size> and the PEM as the data (tlsWriteCredential).
  return WalterModem::tlsWriteCredential(false, slot, pem);
}

bool WalterTlsStore::configureProfile(std::uint8_t profile, std::uint8_t slot, std::uint64_t deadline_ms) {
  if (!uplink_.started()) return false;
  if (clock_.nowMs() >= deadline_ms) return false;
  // AT+SQNSPCFG=<profile>,<TLS 1.2>,"",<validate the chain AND the host name>,<slot>,,,"","",0,1,0
  // (WalterModem.cpp, tlsConfigProfile). The library's default for the validation argument is
  // "validate nothing"; it is never left to the default here, and this is the only call.
  return WalterModem::tlsConfigProfile(profile, WALTER_MODEM_TLS_VALIDATION_URL_AND_CA,
                                       WALTER_MODEM_TLS_VERSION_12, slot);
}

}  // namespace modem
