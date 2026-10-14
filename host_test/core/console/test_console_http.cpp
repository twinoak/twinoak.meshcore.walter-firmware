// The console's three HTTPS lines (task p3-02; docs/console.md): `http get <url>`, `fetch
// <base-url> [candidate|scanner]` and `trust tls` - what each says on the rig the other console
// tests use, with the scripted server behind the firmware's own HTTP; what a build without a
// client, without a store, or with the uplink down says instead; and the parser's refusals.
#include <doctest.h>

#include <memory>
#include <string>

#include "core/board/radio_pins.h"
#include "core/console/console.h"
#include "core/console/parser.h"
#include "core/health/health.h"
#include "core/image/store_bench.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/net/trust.h"
#include "core/update/fetch.h"
#include "core/uplink/policy.h"
#include "fakes/capture.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "core/net/net_bench.h"
#include "sim/fake_modem.h"
#include "sim/scripted_radio.h"

using bench::Candidate;
using bench::has;
using core::console::Console;
using core::console::parseLine;
using core::console::Verb;

namespace {

struct HttpConsole {
  bench::StoreBench store;  // the store over a simulated flash, the test key's trust table, one verdict
  fakes::FakeClock& clock = store.clock;
  sim::FakeHttp http{clock};
  core::net::Client client{http, clock};
  core::net::Download download{client, clock};
  fakes::CaptureOut out;
  core::update::Fetch fetch{client, download, store.store, store.verdict, clock, &out};
  sim::FakeTlsStore tls_store;
  fakes::FakeRetained retained;
  core::net::TrustLoader trust{tls_store, retained, clock};

  fakes::FakePins pins{clock};
  fakes::FakeRestart restart;
  core::board::RadioPins radio{pins, clock, retained};
  core::health::Health health{retained, clock};
  sim::ScriptedRadio rak{clock};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{radio, rak};
  core::link::client::Client link{port, clock, store.wall, slots};
  sim::FakeModem modem{clock};
  core::uplink::Settings settings{};
  core::uplink::UplinkPolicy uplink{modem, clock, retained, settings};
  std::unique_ptr<Console> console;

  HttpConsole() {
    pins.record_log = false;
    health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
    clock.advance(core::board::kKillTiming.settle_ms);
    radio.safe();
    uplink.begin();
    trust.begin();
    full();
  }

  // The whole thing: client, fetch and trust loader, and the uplink to be held to.
  void full() {
    console.reset(new Console(out, radio, health, clock, restart, link, slots, &store.store, nullptr,
                              nullptr, nullptr, nullptr, nullptr, &uplink, &client, &fetch, &trust));
  }
  // No client at all (main handed over nothing).
  void withoutClient() {
    console.reset(new Console(out, radio, health, clock, restart, link, slots, &store.store, nullptr,
                              nullptr, nullptr, nullptr, nullptr, &uplink));
  }
  // A client but no store: no fetch.
  void withoutStore() {
    console.reset(new Console(out, radio, health, clock, restart, link, slots, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, &uplink, &client, nullptr, &trust));
  }

  std::string run(const char* line) {
    out.clear();
    console->runLine(line);
    return out.text;
  }
};

}  // namespace

TEST_CASE("the parser takes the three lines and refuses the rest") {
  CHECK(parseLine("http get https://meshmaster.dk/").command.verb == Verb::kHttpGet);
  CHECK(std::string(parseLine("http get https://meshmaster.dk/a?b=1").command.url) == "https://meshmaster.dk/a?b=1");
  CHECK(parseLine("  http   get  https://x.example/ \r\n").command.verb == Verb::kHttpGet);
  CHECK_FALSE(parseLine("http").ok);
  CHECK(has(parseLine("http").error, "http needs a subcommand"));
  CHECK_FALSE(parseLine("http post https://x.example/").ok);
  CHECK(has(parseLine("http post https://x.example/").error, "unknown http subcommand (get): post"));
  CHECK_FALSE(parseLine("http get").ok);
  CHECK(has(parseLine("http get").error, "needs a URL"));
  CHECK_FALSE(parseLine("http get https://x.example/ now").ok);
  // The scheme is the client's to refuse, so that the refusal is tested where it lives.
  CHECK(parseLine("http get ftp://x.example/").command.verb == Verb::kHttpGet);

  CHECK(parseLine("fetch https://r.example/v1").command.verb == Verb::kFetch);
  CHECK(parseLine("fetch https://r.example/v1").command.which == static_cast<std::uint8_t>(core::image::Which::kCandidate));
  CHECK(parseLine("fetch https://r.example/v1 candidate").command.which == static_cast<std::uint8_t>(core::image::Which::kCandidate));
  CHECK(parseLine("fetch https://r.example/v1 scanner").command.which == static_cast<std::uint8_t>(core::image::Which::kScanner));
  CHECK(std::string(parseLine("fetch https://r.example/v1 scanner").command.url) == "https://r.example/v1");
  CHECK_FALSE(parseLine("fetch").ok);
  CHECK(has(parseLine("fetch").error, "needs a base URL"));
  CHECK_FALSE(parseLine("fetch https://r.example/v1 golden").ok);
  CHECK(has(parseLine("fetch https://r.example/v1 golden").error, "promotion"));
  CHECK_FALSE(parseLine("fetch https://r.example/v1 bridge").ok);
  CHECK_FALSE(parseLine("fetch https://r.example/v1 scanner now").ok);

  CHECK(parseLine("trust tls").command.verb == Verb::kTrustTls);
  CHECK(parseLine("trust").command.verb == Verb::kTrust);
  CHECK_FALSE(parseLine("trust ssl").ok);
  CHECK(has(parseLine("trust ssl").error, "or 'tls'"));
}

