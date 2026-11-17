// What a call that worked looks like, written down (task p15-07).
//
// `test_call_record.cpp` is the 8 November stall: the request goes out, the modem answers *nothing
// buffered yet* to the polls, a ring announces the answer, the close arrives, and one command goes
// unanswered for the library's whole 30 seconds. What could not be said was what is **different**
// about a call that works. The config, the heartbeat, the claim and a job's result are answered at
// every wake by the same server through the same code, and nothing was kept of them: does their
// answer come in two pieces? does the server close behind it? when is the receive sent, against
// the rings? Until a good call can be laid beside a bad one, every cure is a guess.
//
// So these are the two things that task adds to the record itself, each through the firmware's own
// HTTP/1.1 and client over the bench's socket:
//
//   * **each receive's two times** - when it was asked and when it came back - for a receive that
//     returned bytes as for the one that failed, so that a read can be placed against the rings in
//     both;
//   * **what the answer said of itself**: the status, whether it was framed by a length or by
//     chunks or by the close, how many bytes of body were read, whether it said `Connection:
//     close`, and whether the other end's close was handed over before or after the last byte.
//
// And the rail under both: a record kept by a firmware before this task still reads, and says
// *nothing* about when a receive was asked rather than a figure it never wrote down.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/net/call_record.h"
#include "fakes/capture.h"
#include "fakes/sim_key_value.h"
#include "net_bench.h"

using bench::MemorySink;
using bench::NetBench;
using core::net::CallMoment;
using core::net::CallRecord;
using core::net::CallStep;
using core::net::Options;
using core::net::Result;
using interfaces::HttpOutcome;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

std::string wordsOf(const CallRecord& record, const char* what = "claim",
                    const char* lead = "in the latest call that was answered") {
  fakes::CaptureOut out;
  record.describe(out, what, lead);
  return out.text;
}

std::size_t countOf(const CallRecord& record, CallMoment what) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < record.steps(); ++i) {
    if (record.step(i).what == what) ++n;
  }
  return n;
}

CallStep firstOf(const CallRecord& record, CallMoment what) {
  for (std::size_t i = 0; i < record.steps(); ++i) {
    if (record.step(i).what == what) return record.step(i);
  }
  return CallStep();
}

// A record of exactly the shape the firmware **before** this task kept: the packed layout of
// `core/net/call_record.h` at version 1, with a ring and a receive that returned 300 bytes, and
// the sixth byte of that receive 0 - because nothing wrote a duration there. Built by hand to the
// layout; no byte of it was read off a unit.
constexpr const char* kRecordBeforeThisTask =
    "010302900101003200032c01004600012c0100000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000";

// The server's answer, and the modem under it, for a call that works: the response comes in
// several pieces (`piece_bytes`), a ring is handed over while they are being read, and the close
// at `close_after_ms` after the request went out.
void aCallThatWorks(NetBench& b, std::size_t body_bytes, std::size_t piece_bytes,
                    std::uint32_t close_after_ms) {
  b.server().serve("/agent/v1/jobs/claim", std::vector<std::uint8_t>(body_bytes, 'j'));
  b.socket().piece_bytes = piece_bytes;
  b.socket().piece_ms = 20;
  b.socket().ring_after_ms = 10;
  b.socket().ring_bytes = static_cast<std::uint16_t>(body_bytes);
  b.socket().close_after_ms = close_after_ms;
}

Result theClaim(NetBench& b) {
  MemorySink sink;
  Options options;
  options.authorization = "Bearer a-token-nobody-may-print";
  options.sensitive = true;
  const std::string sent = "{\"max\":2}";
  return b.client.post("https://server.example/agent/v1/jobs/claim",
                       reinterpret_cast<const std::uint8_t*>(sent.data()), sent.size(),
                       "application/json", options, sink);
}

}  // namespace

