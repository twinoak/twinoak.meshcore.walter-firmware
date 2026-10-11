// Identification and access-port protection (task p1-04): the simulated nRF52840 identified from
// FICR with nothing else touched; a different part or page size reported and refused by everything
// below; a protected chip reported, with no memory access attempted and every operation answering
// "protected"; the control and memory access ports checked; the session closed.
#include <doctest.h>

#include <string>

#include "sim/nrf52_bench.h"

using core::board::Outcome;
using core::nrf52::CoreState;
using core::nrf52::FlashRegion;
using core::nrf52::Protection;
using core::nrf52::Result;
using core::nrf52::Status;
namespace regs = core::nrf52;

namespace {
const FlashRegion kApp{0x26000, 0xF0000};
}

TEST_CASE("identify the simulated chip: part, variant, flash, device id; protection open; nothing touched") {
  sim::Nrf52Bench b;
  const Result r = b.open();
  CHECK(r.ok());
  CHECK(b.nrf.isOpen());
  CHECK(b.nrf.protection() == Protection::kOpen);
  CHECK(b.nrf.identified());
  const core::nrf52::Identity& id = b.nrf.identity();
  CHECK(id.part == regs::ficr::kPartNrf52840);
  CHECK(id.variant_raw == 0x41414630);
  CHECK(std::string(id.variant) == "AAF0");
  CHECK(id.page_size == 4096);
  CHECK(id.pages == 256);
  CHECK(id.flash_bytes == 1024 * 1024);
  CHECK(id.info_flash_kb == 1024);
  CHECK(id.device_id[0] == 0xDEADBEEF);
  CHECK(id.device_id[1] == 0x0BADF00D);
  CHECK(id.is_nrf52840);
  CHECK(b.nrf.ctrlApIdr() == regs::ctrl_ap::kIdrValue);
  CHECK(b.nrf.memAp().opened());
  CHECK(b.nrf.memAp().idr() == core::swd::mem_ap::kCortexM4AhbApIdr);
  // The chip was read and nothing more: no flash controller access, no debug register written,
  // the core still running.
  CHECK(b.chip.log().empty());
  CHECK(b.chip.counters().dhcsr_writes == 0);
  CHECK(b.chip.counters().config_writes == 0);
  CHECK(!b.chip.coreHalted());
  CHECK(b.chip.errors().empty());
  CHECK(b.target.errors().empty());
  CHECK(b.pins.violations.empty());
  // And the power-up is requested for the session, withdrawn at close.
  CHECK((b.target.ctrlStat() & core::swd::ctrl_stat::kPowerUpAcks) == core::swd::ctrl_stat::kPowerUpAcks);
  CHECK(b.nrf.close().ok());
  CHECK((b.target.ctrlStat() & core::swd::ctrl_stat::kPowerUpRequests) == 0);
  CHECK(!b.nrf.isOpen());
  b.release();
}

TEST_CASE("a different part is reported and refused by everything below") {
  sim::Nrf52Bench b;
  b.chip.info_part = 0x52833;
  b.chip.info_variant = 0x41414230;  // "AAB0"
  const Result r = b.open();
  CHECK(r.status == Status::kNotNrf52840);
  CHECK(b.nrf.isOpen());
  CHECK(!b.nrf.identified());
  CHECK(b.nrf.identity().part == 0x52833);  // reported
  CHECK(std::string(b.nrf.identity().variant) == "AAB0");
  CHECK(b.nrf.protection() == Protection::kOpen);
  std::uint32_t word = 0;
  CHECK(b.nrf.read(0x10000100, &word, 1).status == Status::kNotNrf52840);
  CHECK(b.nrf.erasePage(kApp, 0x26000).status == Status::kNotNrf52840);
  const std::uint32_t data[1] = {0};
  CHECK(b.nrf.writeWords(kApp, 0x26000, data, 1).status == Status::kNotNrf52840);
  CHECK(b.nrf.halt().status == Status::kNotNrf52840);
  CHECK(b.nrf.resetAndHalt().status == Status::kNotNrf52840);
  CHECK(b.nrf.run().status == Status::kNotNrf52840);
  CHECK(b.nrf.systemReset().status == Status::kNotNrf52840);
  CoreState state;
  CHECK(b.nrf.coreState(&state).status == Status::kNotNrf52840);
  CHECK(b.chip.log().empty());
  CHECK(b.chip.counters().dhcsr_writes == 0);
  b.close();
}

TEST_CASE("pages that are not 4096 bytes are refused, as is a page count that cannot be") {
  sim::Nrf52Bench b;
  b.chip.code_page_size = 1024;
  CHECK(b.open().status == Status::kNotNrf52840);
  CHECK(b.nrf.identity().page_size == 1024);
  CHECK(!b.nrf.identified());
  b.close();
  sim::Nrf52Bench c;
  c.chip.code_size = 0xFFFFFFFF;  // "Unspecified"
  CHECK(c.open().status == Status::kNotNrf52840);
  CHECK(c.nrf.identity().flash_bytes == 0);
  c.close();
}

