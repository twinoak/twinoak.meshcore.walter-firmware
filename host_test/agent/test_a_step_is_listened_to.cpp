// A step is listened to (task p18-03), the half of it that is the job's: no call to the server
// between a step's command and its end, and a lease seen to between the steps.
//
// **21 November, Bellevue, meshmaster.dk job 39506.** The fifth step of the first session a unit
// ever ran was `hist 869.418 869.818 25`. The scanner numbered its lines `seq` 245 to 260, sixteen
// of about 170 bytes 22 ms apart, and its `done` 261. The server holds 245 to 251 and 261: nine
// measurement lines are not there, and the step's result says *done, 9 lines* with nothing
// anywhere saying that any are missing.
//
// Both halves of the cause were on this side of the UART. The job wrote its *step 5* line and
// called `postApplyProgress` the moment the step had begun - a second or more over LTE-M with the
// scanner already speaking and nobody reading the port - and the driver's receive ring was 1 024
// bytes, which is seven of those lines.
//
// So the first case here is that step, with a progress call that takes two seconds. It is the
// case that fails on the code as it was, with about seven lines kept. The rest are the rules that
// follow: the lease between the steps and never inside one, a loss counted and said when a ring is
// made too small on purpose, and the scope step of that same session unchanged.
//
// The machine's own half - the entry before the command, the ring's arithmetic, ten seconds at the
// baud rate, the journal entry of a lost step - is
// host_test/core/update/test_a_step_is_listened_to.cpp.
#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agent/session_job.h"
#include "core/update/scanner_session.h"
#include "sim/scripted_scanner.h"
#include "walter_agent/firmware_jobs.h"

using bench::bodyLines;
using bench::has;
using bench::SessionJob;
using bench::SlowFlash;
using bench::stepRows;
using core::cycle::Pass;
namespace upd = core::update;

namespace {

// Every `"seq":N` of a body, in the order it appears: what the scanner numbered its lines, as the
// server got them. A gap here is a measurement nobody has.
std::vector<long long> seqsOf(const std::string& body) {
  std::vector<long long> seqs;
  std::size_t at = 0;
  while ((at = body.find("\"seq\":", at)) != std::string::npos) {
    at += 6;
    long long value = 0;
    bool any = false;
    while (at < body.size() && body[at] >= '0' && body[at] <= '9') {
      value = value * 10 + (body[at] - '0');
      ++at;
      any = true;
    }
    if (any) seqs.push_back(value);
  }
  return seqs;
}

std::vector<long long> seqsOfPosts(const std::vector<bench::SeenLines>& posts) {
  std::vector<long long> seqs;
  for (const bench::SeenLines& post : posts) {
    const std::vector<long long> some = seqsOf(post.body);
    seqs.insert(seqs.end(), some.begin(), some.end());
  }
  return seqs;
}

// The lines of every post of one job, in order.
std::vector<std::string> allLines(const std::vector<bench::SeenLines>& posts) {
  std::vector<std::string> lines;
  for (const bench::SeenLines& post : posts) {
    const std::vector<std::string> some = bodyLines(post.body);
    for (const std::string& one : some) {
      if (!one.empty()) lines.push_back(one);
    }
  }
  return lines;
}

// Where the lease stood at `at_ms`, from what the server recorded: a lease is set to *now plus
// what was asked* at every extension, so the newest extension at or before the moment is the one
// that decides.
std::uint64_t leaseAt(const std::vector<bench::SeenExtend>& extends, std::uint64_t at_ms) {
  std::uint64_t until = 0;
  for (const bench::SeenExtend& one : extends) {
    if (one.at_ms <= at_ms && !one.refused) {
      until = one.at_ms + static_cast<std::uint64_t>(one.seconds) * 1000u;
    }
  }
  return until;
}

// A command this scanner was told, and when.
struct Told {
  std::string line;
  std::uint64_t at_ms = 0;
};

}  // namespace

// ------------------------------------------------------------------ 21 November

