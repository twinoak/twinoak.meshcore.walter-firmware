// The flash controller (task p1-04): a page erase and word writes with the simulator's log showing
// CONFIG set and restored and READY awaited; a write to a page that was not erased leaving the AND
// of old and new, caught by the read-back; READY late, READY never (bounded, an error, CONFIG
// restored); a FAULT in the middle (CONFIG restored); the bus stalling on a slow write (WAIT seen
// and retried); nWRITE; power lost at the Nth operation; and the simulator's own CONFIG check
// shown firing. Every test asserts the chip's log stayed inside the region.
#include <doctest.h>

#include <string>
#include <vector>

#include "sim/nrf52_bench.h"

using core::board::Outcome;
using core::nrf52::FlashRegion;
using core::nrf52::Result;
using core::nrf52::Status;
using sim::FlashEvent;
namespace regs = core::nrf52;

namespace {

const FlashRegion kApp{0x26000, 0xF0000};

// The log, as "kind kind kind" with CONFIG values, for a test to compare against the sentence it
// expects.
std::string shape(const std::vector<FlashEvent>& log) {
  std::string s;
  for (const FlashEvent& e : log) {
    if (!s.empty()) s += ' ';
    switch (e.kind) {
      case FlashEvent::Kind::kConfig: s += "CONFIG=" + std::to_string(e.value); break;
      case FlashEvent::Kind::kErasePage: s += "ERASE"; break;
      case FlashEvent::Kind::kWrite: s += "WRITE"; break;
      case FlashEvent::Kind::kRejectedWrite: s += "REJECTED-WRITE"; break;
      case FlashEvent::Kind::kRejectedErase: s += "REJECTED-ERASE"; break;
      case FlashEvent::Kind::kReadyRead: s += e.value ? "READY" : "busy"; break;
      case FlashEvent::Kind::kPowerLost: s += "POWER-LOST"; break;
    }
  }
  return s;
}

// Every CONFIG write in the log, in order.
std::vector<std::uint32_t> configs(const std::vector<FlashEvent>& log) {
  std::vector<std::uint32_t> v;
  for (const FlashEvent& e : log) {
    if (e.kind == FlashEvent::Kind::kConfig) v.push_back(e.value);
  }
  return v;
}

struct FlashBench : sim::Nrf52Bench {
  FlashBench() {
    chip.onlyTouch(kApp.begin, kApp.end);
    REQUIRE(open().ok());
  }
  ~FlashBench() {
    CHECK(chip.logInside(kApp.begin, kApp.end));
    // CONFIG is read-only again after every operation - on a chip that still has power. One
    // that lost it mid-operation keeps nothing; it comes back read-only (localReset).
    if (chip.powered()) CHECK(chip.config() == regs::nvmc::kConfigRen);
    CHECK(pins.violations.empty());
  }
};

}  // namespace

TEST_CASE("erase a page, write words, read them back: CONFIG set and restored, READY awaited") {
  FlashBench b;
  b.chip.fillFlash(0x26000, 0x27000, 0x00000000);  // a page full of zeros: only an erase can clear it
  const std::uint64_t t0 = b.clock.now_ms;
  Result r = b.nrf.erasePage(kApp, 0x26000);
  CHECK(r.ok());
  CHECK(r.address == 0x26000);
  CHECK(b.chip.counters().erases == 1);
  CHECK(b.chip.flashWord(0x26000) == 0xFFFFFFFF);
  CHECK(b.chip.flashWord(0x26FFC) == 0xFFFFFFFF);
  CHECK(b.chip.flashWord(0x27000) == 0xFFFFFFFF);  // untouched (it was blank)
  // CONFIG = Een, the erase, READY polled - busy for the erase's 85 ms, then ready - CONFIG = Ren.
  std::string s = shape(b.chip.log());
  CHECK(s.rfind("CONFIG=2 ERASE busy", 0) == 0);
  CHECK(s.find(" READY CONFIG=0") == s.size() - std::string(" READY CONFIG=0").size());
  CHECK(configs(b.chip.log()) == std::vector<std::uint32_t>{2, 0});
  // 85 ms of simulated time passed before READY: a millisecond's delay per poll plus the three
  // SWD transfers each poll costs (TAR, DRW, RDBUFF: about 1.3 ms at 125 kHz), so some forty
  // polls - the exact count is the wire's and not the test's.
  CHECK(b.clock.now_ms - t0 >= 85);
  CHECK(b.nrf.counters().ready_polls >= 30);
  CHECK(b.nrf.counters().ready_polls <= 90);
  b.chip.clearLog();

  const std::uint32_t data[6] = {0x20000000, 0x000260C1, 0xDEADBEEF, 0x00000000, 0xFFFFFFFF, 0x12345678};
  r = b.nrf.writeWords(kApp, 0x26000, data, 6);
  CHECK(r.ok());
  CHECK(r.address == 0x26000);
  CHECK(b.chip.counters().writes == 6);
  for (unsigned i = 0; i < 6; ++i) CHECK(b.chip.flashWord(0x26000 + 4 * i) == data[i]);
  CHECK(b.chip.flashWord(0x26018) == 0xFFFFFFFF);
  // CONFIG = Wen, six writes, READY (41 us have long passed at SWD speed), CONFIG = Ren.
  s = shape(b.chip.log());
  CHECK(s == "CONFIG=1 WRITE WRITE WRITE WRITE WRITE WRITE READY CONFIG=0");
  CHECK(b.chip.counters().rejected_writes == 0);
  CHECK(b.chip.counters().stalls == 0);

  std::uint32_t back[6] = {};
  r = b.nrf.read(0x26000, back, 6);
  CHECK(r.ok());
  for (unsigned i = 0; i < 6; ++i) CHECK(back[i] == data[i]);
  CHECK(b.nrf.counters().erases == 1);
  CHECK(b.nrf.counters().words_written == 6);
  CHECK(b.nrf.counters().config_writes == 4);
  CHECK(b.chip.errors().empty());
  CHECK(b.target.errors().empty());
  b.close();
}

