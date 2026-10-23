// The bounded reader of a job's payload (task p4-05, walter_agent/json_scan.h), held to the four
// payloads' own fixtures - meshmaster.dk's `FirmwareOfferPayload.json`, `FirmwareApplyPayload.json`,
// `FirmwareStatePayload.json` and `PowerCyclePayload.json`, read at run time - and to what it must
// step over without reading: nested objects and arrays, strings with brackets in them, escapes.
#include <doctest.h>

#include <cstring>
#include <string>

#include "agent/meshmaster_script.h"
#include "walter_agent/json_scan.h"

namespace json = agent::json;

namespace {

json::Value whole(const std::string& text, const char* name) { return json::member(text.c_str(), text.size(), name); }

}  // namespace

TEST_CASE("the offer's payload, field for field, out of the fixture's bytes") {
  const std::string text = bench::contractFixture("FirmwareOfferPayload");
  long long n = 0;
  char s[128];
  REQUIRE(json::integer(whole(text, "releaseId"), &n));
  CHECK(n == 7);
  REQUIRE(json::text(whole(text, "product"), s, sizeof s));
  CHECK(std::string(s) == "twinoak-repeater-firmware");
  REQUIRE(json::text(whole(text, "version"), s, sizeof s));
  CHECK(std::string(s) == "0.3.0+abc1234");
  REQUIRE(json::integer(whole(text, "slot"), &n));
  CHECK(n == 0);
  REQUIRE(json::text(whole(text, "base"), s, sizeof s));
  CHECK(std::string(s) == "/agent/v1/firmware/7");
  REQUIRE(json::text(whole(text, "expectedSha256"), s, sizeof s));
  CHECK(std::strlen(s) == 64);
  REQUIRE(json::integer(whole(text, "size"), &n));
  CHECK(n == 4096);
  CHECK_FALSE(whole(text, "firstInstall").found());
}

TEST_CASE("the apply's, the state's and the power cycle's payloads") {
  const std::string apply = bench::contractFixture("FirmwareApplyPayload");
  bool b = false;
  REQUIRE(json::boolean(whole(apply, "firstInstall"), &b));
  CHECK(b);
  char s[128];
  REQUIRE(json::text(whole(apply, "expectedSha256"), s, sizeof s));
  CHECK(std::string(s) == "662a969ee40557c75cdb9dec10d91477a848282914cdae9a740f08d1a4372b0a");
  long long n = 0;
  REQUIRE(json::integer(whole(apply, "releaseId"), &n));
  CHECK(n == 7);

  const std::string state = bench::contractFixture("FirmwareStatePayload");
  CHECK(state == "{}");
  CHECK_FALSE(whole(state, "anything").found());

  const std::string cycle = bench::contractFixture("PowerCyclePayload");
  REQUIRE(json::integer(whole(cycle, "offSeconds"), &n));
  CHECK(n == 5);
}

TEST_CASE("nested containers and strings are stepped over whole; the member found is the one at depth one") {
  const std::string text =
      "{ \"a\" : {\"base\":\"inner\", \"x\":[1,2,{\"y\":3}]}, \"list\":[\"base\", {\"base\":1}], "
      "\"s\":\"a \\\"quoted\\\" {brace} and [bracket]\", \"base\":\"/outer\" ,\"n\":-42,\"t\":true,\"f\":false,\"z\":null}";
  char s[64];
  REQUIRE(json::text(whole(text, "base"), s, sizeof s));
  CHECK(std::string(s) == "/outer");
  REQUIRE(json::text(whole(text, "s"), s, sizeof s));
  CHECK(std::string(s) == "a \"quoted\" {brace} and [bracket]");
  long long n = 0;
  REQUIRE(json::integer(whole(text, "n"), &n));
  CHECK(n == -42);
  bool b = true;
  REQUIRE(json::boolean(whole(text, "f"), &b));
  CHECK_FALSE(b);
  CHECK(json::isNull(whole(text, "z")));
  CHECK(json::isObject(whole(text, "a")));
  // The array's elements, by index.
  const json::Value list = whole(text, "list");
  REQUIRE(list.found());
  REQUIRE(json::text(json::element(list, 0), s, sizeof s));
  CHECK(std::string(s) == "base");
  CHECK(json::isObject(json::element(list, 1)));
  CHECK_FALSE(json::element(list, 2).found());
  // The kinds do not lie: a string is not an integer, a number not a string, a bool not a number.
  CHECK_FALSE(json::integer(whole(text, "base"), &n));
  CHECK_FALSE(json::text(whole(text, "n"), s, sizeof s));
  CHECK_FALSE(json::integer(whole(text, "t"), &n));
}

TEST_CASE("what is not found: a missing member, a malformed object, text that ends early, a string too long") {
  char s[8];
  CHECK_FALSE(whole("{\"a\":1}", "b").found());
  CHECK_FALSE(whole("[1,2]", "a").found());
  CHECK_FALSE(whole("{\"a\":{\"b\":1", "a").found());
  CHECK_FALSE(whole("{\"a\":\"unterminated", "a").found());
  CHECK_FALSE(json::text(whole("{\"a\":\"too long for eight\"}", "a"), s, sizeof s));
  CHECK_FALSE(json::member(nullptr, 0, "a").found());
  // An integer that would overflow is not an integer.
  long long n = 0;
  CHECK_FALSE(json::integer(whole("{\"a\":99999999999999999999}", "a"), &n));
  // Escapes: the two-character forms are undone; a \u escape becomes one `?`.
  char u[32];
  REQUIRE(json::text(whole("{\"a\":\"x\\u00e5y\\/z\\n\"}", "a"), u, sizeof u));
  CHECK(std::string(u) == "x?y/z\n");
}