TEST_CASE("the hist step of 21 November: sixteen lines and a done, seq unbroken") {
  SessionJob unit;
  // **Every call to the server takes two seconds**, which is what an LTE-M connect and handshake
  // cost that evening. On the code as it was this is what took the nine lines.
  unit.world.http.socket.connect_ms = 2000;
  // The step, as the scanner ran it: sixteen lines of about 170 bytes, 22 ms apart, numbered from
  // 245, and a `done` numbered 261.
  bench::Answer hist;
  hist.lines = 16;
  hist.line_bytes = 170;
  hist.first_seq = 245;
  hist.done = true;
  unit.scripted.answers["hist"] = hist;
  unit.scripted.first_line_ms = 22;
  unit.scripted.between_ms = 22;
  unit.server().queue(bench::ownJob(60, agent::kSessionType,
                                    bench::sessionPayload({{"hist 869.418 869.818 25", 40}}))
                          .json());

  const Pass& pass = unit.oneWake();
  INFO(unit.v().out.text);
  CHECK(pass.ending == core::cycle::Ending::kSlept);

  // **The sixteen, and the `done`.** `seq` 245 to 260 and then 261, with nothing missing between
  // them - which is the one thing the server could not say about that step and now can.
  const std::vector<long long> seqs = seqsOfPosts(unit.posts(60));
  std::vector<long long> wanted;
  for (long long n = 245; n <= 261; ++n) wanted.push_back(n);
  INFO("seq: " << seqs.size() << " line(s)");
  CHECK(seqs == wanted);
  // The step's own lines: the greeting's `ack`s and its `info` line are the first step's, so there
  // are a few more of them than the step's own seventeen.
  const std::vector<std::string> lines = allLines(unit.posts(60));
  CHECK(lines.size() > 17);
  std::size_t hist_lines = 0;
  for (const std::string& line : lines) hist_lines += has(line, "\"t\":\"hist\"") ? 1 : 0;
  CHECK(hist_lines == 16);
  CHECK(has(lines.back(), "\"t\":\"done\""));

  // Nothing was lost at the port, and nothing says anything was: the step's journal has no second
  // entry and the result's journal no line about one.
  CHECK(unit.world.rak.lost_bytes == 0);
  CHECK(unit.v().link.counters().port_losses == 0);
  CHECK(unit.half().firmware.counters().steps_lossy == 0);
  const std::string data = unit.lastData(60);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  REQUIRE(unit.posts(60).size() == 1);
  CHECK(stepRows(data) == "done/" + std::to_string(lines.size()) + "/" +
                              std::to_string(unit.posts(60)[0].body.size()) + " ");
  CHECK_FALSE(has(data, "lines are missing"));

  // **And no call fell inside the step.** The fixture's hook watches every request of the API and
  // fails the case on one that arrives while the machine is listening to a step; this is the same
  // thing counted by the firmware itself, which refuses such a call rather than making it.
  CHECK(unit.calls_in_a_step.empty());
  CHECK(unit.half().firmware.counters().calls_in_step == 0);
  // The progress post of *step 1* went out all the same - before the command, which is the whole
  // of the change - so the job's page still says what the unit is doing.
  bool said_step_one = false;
  for (const std::string& report : unit.server().progress[60]) {
    said_step_one = said_step_one || has(report, "\"note\":\"step-1\"");
  }
  CHECK(said_step_one);
}

// ------------------------------------------------------------------ the lease between the steps