TEST_CASE("a build with no client says so in one line; the uplink down is refused with 'lte up' first") {
  HttpConsole rig;
  rig.withoutClient();
  CHECK(has(rig.run("http get https://server.example/x"), "error: this build has no HTTPS client"));
  CHECK(has(rig.run("fetch https://server.example/r"), "error: this build has no HTTPS client"));
  CHECK(rig.http.socket.opens == 0);
  rig.full();
  // The uplink is down: nothing is sent.
  const std::string down = rig.run("http get https://server.example/x");
  CHECK(has(down, "error: the uplink is down: 'lte up' first"));
  CHECK(rig.http.socket.opens == 0);
  CHECK(has(rig.run("fetch https://server.example/r"), "'lte up' first"));
}

TEST_CASE("http get: the status, the named headers, the first bytes as hex, the size, the time; a cut at 1 KB; a refused scheme; a TLS failure") {
  HttpConsole rig;
  rig.run("lte up");
  REQUIRE(rig.uplink.state() == interfaces::UplinkState::kUp);
  rig.http.server.serve("/hello", {'h', 'i', '!', 0x00, 0xFF}, "\"v7\"", "Wed, 30 Sep 2026 02:39:56 GMT");
  std::string text = rig.run("http get https://server.example/hello");
  INFO(text);
  CHECK(has(text, "http get: https://server.example/hello (the chain is verified"));
  CHECK(has(text, "GET server.example/hello -> 200 ok, 5 bytes"));
  CHECK(has(text, "http get: 200 ok"));
  CHECK(has(text, "Content-Type: application/octet-stream"));
  CHECK(has(text, "ETag: \"v7\""));
  CHECK(has(text, "Last-Modified: Wed, 30 Sep 2026 02:39:56 GMT"));
  CHECK(has(text, "Accept-Ranges: bytes"));
  CHECK(has(text, "Content-Length: 5"));
  CHECK(has(text, "first 5 bytes: 68692100ff"));
  CHECK(has(text, "5 bytes in "));

  rig.http.server.serve("/big", bench::patternBytes(5000));
  text = rig.run("http get https://server.example/big");
  CHECK(has(text, "http get: 200 the response is larger than the bound: cut off there"));
  CHECK(has(text, "first 32 bytes: "));
  CHECK(has(text, "1024 bytes (cut off: a console line reads 1 KB at most)"));

  text = rig.run("http get http://server.example/hello");
  CHECK(has(text, "error: http get: only https URLs are accepted"));

  rig.http.server.statuses["/denied"] = sim::StatusRule{403, "", "no"};
  text = rig.run("http get https://server.example/denied");
  CHECK(has(text, "http get: 403 denied (401 or 403)"));

  rig.http.server.hosts.push_back(sim::ScriptedHost{"wrong.example", core::net::kRootUserTrustEcc});
  text = rig.run("http get https://wrong.example/hello");
  CHECK(has(text, "error: http get: no response: the TLS handshake failed"));

  rig.http.server.hosts.push_back(sim::ScriptedHost{"other.example", core::net::kRootIsrgX1});
  rig.http.server.redirects["/go"] = sim::Redirect{302, "https://other.example/hello"};
  rig.http.server.serve("/hello", {'o', 'k'});
  text = rig.run("http get https://server.example/go");
  CHECK(has(text, "-> 302"));
  CHECK(has(text, "http get: 200 ok after a redirect to other.example"));
}