TEST_CASE("a write to a page that was not erased leaves the AND of old and new, and the read-back catches it") {
  FlashBench b;
  b.chip.setFlashWord(0x27000, 0x12345678);
  const std::uint32_t data[1] = {0xFF00FF00};
  CHECK(b.nrf.writeWords(kApp, 0x27000, data, 1).ok());
  std::uint32_t back = 0;
  CHECK(b.nrf.read(0x27000, &back, 1).ok());
  CHECK(back == (0x12345678u & 0xFF00FF00u));
  CHECK(back == 0x12005600);
  CHECK(back != data[0]);  // what a flasher's verify sees: the word is not what was written
  CHECK(b.chip.counters().writes == 1);
  // Erased first, the same write lands whole.
  CHECK(b.nrf.erasePage(kApp, 0x27000).ok());
  CHECK(b.nrf.writeWords(kApp, 0x27000, data, 1).ok());
  CHECK(b.nrf.read(0x27000, &back, 1).ok());
  CHECK(back == data[0]);
  b.close();
}

TEST_CASE("READY late: inside the bound, more polls, still ok") {
  FlashBench b;
  b.chip.ready_extra_us = 150000;  // 85 + 150 = 235 ms, under the 300 ms bound
  const std::uint64_t t0 = b.clock.now_ms;
  const Result r = b.nrf.erasePage(kApp, 0x30000);
  CHECK(r.ok());
  CHECK(b.clock.now_ms - t0 >= 235);
  CHECK(b.nrf.counters().ready_polls >= 90);
  CHECK(b.nrf.counters().ready_polls <= 240);
  CHECK(configs(b.chip.log()) == std::vector<std::uint32_t>{2, 0});
  CHECK(b.chip.flashWord(0x30000) == 0xFFFFFFFF);
  b.close();
}

TEST_CASE("READY never: bounded, an error naming the register, CONFIG restored") {
  FlashBench b;
  b.chip.ready_never = true;
  const std::uint64_t t0 = b.clock.now_ms;
  Result r = b.nrf.erasePage(kApp, 0x30000);
  CHECK(r.status == Status::kNvmcTimeout);
  CHECK(r.address == regs::nvmc::kReady);
  CHECK(b.nrf.counters().ready_polls == core::nrf52::kErasePageTimeoutMs + 1);
  CHECK(b.clock.now_ms - t0 >= core::nrf52::kErasePageTimeoutMs);
  CHECK(b.clock.now_ms - t0 < 3 * core::nrf52::kErasePageTimeoutMs);  // the polls' SWD transfers take about as long again
  CHECK(configs(b.chip.log()) == std::vector<std::uint32_t>{2, 0});
  CHECK(b.chip.config() == regs::nvmc::kConfigRen);
  CHECK(b.nrf.counters().erases == 0);  // not counted as done
  b.close();

  // The same for a write, on a fresh chip (the one above is busy for ever now, and a flash write
  // to a busy controller stalls the bus - the next test). One word: a second would stall too.
  FlashBench c;
  c.chip.ready_never = true;
  const std::uint32_t data[1] = {1};
  r = c.nrf.writeWords(kApp, 0x31000, data, 1);
  CHECK(r.status == Status::kNvmcTimeout);
  CHECK(r.address == regs::nvmc::kReady);
  // The write went through before READY was awaited (the chip did it); the bound for a write is
  // the shorter one; CONFIG is read-only again.
  CHECK(c.chip.counters().writes == 1);
  CHECK(c.nrf.counters().ready_polls == core::nrf52::kWriteTimeoutMs + 1);
  CHECK(configs(c.chip.log()) == std::vector<std::uint32_t>{1, 0});
  CHECK(c.chip.config() == regs::nvmc::kConfigRen);
  CHECK(c.nrf.counters().words_written == 0);
  c.close();
}

