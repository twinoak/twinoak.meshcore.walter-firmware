// What a trace line may not carry out (task p10-06, core/trace/redact.h).
//
// The two cases that matter are the first two: **a real request head**, written by the firmware's
// own request writer (`core/net/http1.cpp`) with a known token in it, escaped the way the modem
// library escapes a payload before it logs it - through the redactor, and then through the whole
// of the hook (`formatAndRedact`, which is every decision `components/modem/modem_trace.cpp`
// makes) with the arguments ESP-IDF hands it. That is the line the hook sees on a board - TLS is
// inside the modem, so the UART carries the HTTP head in plaintext - and the token is searched for
// in the whole buffer afterwards, not only in the part that was written.
#include <doctest.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/net/client.h"
#include "core/net/http1.h"
#include "core/trace/redact.h"
#include "core/version.h"
#include "interfaces/http.h"

using core::trace::Redactor;

namespace {

// The two secrets, the pair every other test of this firmware uses for them
// (host_test/rails/test_rail_secrets.cpp).
constexpr const char* kToken = "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV";
constexpr const char* kPin = "86421357";

// A line through a redactor, as the hook does it: in place, length unchanged.
struct Line {
  char text[4096] = {};
  std::size_t length = 0;
  std::size_t taken = 0;

  Line(const Redactor& redactor, const std::string& from) {
    REQUIRE(from.size() + 1 <= sizeof text);
    std::memcpy(text, from.data(), from.size());
    length = from.size();
    taken = redactor.apply(text, length);
  }
  std::string str() const { return std::string(text, length); }
  bool has(const char* what) const { return str().find(what) != std::string::npos; }
};

// What the library's `escapeBuffer` does to the bytes it is about to log (WalterModem.cpp): CR
// and LF become a backslash and a letter, and every other printable byte is itself. Written here
// rather than linked, because the library cannot be compiled on the host - and it is two lines.
std::string escaped(const std::string& bytes) {
  std::string out;
  for (char c : bytes) {
    if (c == '\r') {
      out += "\\r";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// The head of a POST exactly as this firmware sends it: the four headers of
// core/net/client.h's own request, the token among them.
std::string realRequestHead(const char* token) {
  interfaces::HttpRequest request;
  request.method = interfaces::HttpMethod::kPost;
  request.host = "meshmaster.dk";
  request.path = "/agent/v1/observations";
  request.headers[0] = {core::net::kVersionHeader, core::version()};
  request.headers[1] = {"Accept", "application/json"};
  char bearer[256] = {};
  std::snprintf(bearer, sizeof bearer, "Bearer %s", token);
  request.headers[2] = {"Authorization", bearer};
  request.headers[3] = {"Content-Type", "application/json"};
  request.header_count = 4;
  const char* body = "{\"observations\":[]}";
  request.body = reinterpret_cast<const std::uint8_t*>(body);
  request.body_size = std::strlen(body);
  char head[core::net::kRequestHeadBytes] = {};
  const std::size_t n =
      core::net::writeRequestHead(request, "twinoak-walter/0.1.3", head, sizeof head);
  REQUIRE(n > 0);
  return std::string(head, n) + body;
}

}  // namespace

TEST_CASE("a request this firmware really writes, escaped as the library escapes it, carries no "
          "token out of the redactor") {
  Redactor redactor;
  CHECK(redactor.keep(0, kToken));
  CHECK(redactor.keep(1, kPin));
  CHECK(redactor.holding() == 2);

  const std::string head = realRequestHead(kToken);
  // The control: the token really is in the line before the redactor sees it. A test that
  // searched a line the token was never in would pass by doing nothing.
  CHECK(head.find(kToken) != std::string::npos);
  CHECK(head.find("Authorization: Bearer") != std::string::npos);

  const Line line(redactor, "D (54321) WalterModem: TX: " + escaped(head));
  const std::string out = line.str();
  CHECK(out.size() == line.length);
  CHECK_FALSE(line.has(kToken));
  // Nor a piece of it long enough to be worth anything.
  CHECK_FALSE(line.has("zQ7x4LpR"));
  CHECK(line.taken >= 1);
  // Everything else of the request is still there: a trace that redacted the request away would
  // be no use at all.
  CHECK(line.has("POST /agent/v1/observations HTTP/1.1"));
  CHECK(line.has("Host: meshmaster.dk"));
  CHECK(line.has("X-Agent-Version:"));
  CHECK(line.has("Content-Type: application/json"));
  // The library's escaping leaves a quote alone, so the body reads as itself.
  CHECK(line.has("{\"observations\":[]}"));
  // The header is still visible as a header, with asterisks where its value was.
  CHECK(line.has("Authorization: *"));
}

// The hook itself, with the arguments ESP-IDF hands it: `esp_log_write` composes one format for
// the whole line - the level letter, the timestamp, the tag, then the caller's own format - and
// calls the vprintf-like function once with it. This is that call, and the request going through
// it is the one above.
namespace {

core::trace::Formatted hook(const Redactor& redactor, char* buffer, std::size_t capacity,
                            const char* format, ...) {
  std::va_list args;
  va_start(args, format);
  const core::trace::Formatted out =
      core::trace::formatAndRedact(redactor, buffer, capacity, format, args);
  va_end(args);
  return out;
}

}  // namespace

TEST_CASE("a request with a known token through the hook, and the token is in nothing it wrote") {
  Redactor redactor;
  CHECK(redactor.keep(0, kToken));
  CHECK(redactor.keep(1, kPin));
  const std::string payload = escaped(realRequestHead(kToken));

  char buffer[4096] = {};
  // ESP-IDF's LOG_FORMAT(D, "TX: %.*s") as the library's line is composed: the timestamp and the
  // tag first, then the escaped payload.
  const core::trace::Formatted line =
      hook(redactor, buffer, sizeof buffer, "D (%lu) %s: TX: %.*s\n", 54321UL, "WalterModem",
           static_cast<int>(payload.size()), payload.c_str());
  const std::string wrote(buffer, line.length);
  CHECK(line.length > 0);
  CHECK(line.taken >= 1);
  CHECK(wrote.find(kToken) == std::string::npos);
  CHECK(wrote.find("zQ7x4LpR") == std::string::npos);
  // What the hook wrote is still the line it was given: the prefix, the direction, the request.
  CHECK(wrote.find("D (54321) WalterModem: TX: POST /agent/v1/observations") == 0);
  CHECK(wrote.back() == '\n');
  // And the buffer holds nothing of the token past the line either (vsnprintf's terminator and
  // whatever was there before): the whole buffer is searched, not only the line.
  CHECK(std::string(buffer, sizeof buffer).find(kToken) == std::string::npos);

  SUBCASE("a line longer than the buffer is cut, and the cut does not let the token out") {
    char small[64] = {};
    const core::trace::Formatted cut =
        hook(redactor, small, sizeof small, "D (1) WalterModem: TX: Authorization: Bearer %s\n",
             kToken);
    CHECK(cut.length == sizeof small - 1);
    CHECK(std::string(small, sizeof small).find(kToken) == std::string::npos);
    // The eighteen bytes of it that did fit are blanked too, which is the point of the cut case.
    CHECK(std::string(small, sizeof small).find("zQ7x4LpR") == std::string::npos);
    CHECK(std::string(small, cut.length).find("Authorization: *") != std::string::npos);
  }

  SUBCASE("a hook with nowhere to write answers nothing") {
    const core::trace::Formatted none = hook(redactor, nullptr, 0, "TX: %s\n", kToken);
    CHECK(none.length == 0);
    CHECK(none.taken == 0);
  }
}

TEST_CASE("the SIM PIN goes out as an AT command, and comes back as asterisks") {
  Redactor redactor;
  CHECK(redactor.keep(1, kPin));
  // `unlockSIM` (WalterModem.cpp) sends `AT+CPIN=<pin>`, and the `_transmitCmd` macro logs its
  // elements unescaped, so this is the line as the board prints it.
  const Line line(redactor, std::string("D (1234) WalterModem: TX: AT+CPIN=") + kPin);
  CHECK_FALSE(line.has(kPin));
  CHECK(line.has("AT+CPIN=********"));
  CHECK(line.taken >= 1);

  SUBCASE("a query is not an assignment and keeps its shape") {
    const Line query(redactor, "D (1234) WalterModem: TX: AT+CPIN?");
    CHECK(query.str() == "D (1234) WalterModem: TX: AT+CPIN?");
    CHECK(query.taken == 0);
  }
}

TEST_CASE("the two positional rails hold for a value the redactor was never given") {
  // The token typed while the trace was already on, so the hook holds the one before it. The
  // position of the header is this firmware's own doing, so it is taken out all the same.
  Redactor redactor;
  CHECK(redactor.keep(0, "an-older-token-than-this"));
  const Line line(redactor,
                  "D (9) WalterModem: TX: POST /x HTTP/1.1\\r\\nAuthorization: Bearer "
                  "a-token-nobody-told-the-hook-about\\r\\nAccept: */*\\r\\n\\r\\n");
  CHECK_FALSE(line.has("a-token-nobody-told-the-hook-about"));
  // And the header after it is untouched: the rail ends at the end of the header, which in an
  // escaped line is the two characters of an escaped CR.
  CHECK(line.has("\\r\\nAccept: */*"));
  CHECK(line.has("POST /x HTTP/1.1"));
}

TEST_CASE("a value is taken out wherever it is, as often as it is there") {
  Redactor redactor;
  CHECK(redactor.keep(0, "SECRETVALUE"));
  const Line line(redactor, "RX: +SQNSRECV: 1,30\\r\\nSECRETVALUE and SECRETVALUE again");
  CHECK_FALSE(line.has("SECRETVALUE"));
  CHECK(line.taken == 2);
  CHECK(line.has("*********** and *********** again"));
  CHECK(line.has("+SQNSRECV: 1,30"));
}

TEST_CASE("what the redactor will not hold") {
  Redactor redactor;
  CHECK(redactor.holding() == 0);

  SUBCASE("an unset setting is an empty value and holds nothing") {
    CHECK(redactor.keep(0, ""));
    CHECK(redactor.keep(1, nullptr));
    CHECK(redactor.holding() == 0);
    const Line line(redactor, "RX: +SQNSRING: 1,876");
    CHECK(line.str() == "RX: +SQNSRING: 1,876");
    CHECK(line.taken == 0);
  }

  SUBCASE("a value longer than a setting's longest is refused, and nothing of it is held") {
    const std::string too_long(core::trace::kMaxSecret + 1, 'z');
    CHECK_FALSE(redactor.keep(0, too_long.c_str()));
    CHECK(redactor.holding() == 0);
    // Not half-held either: the first bytes of it are not searched for.
    const Line line(redactor, "RX: zzzzzzzzzz");
    CHECK(line.str() == "RX: zzzzzzzzzz");
  }

  SUBCASE("a value at the very length a setting allows is held") {
    const std::string longest(core::trace::kMaxSecret, 'q');
    CHECK(redactor.keep(0, longest.c_str()));
    CHECK(redactor.holding() == 1);
  }

  SUBCASE("a value shorter than kLeastSecret is not searched for: it would asterisk the trace") {
    CHECK(redactor.keep(0, "ab"));
    CHECK(redactor.holding() == 0);
    const Line line(redactor, "RX: +CME ERROR: 4 abab");
    CHECK(line.str() == "RX: +CME ERROR: 4 abab");
  }

  SUBCASE("a slot that is not a slot") {
    CHECK_FALSE(redactor.keep(core::trace::kSecrets, kToken));
    CHECK(redactor.holding() == 0);
  }

  SUBCASE("forget") {
    CHECK(redactor.keep(0, kToken));
    CHECK(redactor.keep(1, kPin));
    redactor.forget();
    CHECK(redactor.holding() == 0);
  }
}

TEST_CASE("the lines of a stall, which is what the trace exists for, are left alone") {
  Redactor redactor;
  CHECK(redactor.keep(0, kToken));
  CHECK(redactor.keep(1, kPin));
  // The four things docs/bench.md asks the trace for, and the lengths of 8 November (876 bytes
  // received, 877 and 878 lost): none of them is touched.
  const char* const kept[] = {
      "D (1000) WalterModem: TX: AT+SQNSRECV=1,1500",
      "D (1001) WalterModem: RX: \\r\\n+SQNSRECV: 1,876\\r\\n",
      "D (1002) WalterModem: RX: \\r\\n+SQNSRING: 6,878\\r\\n",
      "D (1003) WalterModem: RX: \\r\\n+SQNSH: 1\\r\\n",
      "D (1004) WalterModem: RX: \\r\\n+CME ERROR: 4\\r\\n",
      "W (1005) WalterModem: Command time-out (TX) Attempt 1 of 1",
  };
  for (const char* text : kept) {
    const Line line(redactor, text);
    CHECK(line.taken == 0);
    CHECK(line.str() == std::string(text));
  }
}

TEST_CASE("a line the redactor cannot be broken by") {
  Redactor redactor;
  CHECK(redactor.keep(0, kToken));

  SUBCASE("an empty line") {
    char nothing[1] = {0};
    CHECK(redactor.apply(nothing, 0) == 0);
    CHECK(redactor.apply(nullptr, 10) == 0);
  }

  SUBCASE("a header with no value at all") {
    const Line line(redactor, "TX: GET / HTTP/1.1\\r\\nAuthorization:\\r\\n\\r\\n");
    CHECK(line.taken == 0);
    CHECK(line.has("Authorization:"));
  }

  SUBCASE("a header cut off by the end of the buffer") {
    const Line line(redactor, std::string("TX: ...Authorization: Bearer ") + kToken);
    CHECK_FALSE(line.has(kToken));
    CHECK(line.has("Authorization: *"));
  }

  SUBCASE("the prefix at the very end of the line") {
    const Line line(redactor, "TX: Authorization:");
    CHECK(line.str() == "TX: Authorization:");
  }

  SUBCASE("two Authorization headers, both taken out") {
    const Line line(redactor,
                    std::string("TX: Authorization: Bearer ") + kToken + "\\r\\nAuthorization: " +
                        kToken + "\\r\\n");
    CHECK_FALSE(line.has(kToken));
    CHECK(line.taken >= 2);
  }

  SUBCASE("a line of real CR LF rather than escaped ones") {
    const Line line(redactor, std::string("TX: Authorization: Bearer ") + kToken +
                                  "\r\nAccept: */*\r\n");
    CHECK_FALSE(line.has(kToken));
    CHECK(line.has("\r\nAccept: */*"));
  }
}