TEST_CASE("a program of 600 s: the lease covers every step, and is never asked for inside one") {
  SessionJob unit;
  // Two steps of the longest a step may be, which is `kProgramSecondsMax` over the program: the
  // lease's own five-minute timer (`kExtendEveryMs`) falls inside each of them.
  bench::Answer slow;
  slow.lines = 5;
  slow.line_bytes = 200;
  slow.done = false;  // each step runs its seconds out
  unit.scripted.answer = slow;
  unit.scripted.first_line_ms = 1000;
  unit.scripted.between_ms = 10000;
  std::vector<Told> told;
  unit.scripted.on_command = [&unit, &told](const std::string& line) {
    told.push_back(Told{line, unit.world.clock.nowMs()});
  };
  unit.server().queue(bench::ownJob(61, agent::kSessionType,
                                    bench::sessionPayload({{"dwell 869.618 60", 300},
                                                           {"scope 863 870 250 0 1000", 300}}))
                          .json());
  // And the two flashes take their quarter of an hour, so that the lease is a real question.
  SlowFlash flash(unit.world.clock, 930000);
  unit.boot();
  unit.v().update.setProgress(&flash);
  unit.v().begin();
  (void)unit.v().run();
  REQUIRE(flash.flashes() == 2);

  const std::vector<bench::SeenExtend>& extends = unit.server().extensions[61];
  REQUIRE_FALSE(extends.empty());
  // The two commands, and when each was said.
  std::vector<Told> commands;
  for (const Told& one : told) {
    if (one.line.rfind("dwell", 0) == 0 || one.line.rfind("scope", 0) == 0) commands.push_back(one);
  }
  REQUIRE(commands.size() == 2);
  // **The lease covered the step before it began**: its 300 seconds, the posting of its lines and
  // the calls that follow (`kLeaseAfterStepMs`).
  for (std::size_t i = 0; i < commands.size(); ++i) {
    CAPTURE(i);
    const std::uint64_t until = leaseAt(extends, commands[i].at_ms);
    const std::uint64_t needed = commands[i].at_ms + 300000u + agent::kLeaseAfterStepMs;
    INFO("said at " << commands[i].at_ms << ", lease until " << until << ", needed " << needed);
    const bool the_lease_covers_the_step = until >= needed;
    CHECK(the_lease_covers_the_step);
  }
  // **And none of them was asked for inside a step.** Every extension landed either before a
  // step's command or after the step had ended, which the fixture's hook holds at every call.
  CHECK(unit.calls_in_a_step.empty());
  CHECK(unit.half().firmware.counters().calls_in_step == 0);
  // The steps ran their clocks out and the repeater is back.
  const std::string data = unit.lastData(61);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(has(stepRows(data), "clock/"));
  CHECK(unit.world.chip->chipHolds(unit.golden));
  CHECK_FALSE(unit.sessionIsOpen());
  // Nothing of the port was lost in either of them: the loop read it every tenth of a second for
  // ten minutes.
  CHECK(unit.v().link.counters().port_losses == 0);
}

// ------------------------------------------------------------------ a loss, counted and said

TEST_CASE("a ring made too small on purpose: the loss is in the journal and in the result") {
  SessionJob unit;
  unit.world.rak.rx_ring = 512;  // an eighth of the board's, and smaller than two lines
  bench::Answer many;
  many.lines = 10;
  many.line_bytes = 400;
  unit.scripted.answers["sweep"] = many;
  unit.scripted.first_line_ms = 20;
  unit.scripted.between_ms = 20;
  unit.server().queue(
      bench::ownJob(62, agent::kSessionType, bench::sessionPayload({{"sweep", 20}})).json());

  (void)unit.oneWake();
  INFO(unit.v().out.text);
  // The port lost bytes, and the count reached the client's counters and the console's line.
  CHECK(unit.world.rak.lost_bytes > 0);
  CHECK(unit.v().link.counters().port_losses > 0);
  CHECK(unit.half().firmware.counters().steps_lossy == 1);
  CHECK(has(unit.v().runLine("rak text"), "the port lost bytes "));
  CHECK(has(unit.v().runLine("agent jobs"), "1 step(s) lost bytes at the port"));

  // **And the result carries that journal line to the server**, with the fields of the contract
  // exactly as they were: `ended`, `lines` and `bytes` and nothing added to a step's row.
  const std::string data = unit.lastData(62);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  const std::string journal = bench::objectOf(data, "journal");
  CHECK(has(journal, "lines are missing"));
  CHECK(has(journal, "\"step\":\"step-1\""));
  const std::string row = bench::objectAt(bench::objectOf(data, "steps"), 0);
  CHECK(bench::topKeys(row) == std::vector<std::string>{"ended", "lines", "bytes"});
  // The step still ended and the session still closed: a loss is said, not acted on. It ended
  // `clock` and not `done` because the scanner's own `done` line was among what the port lost -
  // which is exactly the thing a reader of that step could not otherwise tell from a step whose
  // measurement simply took the whole of its seconds.
  CHECK(bench::member(row, "ended") == "clock");
  CHECK_FALSE(unit.sessionIsOpen());
  CHECK(unit.world.chip->chipHolds(unit.golden));
  CHECK(unit.calls_in_a_step.empty());
}

