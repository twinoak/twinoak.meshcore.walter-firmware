// A power cut at every step of `rak provision` (task p5-03; spec/SYSTEM.md S-5a: "A failure
// anywhere leaves the radio erased or as it was, and the command starts from erased, so it is
// simply run again: no journal"). The simulated chip and the Walter both lose power - at every
// flash and UICR operation of the write, at the chip erase itself, and at the transfers of the
// steps between them (attach, the blank check, the halt, the verify, the look after the reset) -
// then both come back, `rak provision check` says exactly what it sees, and the line it says to
// type succeeds, with the base on the chip word for word.
#include <doctest.h>

#include <set>
#include <string>
#include <vector>

#include "core/provision/provision_bench.h"

using bench::Factory;
using bench::ProvUnit;
using bench::StepRecorder;
using core::provision::Status;
using core::provision::Step;
namespace prov = core::provision;

namespace {

const char* rightName(Factory f) { return bench::isLocked(f) ? "locked" : bench::kDeviceId; }

// After the power is back: what `check` sees, then the line it says, then the chip holds the base.
void recoverAndFinish(ProvUnit& u, const std::string& where) {
  u.powerCutBoth();
  const std::string check = u.runLine("rak provision check");
  INFO(where << "\n" << check);
  const std::string next = bench::nextLineOf(check);
  REQUIRE_MESSAGE(!next.empty(), where << ": check gave no line to type:\n" << check);
  CHECK(bench::hasLine(check, "protection: "));
  CHECK(bench::hasLine(check, "base: sha256"));
  const std::string answer = u.runLine(next.c_str());
  INFO(answer);
  REQUIRE_MESSAGE(bench::hasLine(answer, "PROVISIONED"), where << ": " << next << " did not finish:\n" << answer);
  u.checkChipHoldsBase();
  CHECK(u.v().supervisor.awaitingFirstInstall());
  CHECK(u.v().pins.violations.empty());
}

}  // namespace

TEST_CASE("a power cut at every flash and UICR operation, and at the chip erase: the same command, typed again, finishes") {
  for (const Factory f : {Factory::kLegacyLocked, Factory::kImprovedLocked, Factory::kImprovedOpen}) {
    // A clean run first, to count the operations: the erase, every flash word, every UICR word.
    unsigned ops = 0;
    {
      ProvUnit u(f);
      const prov::Result r = u.run(rightName(f));
      REQUIRE(r.ok());
      ops = u.chip().counters().erase_alls + u.chip().counters().writes + u.chip().counters().uicr_writes;
    }
    REQUIRE(ops > 50);  // the stand-in base: 54 flash words, 2 UICR words, APPROTECT, the erase
    unsigned cuts = 0;
    unsigned ended_at[prov::kStepCount] = {};
    ProvUnit u(f);
    for (unsigned op = 1; op <= ops; ++op) {
      // The chip back in its factory state, with this cut set; the base stays in the store and
      // the unit's own memory (the journal, RTC) is what a real unit would have after the cuts
      // before this one.
      u.world.factory(f);
      u.chip().powerOn();  // the countdown restarts at a power-on, as the chip's does
      u.chip().lose_power_at_op = op;
      u.reboot();
      const prov::Result r = u.run(rightName(f));
      REQUIRE_MESSAGE(!r.ok(), bench::describe(f) << ": the power went at op " << op << " and the run did not notice");
      ended_at[static_cast<std::size_t>(r.ended)]++;
      CHECK(!u.chip().powered());
      ++cuts;
      recoverAndFinish(u, std::string(bench::describe(f)) + ", the power cut at operation " + std::to_string(op));
    }
    MESSAGE(std::string(bench::describe(f)) << ": " << cuts << " power cuts at the chip's operations (1 erase, the rest writes); the run ended in: erase "
                                << ended_at[4] << ", write " << ended_at[5] << ", verify " << ended_at[6] << ", reset " << ended_at[7]);
  }
}

