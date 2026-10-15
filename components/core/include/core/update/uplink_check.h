// The third condition of the Walter's own trial (task p2-04; spec/SYSTEM.md S-6: "a new image is
// confirmed only after it has booted, spoken to the RAK and reached the server"): one exchange
// with the backend over the uplink. Phase 3 brings the modem and the server; until then this is
// an interface with two implementations - the fake in the tests, and in the firmware the one
// below, which says that no server is configured.
//
// What the condition is, when there is a server: kUplinkCondition names it. The strongest
// statement available without an agent (spec/SYSTEM.md §8: nothing talks to MeshMaster yet) is
// to reach the configured server's TLS port and complete a handshake with its chain verified -
// which proves the modem attaches, the network routes, and the unit can still talk to the one
// host it exists to talk to. Phase 3's HTTPS client (tasks/phase-3/02) implements it over the
// modem and replaces UnconfiguredUplink in main/.
//
// The rule that keeps a factory image from rejecting itself: an uplink that answers
// kNotConfigured makes the third condition *not applicable*, and the trial confirms on the first
// two. A unit that has never been given a server - every unit out of the box, and every unit
// until phase 3 - has nothing to reach, and failing it for that would brick the first update of
// every unit. Tested in host_test/core/update/test_self_update.cpp.
#pragma once

#include <cstdint>

namespace core {
namespace update {

enum class UplinkCondition : std::uint8_t {
  kTlsHandshake = 1,  // reach the configured server's TLS port and complete a verified handshake
};

// The condition the firmware applies when a server is configured. TO BE SET ON THE BENCH, with
// the window (self_update.h): the first unit that attaches says how long a handshake takes.
constexpr UplinkCondition kUplinkCondition = UplinkCondition::kTlsHandshake;

// The server the trial must reach, before anybody has provisioned the unit: empty, which means
// "not configured", so the condition does not apply. **It is the `server.url` setting**
// (core/settings/keys.h, task p3-03): the console takes the URL apart and hands the host and the
// port to setServer() at boot and at every change. Every unit out of the box is on this
// constant, and the rule below is what keeps it from rejecting its own first update.
constexpr const char* kSelfUpdateServerHost = "";
constexpr std::uint16_t kSelfUpdateServerPort = 443;

enum class UplinkVerdict : std::uint8_t {
  kReached = 0,        // the exchange completed: the third condition is met
  kFailed,             // the attempt failed (no attach, no route, no handshake); try again later
  kNotConfigured,      // no server is configured: the condition does not apply
};

const char* describe(UplinkVerdict verdict);

class UplinkCheck {
 public:
  virtual ~UplinkCheck() = default;
  // One attempt. May take as long as an LTE attach; the trial window allows for it.
  virtual UplinkVerdict check() = 0;
  // What it would try, for a console line: "a TLS handshake with <host>:<port>".
  virtual const char* serverHost() const = 0;
  virtual std::uint16_t serverPort() const = 0;

  // A person has set `server.url` at the console (task p3-03). The host must stay alive as long
  // as this object does, which the settings store's own buffer does
  // (core/settings/settings.h); an empty host is "not configured" again, and the condition stops
  // applying. Takes effect at the next trial: a trial already running was begun against the
  // server the unit had then.
  virtual void setServer(const char* host, std::uint16_t port) = 0;
};

// The firmware's implementation until phase 3: kSelfUpdateServerHost is empty, so there is
// nothing to reach and the condition does not apply. A non-empty host with no modem driver in
// the build is a mistake this class refuses to paper over: it answers kFailed then, so that a
// build which names a server it cannot reach rolls its update back rather than confirming it
// blind. Tested on the host.
class UnconfiguredUplink final : public UplinkCheck {
 public:
  explicit UnconfiguredUplink(const char* host = kSelfUpdateServerHost,
                              std::uint16_t port = kSelfUpdateServerPort)
      : host_(host != nullptr ? host : ""), port_(port) {}
  UplinkVerdict check() override { return host_[0] == 0 ? UplinkVerdict::kNotConfigured : UplinkVerdict::kFailed; }
  const char* serverHost() const override { return host_; }
  std::uint16_t serverPort() const override { return port_; }
  void setServer(const char* host, std::uint16_t port) override {
    host_ = host != nullptr ? host : "";
    port_ = port;
  }

 private:
  const char* host_;
  std::uint16_t port_;
};

}  // namespace update
}  // namespace core