TEST_CASE("a session that lost nothing says nothing: the result is the shape it always was") {
  SessionJob unit;
  bench::Answer three;
  three.lines = 3;
  three.line_bytes = 200;
  unit.scripted.answer = three;
  unit.server().queue(
      bench::ownJob(63, agent::kSessionType, bench::sessionPayload({{"stat", 20}, {"cad", 20}}))
          .json());
  (void)unit.oneWake();
  const std::string data = unit.lastData(63);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK_FALSE(has(data, "lines are missing"));
  CHECK(unit.half().firmware.counters().steps_lossy == 0);
  CHECK(unit.v().link.counters().port_losses == 0);
  // The journal is the ten lines of a good run and no more: a step is one line, as it has always
  // been.
  const std::vector<std::string> steps = bench::stepsOf(bench::objectOf(data, "journal"));
  CHECK(steps == std::vector<std::string>{"check", "flashing-scanner", "scanner", "step-1",
                                           "step-2", "room", "flashing-golden", "booting",
                                           "proving", "restored"});
}

// ------------------------------------------------------------------ the scope step, as before

TEST_CASE("a scope step of a frame a second for sixty seconds: every frame, and no loss") {
  SessionJob unit;
  // What the three scope steps of 21 November sent: a frame of about 450 bytes a second. They are
  // not known to have lost one - their `seq` was never checked - and a slower server would have
  // cost them one, so this case is here to say that they still do not.
  bench::Answer scope;
  scope.lines = 60;
  scope.line_bytes = 450;
  scope.first_seq = 300;
  scope.done = false;
  unit.scripted.answers["scope"] = scope;
  unit.scripted.first_line_ms = 1000;
  unit.scripted.between_ms = 1000;
  unit.world.http.socket.connect_ms = 2000;  // and a server as slow as that evening's
  unit.server().queue(bench::ownJob(64, agent::kSessionType,
                                    bench::sessionPayload({{"scope 863 870 250 0 1000", 70}}))
                          .json());
  // The big room, so that sixty frames of 450 bytes are kept whole rather than ending `full`.
  unit.boot();
  unit.v().scratch.externalOf(2u * 1024u * 1024u);
  unit.v().begin();
  (void)unit.v().run();

  const std::vector<std::string> lines = allLines(unit.posts(64));
  std::size_t frames = 0;
  for (const std::string& line : lines) frames += line.size() == scope.line_bytes ? 1 : 0;
  CHECK(frames == scope.lines);
  // `seq` 300 to 359, unbroken.
  const std::vector<long long> seqs = seqsOfPosts(unit.posts(64));
  std::vector<long long> wanted;
  for (long long n = 300; n <= 359; ++n) wanted.push_back(n);
  std::vector<long long> measured;
  for (long long one : seqs) {
    if (one >= 300) measured.push_back(one);
  }
  CHECK(measured == wanted);
  CHECK(unit.v().link.counters().port_losses == 0);
  CHECK(unit.calls_in_a_step.empty());
  const std::string data = unit.lastData(64);
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(has(stepRows(data), "clock/"));
}