TEST_CASE("a power cut at the transfers between the writes - attach, blank check, halt, verify, reset, the look - and the same again") {
  for (const Factory f : {Factory::kLegacyLocked, Factory::kImprovedLocked, Factory::kImprovedOpen}) {
    // A clean run, recording the target's transfer count at the end of each step.
    StepRecorder steps;
    {
      ProvUnit u(f);
      steps.target = &u.v().target;
      steps.clock = &u.clock();
      const prov::Result r = u.run(rightName(f), &steps);
      REQUIRE(r.ok());
    }
    REQUIRE(steps.seen.size() == 7);
    // The cut points: around every step's end, and a few inside the longer steps.
    std::set<unsigned> points;
    for (const StepRecorder::Seen& s : steps.seen) {
      for (const int d : {-3, -1, 0, 1, 3}) {
        const int t = static_cast<int>(s.transfers) + d;
        if (t > 0) points.insert(static_cast<unsigned>(t));
      }
    }
    for (std::size_t i = 1; i < steps.seen.size(); ++i) {
      const unsigned a = steps.seen[i - 1].transfers;
      const unsigned b = steps.seen[i].transfers;
      if (b > a + 8) {
        points.insert(a + (b - a) / 4);
        points.insert(a + (b - a) / 2);
        points.insert(a + 3 * (b - a) / 4);
      }
    }
    unsigned cuts = 0;
    unsigned ended_at[prov::kStepCount] = {};
    // Up to the end of the verify a cut must stop the run; inside the reset step the base is on
    // the chip and verified whatever the look after the reset managed to say (a cut in the
    // session's closing transfers, which the sequence does not wait on, is a finished command).
    const unsigned verified = steps.seen[5].transfers;
    ProvUnit u(f);
    for (const unsigned at : points) {
      u.world.factory(f);
      u.chip().powerOn();
      u.reboot();
      // The target of this Walter: silent after `at` transfers, and the chip's power goes with it.
      u.v().target.silent_after_transfers = at;
      u.v().target.on_silenced = [&u] { u.chip().losePower(0); };
      const prov::Result r = u.run(rightName(f));
      if (at <= verified) {
        REQUIRE_MESSAGE(!r.ok(), std::string(bench::describe(f)) << ": the power went after " << at << " of " << verified
                                 << " transfers and the run did not notice: " << std::string(prov::describe(r.status)) << " at "
                                 << std::string(prov::describe(r.ended)));
      } else {
        CHECK(r.ok() == (r.status == Status::kOk));
        if (!r.ok()) CHECK(r.ended == Step::kReset);
        u.checkChipHoldsBase();
      }
      if (!r.ok()) ended_at[static_cast<std::size_t>(r.ended)]++;
      ++cuts;
      recoverAndFinish(u, std::string(bench::describe(f)) + ", the power cut after " + std::to_string(at) + " transfers");
    }
    MESSAGE(std::string(bench::describe(f)) << ": " << cuts << " power cuts at the debug port's transfers; the run ended in: attach "
                                << ended_at[2] << ", variant " << ended_at[3] << ", erase " << ended_at[4] << ", write "
                                << ended_at[5] << ", verify " << ended_at[6] << ", reset " << ended_at[7]);
  }
}

TEST_CASE("after a power cut `rak provision check` says what it sees: an improved part is closed again once something is in flash, a legacy one open") {
  // Improved, open, cut in the middle of the write: the MBR's first word is there, APPROTECT is
  // not 0x5A -> at the power-on the port closes again (PS: HwDisabled and the firmware are both
  // needed), and the line to type is `locked`, not the DEVICEID that opened it.
  {
    ProvUnit u(Factory::kImprovedOpen);
    u.chip().lose_power_at_op = 20;
    const prov::Result r = u.run(bench::kDeviceId);
    REQUIRE(!r.ok());
    REQUIRE(r.ended == Step::kWrite);
    u.powerCutBoth();
    CHECK(u.chip().approtect);
    const std::string check = u.runLine("rak provision check");
    CHECK(bench::hasLine(check, "protection: CLOSED"));
    CHECK(bench::hasLine(check, "link: no application answers HELLO"));
    CHECK(bench::nextLineOf(check) == "rak provision locked confirm");
    // And the DEVICEID that was right before the cut is refused now, with nothing written.
    const std::string refused = u.runLine("rak provision 0badf00ddeadbeef confirm");
    CHECK(bench::hasLine(refused, "error: rak provision: refused at step 2"));
    CHECK(bench::hasLine(refused, "nothing was erased"));
    CHECK(bench::hasLine(u.runLine("rak provision locked confirm"), "PROVISIONED"));
    u.checkChipHoldsBase();
  }
  // Legacy, locked by its firmware, cut after the erase: UICR is erased, so at the power-on the
  // port is open and the radio is named by its DEVICEID now.
  {
    ProvUnit u(Factory::kLegacyLocked);
    u.chip().lose_power_at_op = 1;
    const prov::Result r = u.run("locked");
    REQUIRE(!r.ok());
    REQUIRE(r.ended == Step::kErase);
    u.powerCutBoth();
    CHECK(!u.chip().approtect);
    const std::string check = u.runLine("rak provision check");
    CHECK(bench::hasLine(check, "protection: open"));
    CHECK(bench::nextLineOf(check) == std::string("rak provision ") + bench::kDeviceId + " confirm");
    CHECK(bench::hasLine(u.runLine("rak provision locked confirm"), "refused at step 2"));
    CHECK(bench::hasLine(u.runLine(bench::nextLineOf(check).c_str()), "PROVISIONED"));
    u.checkChipHoldsBase();
  }
}