TEST_CASE("a flash write while the controller is busy for ever: the bus stalls, WAIT times out, CONFIG restored") {
  FlashBench b;
  b.chip.ready_never = true;
  const std::uint32_t data[2] = {1, 2};
  const Result r = b.nrf.writeWords(kApp, 0x31000, data, 2);
  CHECK(r.status == Status::kSwd);
  CHECK(r.swd == core::swd::Status::kWaitTimeout);
  CHECK(r.address == 0x31004);  // the second word is the one that stalled
  CHECK(b.chip.counters().writes == 1);
  CHECK(b.chip.counters().stalls == core::swd::kWaitRetries + 1);
  CHECK(configs(b.chip.log()) == std::vector<std::uint32_t>{1, 0});
  CHECK(b.chip.config() == regs::nvmc::kConfigRen);
  b.close();
}

TEST_CASE("a FAULT in the middle of a write: the error, its address, and CONFIG restored") {
  FlashBench b;
  const std::uint32_t data[4] = {1, 2, 3, 4};
  // A sticky error appears after the chip is open: the first flash access FAULTs.
  b.target.setStickyError();
  Result r = b.nrf.writeWords(kApp, 0x32000, data, 4);
  CHECK(r.status == Status::kSwd);
  CHECK(r.swd == core::swd::Status::kFault);
  // The FAULT hit the CONFIG write (the first transfer after the flag): the restore write comes
  // after the debug port cleared the flag, and lands.
  CHECK(b.chip.config() == regs::nvmc::kConfigRen);
  CHECK(configs(b.chip.log()) == std::vector<std::uint32_t>{0});
  CHECK(b.chip.counters().writes == 0);
  // Cleared, the same call works.
  r = b.nrf.writeWords(kApp, 0x32000, data, 4);
  CHECK(r.ok());
  CHECK(b.chip.flashWord(0x3200C) == 4);
  b.close();
}

TEST_CASE("the lines lost in the middle of a write: the restore is attempted and refused, the status says lines lost") {
  FlashBench b;
  std::vector<std::uint32_t> data(16, 0x0F0F0F0F);
  // SWDIO is set to output for every bit the host drives (SwdLines::driveIo): a write transfer is
  // 8 request bits, 33 data bits and the idle run's first - 42. CONFIG costs two transfers, each
  // word one more; the 450th drive falls in the ninth word.
  unsigned drives = 0;
  b.pins.fail_when = [&](const fakes::Op& op) {
    if (op.gpio == core::board::kSwdIo.gpio && op.kind == fakes::OpKind::kMode &&
        op.mode == interfaces::PinMode::kOutput) {
      drives++;
    }
    return drives > 450;
  };
  const Result r = b.nrf.writeWords(kApp, 0x33000, data.data(), data.size());
  CHECK(r.status == Status::kSwd);
  CHECK(r.swd == core::swd::Status::kLinesLost);
  CHECK(r.address >= 0x33000);
  CHECK(r.address < 0x33040);
  b.pins.fail_when = nullptr;
  // CONFIG = Wen reached the chip before the lines went, and nothing could restore it: the chip
  // is left write-enabled until the next CONFIG write or a reset - which is why the flasher
  // (task p1-05) must treat a lost session as "the chip's state is unknown" and start over.
  CHECK(b.chip.config() == regs::nvmc::kConfigWen);
  CHECK(b.chip.counters().writes >= 1);
  CHECK(b.chip.counters().writes < 16);
  b.chip.clearLog();
  b.release();
  // A new session: open writes nothing; the first flash operation sets and restores CONFIG.
  REQUIRE(b.open().ok());
  CHECK(b.nrf.erasePage(kApp, 0x33000).ok());
  CHECK(b.chip.config() == regs::nvmc::kConfigRen);
  b.close();
}