TEST_CASE("a variant with bytes that are not text prints as question marks") {
  sim::Nrf52Bench b;
  b.chip.info_variant = 0xFFFFFFFF;  // "Unspecified"
  CHECK(b.open().ok());
  CHECK(std::string(b.nrf.identity().variant) == "????");
  b.close();
}

TEST_CASE("a protected chip: the debug port answers, protection is reported, and nothing below is attempted") {
  sim::Nrf52Bench b;
  b.chip.approtect = true;
  const Result r = b.open();
  CHECK(r.status == Status::kProtected);
  CHECK(b.nrf.isOpen());
  CHECK(b.nrf.protection() == Protection::kProtected);
  CHECK(b.dp.connected());
  CHECK(b.dp.dpidr() == sim::kNrf52840Dpidr);
  CHECK(b.nrf.ctrlApIdr() == regs::ctrl_ap::kIdrValue);
  CHECK(!b.nrf.memAp().opened());
  CHECK(!b.nrf.identified());
  // Not one memory access was tried: the chip counted none refused, and the only AP reads were
  // the control port's two.
  CHECK(b.chip.counters().protected_accesses == 0);
  CHECK(b.target.counters().ap_reads == 2);
  CHECK(b.target.counters().ap_writes == 0);
  // Every operation below says so, and still touches nothing.
  CHECK(b.nrf.identify().status == Status::kProtected);
  std::uint32_t word = 0;
  CHECK(b.nrf.read(0x10000100, &word, 1).status == Status::kProtected);
  CHECK(b.nrf.erasePage(kApp, 0x26000).status == Status::kProtected);
  const std::uint32_t data[1] = {0};
  CHECK(b.nrf.writeWords(kApp, 0x26000, data, 1).status == Status::kProtected);
  CHECK(b.nrf.halt().status == Status::kProtected);
  CHECK(b.nrf.resetAndHalt().status == Status::kProtected);
  CHECK(b.nrf.run().status == Status::kProtected);
  CHECK(b.nrf.systemReset().status == Status::kProtected);
  CoreState state;
  CHECK(b.nrf.coreState(&state).status == Status::kProtected);
  CHECK(b.chip.counters().protected_accesses == 0);
  CHECK(b.target.counters().ap_reads == 2);
  CHECK(b.chip.log().empty());
  CHECK(b.chip.counters().tripwire == 0);
  CHECK(std::string(core::nrf52::describe(Protection::kProtected)) == "PROTECTED");
  b.close();
}

TEST_CASE("before open() everything is refused with not-open") {
  sim::Nrf52Bench b;
  CHECK(b.nrf.identify().status == Status::kNotOpen);
  CHECK(b.nrf.halt().status == Status::kNotOpen);
  CHECK(b.nrf.erasePage(kApp, 0x26000).status == Status::kNotOpen);
  std::uint32_t word = 0;
  CHECK(b.nrf.read(0, &word, 1).status == Status::kNotOpen);
  CHECK(b.dp.counters().transfers == 0);
}

TEST_CASE("open() fails cleanly when the debug port does not answer, or the ports are not the chip's") {
  sim::Nrf52Bench silent;
  silent.target.silent = true;
  Result r = silent.open();
  CHECK(r.status == Status::kSwd);
  CHECK(r.swd == core::swd::Status::kNoTarget);
  CHECK(!silent.nrf.isOpen());
  silent.release();

  sim::Nrf52Bench ctrl;
  ctrl.chip.ctrl_ap_idr = 0x04770001;  // an Arm APB-AP where Nordic's CTRL-AP should be
  r = ctrl.open();
  CHECK(r.status == Status::kNoControlPort);
  CHECK(!ctrl.nrf.isOpen());
  CHECK(ctrl.nrf.protection() == Protection::kUnknown);
  ctrl.close();

  sim::Nrf52Bench mem;
  mem.chip.ahb_ap_idr = 0;
  r = mem.open();
  CHECK(r.status == Status::kNoMemoryPort);
  CHECK(mem.nrf.isOpen());
  CHECK(mem.nrf.protection() == Protection::kOpen);
  CHECK(!mem.nrf.identified());
  mem.close();

  sim::Nrf52Bench power;
  power.target.powers_up = false;
  r = power.open();
  CHECK(r.status == Status::kSwd);
  CHECK(r.swd == core::swd::Status::kPowerUpTimeout);
  power.release();
}

TEST_CASE("every status, protection and core state has words") {
  for (int i = 0; i <= static_cast<int>(Status::kCoreTimeout); ++i) {
    CHECK(std::string(core::nrf52::describe(static_cast<Status>(i))) != "?");
  }
  CHECK(std::string(core::nrf52::describe(Protection::kOpen)) == "open");
  CHECK(std::string(core::nrf52::describe(CoreState::kHalted)) == "halted");
  CHECK(std::string(core::nrf52::describe(CoreState::kRunning)) == "running");
  CHECK(std::string(core::swd::describe(core::swd::Status::kNoMemAp)) != "?");
}