TEST_CASE("fetch: the three steps and the six checks on the console, stored into the candidate slot; a refusal; no store") {
  HttpConsole rig;
  rig.run("lte up");
  const Candidate image("store-a");
  rig.http.server.serve("/r/manifest.json", image.manifest);
  rig.http.server.serve("/r/manifest.json.sig", std::vector<std::uint8_t>(image.signature.begin(), image.signature.end()));
  const std::string text0(image.manifest.begin(), image.manifest.end());
  const std::size_t use = text0.find("\"use\": \"swd\"");
  const std::size_t name_at = text0.find("\"name\": \"", use) + 9;
  const std::string name = text0.substr(name_at, text0.find('"', name_at) - name_at);
  rig.http.server.serve("/r/" + name, image.file);

  std::string text = rig.run("fetch https://server.example/r");
  INFO(text);
  CHECK(has(text, "fetch: https://server.example/r into the candidate image of slot 0"));
  CHECK(has(text, "GET server.example/r/manifest.json -> 200"));
  CHECK(has(text, "fetch: check 1, the signature: passed"));
  CHECK(has(text, ("fetch: check 3, the image for this slot: passed - " + name).c_str()));
  CHECK(has(text, "range 0-20479: ok, 206 (attempt 1)"));
  CHECK(has(text, "fetch: check 4, the file's size and hash: passed"));
  CHECK(has(text, "fetch: check 6, the region: passed"));
  CHECK(has(text, ("fetch: stored: " + std::string(image.version)).c_str()));
  CHECK(has(text, "is the candidate image; 'image list' shows it"));
  CHECK(rig.store.read(core::image::Which::kCandidate).present);

  rig.http.server.serve("/r/" + name, Candidate("store-b").file);
  rig.store.store.erase(core::image::Which::kCandidate, core::board::Slot{0});
  text = rig.run("fetch https://server.example/r");
  INFO(text);
  CHECK(has(text, "fetch: refused at check 4, the file's size and hash: the file's SHA-256 is not the manifest's; nothing stored, the slot is empty"));

  text = rig.run("fetch https://server.example/nothing");
  CHECK(has(text, "error: fetch: a request failed"));

  rig.withoutStore();
  CHECK(has(rig.run("fetch https://server.example/r"), "error: fetch: this build has no image store"));
}

TEST_CASE("trust tls: the roots, their fingerprints and expiry, the slots' state before and after the first request") {
  HttpConsole rig;
  std::string text = rig.run("trust tls");
  INFO(text);
  CHECK(has(text, "trust tls: 2 roots; the modem's certificate slots have no record (a first boot or a power cut)"));
  CHECK(has(text, "0. ISRG Root X1"));
  CHECK(has(text, "subject C=US, O=Internet Security Research Group, CN=ISRG Root X1"));
  CHECK(has(text, "sha256 96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6"));
  CHECK(has(text, "expires 2035-06-04; checked 2026-10-14 from https://letsencrypt.org/certs/isrgrootx1.pem"));
  CHECK(has(text, "certificate slot 12, TLS profile 2: not written yet: written at the next request"));
  CHECK(has(text, "1. USERTrust ECC Certification Authority"));
  CHECK(has(text, "sha256 4ff460d54b9c86dabfbcfc5712e0400d2bed3fbc4d4fbdaa86e06adcd2a9ad7a"));
  CHECK(has(text, "certificate slot 13, TLS profile 3: not written yet"));
  CHECK(has(text, "hosts: github.com -> root 1; every other host -> root 0"));
  CHECK(has(text, "0 root(s) written in this boot"));

  // The first request writes them; `trust tls` then says so, and the loader wrote each once.
  rig.run("lte up");
  rig.http.server.serve("/x", {'x'});
  REQUIRE(rig.trust.ensure(rig.clock.nowMs() + 60000) == core::net::TrustOutcome::kWritten);
  text = rig.run("trust tls");
  CHECK(has(text, "are tracked in retained storage"));
  CHECK(has(text, "certificate slot 12, TLS profile 2: written, matches this build's root"));
  CHECK(has(text, "certificate slot 13, TLS profile 3: written, matches this build's root"));
  CHECK(has(text, "2 root(s) written in this boot"));
  CHECK(rig.tls_store.writes == 2);
}

TEST_CASE("help lists the three") {
  HttpConsole rig;
  const std::string text = rig.run("help");
  CHECK(has(text, "http get <url>"));
  CHECK(has(text, "fetch <base-url> [candidate|scanner]"));
  CHECK(has(text, "trust tls"));
}