TEST_CASE("a slow write stalls the bus: the access port answers WAIT, the debug port retries, every word lands") {
  FlashBench b;
  b.chip.write_time_us = 1000;  // twenty-five times tWRITE: longer than the transfer that follows it (432 us), shorter than the ten retries
  std::vector<std::uint32_t> data(8);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = 0xA0000000u + static_cast<std::uint32_t>(i);
  const Result r = b.nrf.writeWords(kApp, 0x34000, data.data(), data.size());
  CHECK(r.ok());
  CHECK(b.chip.counters().stalls > 0);
  CHECK(b.dp.counters().wait_retries > 0);
  CHECK(b.dp.counters().wait_timeouts == 0);
  for (std::size_t i = 0; i < data.size(); ++i) {
    CHECK(b.chip.flashWord(0x34000 + 4 * static_cast<std::uint32_t>(i)) == data[i]);
  }
  CHECK(b.target.errors().empty());
  b.close();
}

TEST_CASE("nWRITE: the same word written a third time since its erase is counted by the simulator") {
  FlashBench b;
  const std::uint32_t data[1] = {0xFFFFFFF0};
  CHECK(b.nrf.writeWords(kApp, 0x35000, data, 1).ok());
  CHECK(b.nrf.writeWords(kApp, 0x35000, data, 1).ok());
  CHECK(b.chip.counters().overwrites == 0);
  CHECK(b.nrf.writeWords(kApp, 0x35000, data, 1).ok());
  CHECK(b.chip.counters().overwrites == 1);
  CHECK(b.nrf.erasePage(kApp, 0x35000).ok());
  CHECK(b.nrf.writeWords(kApp, 0x35000, data, 1).ok());
  CHECK(b.chip.counters().overwrites == 1);
  b.close();
}

TEST_CASE("power lost at the Nth flash operation: a word half-written, the chip gone, the error says no target") {
  FlashBench b;
  b.chip.lose_power_at_op = 3;  // the erase is 1, the first word 2, the second word 3
  CHECK(b.nrf.erasePage(kApp, 0x36000).ok());
  const std::uint32_t data[4] = {0x00000000, 0x00000000, 0x00000000, 0x00000000};
  const Result r = b.nrf.writeWords(kApp, 0x36000, data, 4);
  CHECK(!r.ok());
  CHECK(r.status == Status::kSwd);
  CHECK(r.swd == core::swd::Status::kNoTarget);
  CHECK(!b.chip.powered());
  CHECK(b.chip.flashWord(0x36000) == 0x00000000);  // the first word landed
  CHECK(b.chip.flashWord(0x36004) == 0x0000FFFF);  // the second, half
  CHECK(b.chip.flashWord(0x36008) == 0xFFFFFFFF);  // the rest never came
  CHECK(b.chip.countLog(FlashEvent::Kind::kPowerLost) == 1);
  // The chip has lost power: the destructor's check of CONFIG does not apply (a powered-up chip
  // comes back read-only), so put it as a reset would.
  b.release();
}

TEST_CASE("power lost in the middle of an erase: half the page blank, the rest as it was") {
  FlashBench b;
  b.chip.fillFlash(0x37000, 0x38000, 0x5A5A5A5A);
  b.chip.lose_power_at_op = 1;
  const Result r = b.nrf.erasePage(kApp, 0x37000);
  CHECK(!r.ok());
  CHECK(!b.chip.powered());
  CHECK(b.chip.flashWord(0x37000) == 0xFFFFFFFF);
  CHECK(b.chip.flashWord(0x377FC) == 0xFFFFFFFF);
  CHECK(b.chip.flashWord(0x37800) == 0x5A5AFFFF);  // the word the power went on
  CHECK(b.chip.flashWord(0x37804) == 0x5A5A5A5A);
  CHECK(b.chip.flashWord(0x37FFC) == 0x5A5A5A5A);
  b.release();
}

TEST_CASE("the simulator's own CONFIG check: a flash write with write not enabled is rejected and reported") {
  FlashBench b;
  b.chip.strict = false;  // the sentence is what this test is about
  b.chip.setFlashWord(0x38000, 0xFFFFFFFF);
  // Straight through the memory access port, CONFIG still Ren: as a host that forgot would.
  CHECK(b.nrf.memAp().write32(0x38000, 0).ok());  // the bus accepted it
  CHECK(b.chip.flashWord(0x38000) == 0xFFFFFFFF);  // and nothing was written
  CHECK(b.chip.counters().rejected_writes == 1);
  CHECK(b.chip.countErrors("with NVMC.CONFIG.WEN = 0, not Wen (1)") == 1);
  MESSAGE("the simulator said: " << b.chip.errors()[0]);
  // And ERASEPAGE with CONFIG wrong.
  CHECK(b.nrf.memAp().write32(regs::nvmc::kErasePage, 0x38000).ok());
  CHECK(b.chip.counters().rejected_erases == 1);
  CHECK(b.chip.countErrors("not Een (2)") == 1);
  MESSAGE("the simulator said: " << b.chip.errors()[1]);
  b.chip.clearErrors();
  b.close();
}