TEST_CASE("a call that worked: every receive's two times, and what the answer said of itself") {
  NetBench b;
  // Pieces of 64 bytes, so there are several receives to place against the ring, and the close
  // handed over 30 ms after the request went out - while the body is still being read.
  aCallThatWorks(b, 300, 64, 30);
  const Result r = theClaim(b);
  CHECK(r.status == 200);
  CHECK(r.outcome == HttpOutcome::kOk);

  const CallRecord& record = b.http.call_record;
  REQUIRE(record.held());
  CHECK(record.outcome() == HttpOutcome::kOk);
  CHECK(record.socketId() == b.socket().socket_ids.back());

  // **Each receive's two times.** Every receive that returned bytes says how long its command
  // took, so when it was asked is `at_ms - took_ms` - the arithmetic a person had to do by hand
  // against the rings, now done for them.
  REQUIRE(countOf(record, CallMoment::kData) >= 3);
  for (std::size_t i = 0; i < record.steps(); ++i) {
    const CallStep s = record.step(i);
    if (s.what != CallMoment::kData) continue;
    CHECK(s.took_known);
    CHECK_FALSE(s.took_or_more);
    CHECK(s.took_ms == 20);        // the bench's piece_ms: one command, one round trip
    CHECK(s.at_ms >= s.took_ms);   // it came back after it was asked
    // The sixth byte is a duration here and not a modem code: nothing reads it as one.
    CHECK_FALSE(s.had_cme);
  }

  // The ring, to place those reads against.
  const CallStep ring = firstOf(record, CallMoment::kRing);
  REQUIRE(ring.what == CallMoment::kRing);
  CHECK(ring.value == 300);

  // **What the answer said of itself**: the status, the framing, the bytes, the `Connection:
  // close` - and nothing else of any header.
  CHECK(record.answerKnown());
  CHECK(record.answerStatus() == 200);
  CHECK(record.answerHadLength());
  CHECK_FALSE(record.answerWasChunked());
  CHECK(record.answerSaidClose());
  CHECK(record.answerBytes() == 300);

  const std::string said = wordsOf(record);
  INFO(said);
  CHECK(has(said, "what the modem said in the latest call that was answered (claim), on socket "));
  CHECK(has(said, "it ended as: ok"));
  CHECK(has(said, "a ring: the modem announced 300 bytes"));
  CHECK(has(said, " (asked at +"));
  CHECK(has(said, "ms): a receive returned "));
  CHECK(has(said,
            "the answer: http 200, framed by a length, 300 bytes of body read, and it said "
            "Connection: close"));
  // The close arrived while the body was still being read, which is the one thing the numbers
  // above cannot say.
  CHECK(has(said, "the other end's close was handed over before the last byte was read"));
}

TEST_CASE("the failed receive says when it was asked too, in the same words") {
  NetBench b;
  b.server().serve("/agent/v1/observations", std::vector<std::uint8_t>(878, 'x'));
  b.socket().not_yet_receives = 3;
  b.socket().not_yet_ms = 50;
  b.socket().receive_hangs_ms = 30000;   // the library's command timeout
  b.socket().receive_state = 2;          // WALTER_MODEM_STATE_TIMEOUT: nothing answered
  b.socket().receive_cme = -1;
  b.socket().ring_after_ms = 120;
  b.socket().ring_bytes = 878;
  b.socket().close_after_ms = 500;
  MemorySink sink;
  const Result r = b.client.post("https://server.example/agent/v1/observations",
                                 reinterpret_cast<const std::uint8_t*>("{}"), 2,
                                 "application/json", Options{}, sink);
  CHECK(r.status == 0);

  const CallRecord& record = b.http.call_record;
  const CallStep failed = firstOf(record, CallMoment::kReceiveFailed);
  REQUIRE(failed.what == CallMoment::kReceiveFailed);
  // The duration was always in the record; what is new is that the line does the subtraction.
  CHECK(failed.took_known);
  CHECK(failed.took_ms == failed.value);
  const std::string said = wordsOf(record, "observations", "in it");
  INFO(said);
  CHECK(has(said, "ms (asked at +"));
  CHECK(has(said, "a receive failed after 30000 ms"));
  // Nothing answered, so there is nothing for the answer's own lines to say.
  CHECK_FALSE(record.answerKnown());
  CHECK_FALSE(has(said, "the answer: http"));
  CHECK_FALSE(has(said, "the other end's close was"));
}

