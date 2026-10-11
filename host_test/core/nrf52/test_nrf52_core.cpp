// Core control (task p1-04): halt, reset-and-halt, run, system reset - the simulated core's state
// follows, the debug key is on every write, the vector catch is set for the reset and cleared
// after, and a module that re-engages its protection at reset is reported as protected.
#include <doctest.h>

#include "sim/nrf52_bench.h"

using core::nrf52::CoreState;
using core::nrf52::Protection;
using core::nrf52::Result;
using core::nrf52::Status;
namespace regs = core::nrf52;

namespace {
struct CoreBench : sim::Nrf52Bench {
  CoreBench() { REQUIRE(open().ok()); }
  CoreState state() {
    CoreState s = CoreState::kUnknown;
    CHECK(nrf.coreState(&s).ok());
    return s;
  }
};
}  // namespace

TEST_CASE("halt: the core stops, S_HALT reads 1, the state is halted") {
  CoreBench b;
  CHECK(b.state() == CoreState::kRunning);
  const Result r = b.nrf.halt();
  CHECK(r.ok());
  CHECK((r.value & regs::scs::kSHalt) != 0);
  CHECK(b.chip.coreHalted());
  CHECK(b.chip.debugEnabled());
  CHECK(b.state() == CoreState::kHalted);
  CHECK(b.chip.counters().dhcsr_writes == 1);
  CHECK(b.chip.errors().empty());  // every DHCSR write carried the key
  CHECK(b.chip.counters().resets == 0);
  b.close();
}

TEST_CASE("run: the core leaves Debug state, halting debug is off, the vector catch is off") {
  CoreBench b;
  REQUIRE(b.nrf.halt().ok());
  const Result r = b.nrf.run();
  CHECK(r.ok());
  CHECK(!b.chip.coreHalted());
  CHECK(!b.chip.debugEnabled());
  CHECK((b.chip.demcr() & regs::scs::kVcCoreReset) == 0);
  CHECK(b.state() == CoreState::kRunning);
  CHECK(b.chip.counters().resets == 0);
  CHECK(b.chip.errors().empty());
  b.close();
}

TEST_CASE("reset-and-halt: one reset, the core halted at its reset vector, the vector catch cleared after") {
  CoreBench b;
  const Result r = b.nrf.resetAndHalt();
  CHECK(r.ok());
  CHECK(b.chip.counters().resets == 1);
  CHECK(b.chip.counters().aircr_resets == 1);
  CHECK(b.chip.coreHalted());
  CHECK(b.chip.debugEnabled());
  CHECK((b.chip.demcr() & regs::scs::kVcCoreReset) == 0);
  CHECK(b.state() == CoreState::kHalted);
  CHECK(b.nrf.protection() == Protection::kOpen);
  CHECK(b.chip.config() == regs::nvmc::kConfigRen);
  CHECK(b.chip.errors().empty());
  // run() then lets it go.
  CHECK(b.nrf.run().ok());
  CHECK(b.state() == CoreState::kRunning);
  b.close();
}

TEST_CASE("system reset: one reset, the core running afterwards, debug off") {
  CoreBench b;
  REQUIRE(b.nrf.halt().ok());
  const Result r = b.nrf.systemReset();
  CHECK(r.ok());
  CHECK(b.chip.counters().resets == 1);
  CHECK(!b.chip.coreHalted());
  CHECK(!b.chip.debugEnabled());
  CHECK(b.state() == CoreState::kRunning);
  CHECK(b.nrf.protection() == Protection::kOpen);
  CHECK(b.chip.errors().empty());
  b.close();
}

TEST_CASE("a module that closes its port at reset (build code Fxx, opened by firmware at run time): reset-and-halt says protected") {
  CoreBench b;
  b.chip.protects_after_reset = true;
  const Result r = b.nrf.resetAndHalt();
  CHECK(r.status == Status::kProtected);
  CHECK(b.nrf.protection() == Protection::kProtected);
  CHECK(b.chip.counters().resets == 1);
  // From here every operation is refused without a memory access.
  const unsigned refused_before = b.chip.counters().protected_accesses;
  CHECK(b.nrf.halt().status == Status::kProtected);
  CHECK(b.nrf.run().status == Status::kProtected);
  std::uint32_t word = 0;
  CHECK(b.nrf.read(0x26000, &word, 1).status == Status::kProtected);
  CHECK(b.chip.counters().protected_accesses == refused_before);
  b.close();
}

TEST_CASE("a system reset on the same module: the reset happens, then protected") {
  CoreBench b;
  b.chip.protects_after_reset = true;
  const Result r = b.nrf.systemReset();
  CHECK(r.status == Status::kProtected);
  CHECK(b.chip.counters().resets == 1);
  CHECK(!b.chip.coreHalted());
  b.close();
}

TEST_CASE("the simulator holds a host to the Cortex-M4's MasterType rule: C_DEBUGEN needs a debug master") {
  CoreBench b;
  b.chip.strict = false;
  // A CSW without MasterType, as a host that copied the wrong value would write.
  REQUIRE(b.nrf.memAp().write32(0x20000000, 0).ok());  // the port works
  REQUIRE(b.dp.writeAp(0, core::swd::mem_ap::kCsw, core::swd::mem_ap::kCswAddrIncSingle | core::swd::mem_ap::kCswSizeWord).ok());
  const Result r = b.nrf.halt();
  CHECK(r.status == Status::kCoreTimeout);
  CHECK(!b.chip.coreHalted());
  CHECK(b.chip.countErrors("CSW.MasterType = 0") >= 1);
  MESSAGE("the simulator said: " << b.chip.errors()[0]);
  b.chip.clearErrors();
  b.close();
}

TEST_CASE("the simulator refuses a debug register write without its key") {
  CoreBench b;
  b.chip.strict = false;
  REQUIRE(b.nrf.memAp().write32(regs::scs::kDhcsr, regs::scs::kCDebugEn | regs::scs::kCHalt).ok());
  CHECK(!b.chip.coreHalted());
  CHECK(b.chip.countErrors("without the debug key") == 1);
  REQUIRE(b.nrf.memAp().write32(regs::scs::kAircr, regs::scs::kSysResetReq).ok());
  CHECK(b.chip.counters().resets == 0);
  CHECK(b.chip.countErrors("without the key 0x05FA") == 1);
  b.chip.clearErrors();
  b.close();
}

TEST_CASE("coreState sees a core the chip holds in reset through the control port as not halted") {
  CoreBench b;
  // Not an operation this firmware has; a test of the simulator's CTRL-AP RESET.
  REQUIRE(b.dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kReset, 1).ok());
  CHECK(b.chip.heldInReset());
  REQUIRE(b.dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kReset, 0).ok());
  CHECK(!b.chip.heldInReset());
  CHECK(b.chip.counters().resets == 1);
  CHECK(b.state() == CoreState::kRunning);
  b.close();
}
