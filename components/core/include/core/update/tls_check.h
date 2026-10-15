// The self-update trial's third condition over the real uplink (task p3-02, filling the seam
// task p2-04 left in uplink_check.h): reach the configured server's TLS port and complete a
// handshake with the chain verified against the server's root and the name checked - the
// strongest statement available while nothing talks to MeshMaster yet (spec/SYSTEM.md §8). An
// empty host is "not configured" and the condition does not apply, exactly as UnconfiguredUplink
// answered; task p3-03's settings give the unit a host.
#pragma once

#include <cstdint>

#include "core/net/trust.h"
#include "core/update/uplink_check.h"
#include "interfaces/clock.h"
#include "interfaces/tls_socket.h"

namespace core {
namespace update {

// How long the handshake may take: a connect and a TLS handshake over LTE-M, seconds each. TO BE
// SET ON THE BENCH, with the trial window (self_update.h).
constexpr std::uint32_t kHandshakeDeadlineMs = 45000;

class TlsHandshakeCheck final : public UplinkCheck {
 public:
  // `trust` may be nullptr in a test; in the firmware it is the loader, so that the first trial
  // of a unit's life does not fail for want of a root in the modem.
  TlsHandshakeCheck(interfaces::TlsSocket& socket, net::TrustLoader* trust, interfaces::Clock& clock,
                    const char* host = kSelfUpdateServerHost,
                    std::uint16_t port = kSelfUpdateServerPort)
      : socket_(socket), trust_(trust), clock_(clock), host_(host != nullptr ? host : ""), port_(port) {}

  UplinkVerdict check() override;
  const char* serverHost() const override { return host_; }
  std::uint16_t serverPort() const override { return port_; }
  // `server.url` at the console (task p3-03): the host the next trial reaches for.
  void setServer(const char* host, std::uint16_t port) override {
    host_ = host != nullptr ? host : "";
    port_ = port;
  }

  // The last attempt's outcome, for a console line.
  interfaces::TlsOpen lastOpen() const { return last_; }

 private:
  interfaces::TlsSocket& socket_;
  net::TrustLoader* trust_;
  interfaces::Clock& clock_;
  const char* host_;
  std::uint16_t port_;
  interfaces::TlsOpen last_ = interfaces::TlsOpen::kNoUplink;
};

}  // namespace update
}  // namespace core