TEST_CASE("a chunked answer, and one framed by the close that comes after the last byte") {
  NetBench b;
  sim::ServedFile chunked;
  chunked.bytes = bench::patternBytes(240);
  chunked.chunked = true;
  chunked.chunk_size = 60;
  b.server().files["/agent/v1/jobs/claim"] = chunked;
  b.socket().close_after_ms = 30;
  {
    const Result r = theClaim(b);
    REQUIRE(r.status == 200);
    const CallRecord& record = b.http.call_record;
    CHECK(record.answerWasChunked());
    CHECK_FALSE(record.answerHadLength());
    CHECK(record.answerBytes() == 240);
    const std::string said = wordsOf(record);
    INFO(said);
    CHECK(has(said, "the answer: http 200, chunked, 240 bytes of body read, and it said "
                    "Connection: close"));
  }

  // A body with neither framing header: it ends with the connection, so the close is what
  // completes it and is handed over **after** the last byte was read. One receive takes the whole
  // of it, and the close comes at the next ask.
  sim::ServedFile until_close;
  until_close.bytes = bench::patternBytes(240, 2);
  until_close.no_length = true;
  b.server().files["/agent/v1/jobs/claim"] = until_close;
  b.socket().piece_bytes = 1500;
  b.socket().close_after_ms = 1;
  const Result r = theClaim(b);
  REQUIRE(r.status == 200);
  const CallRecord& record = b.http.call_record;
  CHECK_FALSE(record.answerHadLength());
  CHECK_FALSE(record.answerWasChunked());
  CHECK(record.answerBytes() == 240);
  const std::string said = wordsOf(record);
  INFO(said);
  CHECK(has(said, "the answer: http 200, framed by nothing, so it ran to the close, 240 bytes of "
                  "body read, and it said Connection: close"));
  CHECK(has(said, "the other end's close was handed over after the last byte was read"));
}

TEST_CASE("a receive whose command took longer than the byte it is kept in says so") {
  NetBench b;
  aCallThatWorks(b, 300, 64, 0);
  b.socket().piece_ms = 400;  // past kCallTookAtMost, which is one byte with the plus-one bias
  const Result r = theClaim(b);
  REQUIRE(r.status == 200);
  const CallRecord& record = b.http.call_record;
  const CallStep data = firstOf(record, CallMoment::kData);
  REQUIRE(data.what == CallMoment::kData);
  CHECK(data.took_known);
  CHECK(data.took_ms == core::net::kCallTookAtMost);
  CHECK(data.took_or_more);
  // The clause says *or earlier*, so nobody reads the moment it was asked as exact.
  CHECK(has(wordsOf(record), " ms or earlier)"));
}

TEST_CASE("a record kept before this task still prints, and invents no time for its receives") {
  fakes::SimKeyValue kv;
  kv.put(core::net::kCallRecordKey, kRecordBeforeThisTask);
  CallRecord record;
  REQUIRE(record.load(&kv));
  CHECK(record.socketId() == 2);
  CHECK(record.bytesSent() == 400);
  CHECK(record.notYets() == 1);
  CHECK(countOf(record, CallMoment::kData) == 1);
  const CallStep data = firstOf(record, CallMoment::kData);
  // The duration was never written down, so the record says nothing about when it was asked -
  // rather than a 0 that would read as *it came back in the same millisecond*.
  CHECK_FALSE(data.took_known);
  CHECK(data.value == 300);

  const std::string said = wordsOf(record, "observations", "in it");
  INFO(said);
  CHECK(has(said, "what the modem said in it (observations), on socket 2: 400 bytes sent"));
  CHECK(has(said, "+50 ms: a ring: the modem announced 300 bytes"));
  CHECK(has(said, "+70 ms: a receive returned 300 bytes"));
  CHECK_FALSE(has(said, "asked at"));
  // And nothing of an answer: those four facts are not in the stored bytes at all, so a record
  // from any earlier firmware says exactly what it always said.
  CHECK_FALSE(record.answerKnown());
  CHECK_FALSE(has(said, "the answer: http"));
}

TEST_CASE("a record copied out of the live one holds everything, and the live one goes on") {
  NetBench b;
  aCallThatWorks(b, 300, 64, 30);
  REQUIRE(theClaim(b).status == 200);
  CallRecord kept;
  kept.copyFrom(b.http.call_record);
  const std::string of_the_claim = wordsOf(kept);
  REQUIRE(has(of_the_claim, "the answer: http 200"));

  // The next call overwrites the live record; the copy is untouched, which is the whole reason a
  // call that worked is copied out of it at all.
  b.socket().not_yet_receives = 2;
  b.socket().receive_hangs_ms = 30000;
  b.socket().receive_state = 2;
  b.socket().receive_cme = -1;
  MemorySink sink;
  (void)b.client.post("https://server.example/agent/v1/observations",
                      reinterpret_cast<const std::uint8_t*>("{}"), 2, "application/json",
                      Options{}, sink);
  CHECK(b.http.call_record.outcome() != HttpOutcome::kOk);
  CHECK(wordsOf(kept) == of_the_claim);

  // And a copy is never in the store: nothing of it is written, and dropping it leaves nothing.
  fakes::SimKeyValue kv;
  CHECK_FALSE(kv.has(core::net::kCallRecordKey));
  kept.drop();
  CHECK_FALSE(kept.held());
  CHECK(has(wordsOf(kept), "no call has been written down yet"));
}
