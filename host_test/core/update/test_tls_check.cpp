// The self-update trial's third condition over the real uplink (task p3-02, core/update/
// tls_check.h): a handshake with the configured server's TLS port, its chain held to the server's
// root. No host is "not configured" and the condition does not apply; a host whose chain ends
// elsewhere, or that cannot be reached, is "not reached"; the roots are loaded first.
#include <doctest.h>

#include "core/net/trust.h"
#include "core/update/tls_check.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_retained.h"
#include "sim/fake_tls_socket.h"
#include "sim/scripted_server.h"

namespace upd = core::update;

TEST_CASE("no host: not configured; a reachable host with the right root: reached; the wrong root or no host: not reached") {
  fakes::FakeClock clock;
  sim::ScriptedServer server;
  server.hosts.push_back(sim::ScriptedHost{"meshmaster.dk", core::net::kRootIsrgX1});
  server.hosts.push_back(sim::ScriptedHost{"wrong-root.example", core::net::kRootUserTrustEcc});
  sim::FakeTlsSocket socket(server, clock);
  fakes::FakeRetained retained;
  sim::FakeTlsStore store;
  core::net::TrustLoader trust(store, retained, clock);
  trust.begin();

  upd::TlsHandshakeCheck none(socket, &trust, clock, "", 443);
  CHECK(none.check() == upd::UplinkVerdict::kNotConfigured);
  CHECK(socket.opens == 0);
  CHECK(store.writes == 0);  // nothing configured: the roots were not even written

  upd::TlsHandshakeCheck reached(socket, &trust, clock, "meshmaster.dk", 443);
  CHECK(reached.check() == upd::UplinkVerdict::kReached);
  CHECK(reached.lastOpen() == interfaces::TlsOpen::kOpen);
  CHECK(socket.last_root == core::net::kRootIsrgX1);
  CHECK(store.writes == 2);  // the roots went in first
  CHECK_FALSE(socket.isOpen());  // and the connection was closed again

  upd::TlsHandshakeCheck wrong(socket, &trust, clock, "wrong-root.example", 443);
  CHECK(wrong.check() == upd::UplinkVerdict::kFailed);
  CHECK(wrong.lastOpen() == interfaces::TlsOpen::kTlsFailed);

  upd::TlsHandshakeCheck nobody(socket, &trust, clock, "nobody.example", 443);
  CHECK(nobody.check() == upd::UplinkVerdict::kFailed);
  CHECK(nobody.lastOpen() == interfaces::TlsOpen::kConnectFailed);

  socket.no_uplink = true;
  CHECK(reached.check() == upd::UplinkVerdict::kFailed);
  CHECK(reached.lastOpen() == interfaces::TlsOpen::kNoUplink);
  CHECK(std::string(reached.serverHost()) == "meshmaster.dk");
  CHECK(reached.serverPort() == 443);

  // The condition is a **handshake and nothing more**, so there is no request to carry
  // `X-Agent-Version` and task p10-05 left it alone: the server saw no request of any of the
  // five checks above, and the one that reached the server opened and closed a connection.
  CHECK(server.requests.empty());
  CHECK(socket.opens > 0);
}

TEST_CASE("a modem that will not take the roots is a failed check, not a confirmed trial") {
  fakes::FakeClock clock;
  sim::ScriptedServer server;
  server.hosts.push_back(sim::ScriptedHost{"meshmaster.dk", core::net::kRootIsrgX1});
  sim::FakeTlsSocket socket(server, clock);
  fakes::FakeRetained retained;
  sim::FakeTlsStore store;
  store.refuse_writes = true;
  core::net::TrustLoader trust(store, retained, clock);
  trust.begin();
  upd::TlsHandshakeCheck check(socket, &trust, clock, "meshmaster.dk", 443);
  CHECK(check.check() == upd::UplinkVerdict::kFailed);
  CHECK(socket.opens == 0);
}
