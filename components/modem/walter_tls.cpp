#include "modem/walter_tls.h"

#include "WalterModem.h"

#include "core/net/trust.h"

namespace modem {

namespace {

// What the library's event task tells us about socket 1: a ring (bytes arrived) and a close.
// Written by that task, read by the main task; each a flag, nothing to tear.
volatile bool g_ring = false;
volatile bool g_closed = false;

void socketEvent(WMSocketEventType event, const WMSocketEventData* data, void* args) {
  (void)args;
  if (data == nullptr || data->conn_id != kSocketId) return;
  if (event == WALTER_MODEM_SOCKET_EVENT_RING) g_ring = true;
  if (event == WALTER_MODEM_SOCKET_EVENT_DISCONNECTED) g_closed = true;
}

}  // namespace

// ------------------------------------------------------------------------------------ the socket

interfaces::TlsOpen WalterTlsSocket::open(const char* host, std::uint16_t port, std::uint8_t root,
                                          std::uint64_t deadline_ms) {
  if (open_) return interfaces::TlsOpen::kBusy;
  if (host == nullptr || host[0] == 0) return interfaces::TlsOpen::kConnectFailed;
  if (uplink_.state() != interfaces::UplinkState::kUp) return interfaces::TlsOpen::kNoUplink;
  if (root >= core::net::tlsRootCount()) return interfaces::TlsOpen::kNoSuchRoot;
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  if (!handler_set_) {
    WalterModem::setSocketEventHandler(socketEvent, nullptr);
    handler_set_ = true;
  }
  g_ring = false;
  g_closed = false;

  // The socket's parameters, every time: a previous life of the modem may have left others.
  if (!WalterModem::socketConfig(kSocketId, 1, kSocketPacketBytes, kSocketExchangeTimeoutS,
                                 kSocketConnectTimeoutS, kSocketSendDelayMs)) {
    return interfaces::TlsOpen::kConnectFailed;
  }
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  // Rings that say how much arrived, bytes as bytes, no listening, bytes sent as bytes.
  if (!WalterModem::socketConfigExtended(kSocketId, WALTER_MODEM_SOCKET_RING_MODE_DATA_AMOUNT,
                                         WALTER_MODEM_SOCKET_RECV_MODE_TEXT, 0,
                                         WALTER_MODEM_SOCKET_LISTEN_MODE_DISABLED,
                                         WALTER_MODEM_SOCKET_SEND_MODE_TEXT)) {
    return interfaces::TlsOpen::kConnectFailed;
  }
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  // TLS on, with the profile that validates the chain against this root (core/net/trust.h:
  // tlsProfileOf). The profile itself was configured by WalterTlsStore::configureProfile.
  if (!WalterModem::socketConfigSecure(kSocketId, true, core::net::tlsProfileOf(root))) {
    return interfaces::TlsOpen::kConnectFailed;
  }
  if (!before(deadline_ms)) return interfaces::TlsOpen::kTimeout;
  // The dial: DNS, TCP and the TLS handshake, in command mode. One ERROR for all three failures
  // (walter_tls.h), hence kDialRefused.
  if (!WalterModem::socketDial(kSocketId, WALTER_MODEM_SOCKET_PROTO_TCP, port, host)) {
    return interfaces::TlsOpen::kDialRefused;
  }
  open_ = true;
  return interfaces::TlsOpen::kOpen;
}

bool WalterTlsSocket::send(const std::uint8_t* data, std::size_t size, std::uint64_t deadline_ms) {
  if (!open_ || data == nullptr) return false;
  while (size > 0) {
    if (!before(deadline_ms) || g_closed) return false;
    const std::size_t n = size > kSocketReceiveMax ? kSocketReceiveMax : size;
    // The library takes a non-const pointer and does not write through it (it hands the bytes to
    // uart_write_bytes after the modem's prompt).
    if (!WalterModem::socketSend(kSocketId, const_cast<std::uint8_t*>(data), static_cast<std::uint16_t>(n),
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
  const std::size_t want = max > kSocketReceiveMax ? kSocketReceiveMax : max;
  for (;;) {
    if (!before(deadline_ms)) return interfaces::TlsRecv::kTimeout;
    // Ask. `+SQNSRECV: 1,<n>` with n = 0 is "nothing buffered"; the ring flag is only a cue.
    g_ring = false;
    WalterModemRsp rsp = {};
    if (!WalterModem::socketReceive(kSocketId, out, want, &rsp)) {
      return g_closed ? interfaces::TlsRecv::kClosed : interfaces::TlsRecv::kError;
    }
    if (rsp.type == WALTER_MODEM_RSP_DATA_TYPE_SOCKET && rsp.data.socketResponse.bytesReceived > 0) {
      std::size_t n = rsp.data.socketResponse.bytesReceived;
      if (n > want) n = want;  // the library copies what the modem said; never more than asked
      *got = n;
      return interfaces::TlsRecv::kData;
    }
    // Nothing buffered. The other end closed, and everything it sent has been read: the end.
    if (g_closed) return interfaces::TlsRecv::kClosed;
    // Wait for a ring or the poll interval, whichever comes first, inside the deadline.
    const std::uint64_t now = clock_.nowMs();
    std::uint64_t until = now + kReceivePollMs;
    if (until > deadline_ms) until = deadline_ms;
    while (clock_.nowMs() < until && !g_ring && !g_closed) clock_.delayMs(5);
  }
}

void WalterTlsSocket::close() {
  if (!open_) return;
  open_ = false;
  // AT+SQNSH. A socket the other end already closed answers ERROR, which is fine: it is closed.
  WalterModem::socketClose(kSocketId);
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
