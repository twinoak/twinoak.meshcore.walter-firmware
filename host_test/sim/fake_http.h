// interfaces::Http on the host (task p3-02): **the firmware's own HTTP/1.1 over the fake TLS
// socket to the scripted server** - not a stub of HTTP. core/net/http_over_tls.h is what the
// board runs and what every test of the client, the download and the fetch runs through here,
// so a response the scripted server builds is parsed by the same code that parses the modem's
// bytes, in the same pieces.
//
// The script is the server's (sim/scripted_server.h: files by path, ranges on or off, redirects,
// statuses, a body one byte too long, a wrong Content-Range, chunked, slowness) and the
// socket's (sim/fake_tls_socket.h: a drop after N bytes, a stall, a TLS refusal, the root the
// chain ends at). The trust loader is optional: a test that wants to see the roots written gives
// one over a FakeTlsStore; the rest leave it out.
#pragma once

#include "core/net/http_over_tls.h"
#include "fakes/fake_clock.h"
#include "interfaces/http.h"
#include "sim/fake_tls_socket.h"
#include "sim/scripted_server.h"

namespace sim {

class FakeHttp final : public interfaces::Http {
 public:
  explicit FakeHttp(fakes::FakeClock& clock, core::net::TrustLoader* trust = nullptr)
      : socket(server, clock), http(socket, trust, clock) {}

  ScriptedServer server;
  FakeTlsSocket socket;
  core::net::HttpOverTls http;

  void request(const interfaces::HttpRequest& request, interfaces::HttpWantedHeader* wanted,
               std::size_t wanted_count, interfaces::HttpResponse* response,
               interfaces::HttpBodySink& sink) override {
    http.request(request, wanted, wanted_count, response, sink);
  }
};

}  // namespace sim
