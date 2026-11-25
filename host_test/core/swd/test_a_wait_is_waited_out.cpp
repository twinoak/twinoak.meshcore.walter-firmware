// **A WAIT is waited out** (task p19-01).
//
// Since Walter 0.1.11 the scanner flash of every NoiseScope session on the unit at Bellevue failed
// at its first try, eight sessions of eight, the same way wherever the step was printed: *scan: swd
// wait-timeout, nothing written*, at `0x036000` after 32 to 33 s four times and at three other
// places and moments. The flash back over a running scanner has never failed, and neither did the
// radio's update of 23 November, whose flash began minutes into its wake. A session's flash begins
// one second after the wake has set the radio's clock - and the radio writes its clock's file at
// the first clock set after a boot.
//
// So the reading is that the scan, which reads the application's pages **while the application
// runs**, meets a page erase of the radio's own. A WAIT is the chip saying it cannot take the
// access now (ADIv5 B4.2.3); a page erase holds the flash for tERASEPAGE, 85 ms (nRF52840 Product
// Specification v1.11, 4.3.10.1); and the debug port gave up after ten tries back to back, a few
// milliseconds. This file holds what the port does now:
//
//   a WAIT shorter than the bound is waited out, the page read, and the flash goes on - and the
//   counters say how long it waited, where, and how many transfers waited at all;
//   a WAIT longer than the bound ends exactly as it ended before: `wait-timeout`, nothing written,
//   and the second try of task p18-05 above it;
//   a WAIT after the halt is waited out too, because the port is one port;
//   **no target and a FAULT are answered at once**, as they always were: nothing waits for them.
//
// The bound is `core::swd::kWaitBoundMs` and the loop's own constant bound is `kWaitPauses`; the
// test of the worst case in clocks is in `test_swd_dp.cpp`, where it has always been.
#include <doctest.h>

#include <string>

#include "core/flasher/flasher_bench.h"
#include "core/swd/dp.h"
#include "sim/swd_bench.h"

using sim::digestOf;
using sim::fl::Image;
using sim::fl::MemoryImage;
using sim::fl::Outcome;
using sim::fl::Result;
using sim::fl::Step;
namespace swd = core::swd;

namespace {

struct Bench : sim::FlasherBench {
  std::vector<std::uint8_t> image = sim::makeImage(2, false);
  std::vector<std::uint8_t> sha = digestOf(image);
  MemoryImage source{image.data(), static_cast<std::uint32_t>(image.size())};

  Image whole() {
    allow(0x26000, static_cast<std::uint32_t>(image.size()));
    return Image{&source, 0x26000, sha.data()};
  }
};

// tERASEPAGE, as the Product Specification gives it: the figure the bound was worked out from.
constexpr std::uint32_t kErasePageMs = 85;

}  // namespace

// ------------------------------------------------- the operation the scan meets, waited out

TEST_CASE("a page the application is erasing under the scan: the WAIT is waited out, the page read, and the flash goes on") {
  Bench b;
  // The first page the scan reads is busy with an operation of the application's own - 86 ms, one
  // page erase and a shade - and the chip answers WAIT to every access to it until it is over.
  b.chip.stallPageFor(0x26000, kErasePageMs + 1);
  const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
  REQUIRE(r.ok());
  CHECK(r.step == Step::kDone);
  CHECK_FALSE(r.wrote_nothing);

  // **One transfer waited**, in the scan, at the page the application was erasing - and it waited
  // the operation out rather than a tenth of it.
  CHECK(r.counters.waits == 1);
  CHECK(r.counters.wait_longest_step == Step::kScan);
  CHECK(r.counters.wait_longest_at_known);
  CHECK(r.counters.wait_longest_at == 0x26000);
  const bool long_enough = r.counters.wait_longest_ms >= kErasePageMs + 1;
  const bool not_too_long = r.counters.wait_longest_ms <= kErasePageMs + 1 + 2 * swd::kIdlePauseMs;
  CHECK(long_enough);
  CHECK(not_too_long);
  // And it is inside the bound, which is what made it survivable.
  CHECK(r.counters.wait_longest_ms < swd::kWaitBoundMs);
  MESSAGE("the scan met a page erase of the radio's own and waited " << r.counters.wait_longest_ms
                                                                    << " ms out");
}

TEST_CASE("the same operation after the halt: waited out there too, and the flash still finishes") {
  Bench b;
  // The bootloader's settings page, which is read **after** the core is halted and never before
  // it: the one place a test can put an operation the firmware meets with the application stopped.
  b.chip.stallPageFor(b.profile.settings_page, kErasePageMs + 1, /*while_running=*/false);
  const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
  REQUIRE(r.ok());
  CHECK(r.counters.waits >= 1);
  CHECK(r.counters.wait_longest_at == b.profile.settings_page);
  const bool after_the_halt = r.counters.wait_longest_step == Step::kSettingsInvalidate ||
                              r.counters.wait_longest_step == Step::kSettingsValidate;
  CHECK(after_the_halt);
  CHECK(r.counters.wait_longest_ms >= kErasePageMs + 1);
}

TEST_CASE("a flash that met nothing: the counters are nought, and the port waited for nothing") {
  Bench b;
  const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
  REQUIRE(r.ok());
  CHECK(r.counters.waits == 0);
  CHECK(r.counters.wait_longest_ms == 0);
  CHECK(r.counters.wait_longest_step == Step::kNone);
  CHECK_FALSE(r.counters.wait_longest_at_known);
}

// ------------------------------------------------------------- one that outlasts the bound

TEST_CASE("an operation that outlasts the bound: wait-timeout, nothing written, and the time it waited in the counters") {
  Bench b;
  // Ten times the bound: whatever this is, it is not one flash operation, and the flasher ends as
  // it ended before this task - the step, the status, the address, and nothing written.
  b.chip.stallPageFor(0x26000, 10 * swd::kWaitBoundMs);
  const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
  REQUIRE_FALSE(r.ok());
  CHECK(r.outcome == Outcome::kFailed);
  CHECK(r.step == Step::kScan);
  CHECK(r.nrf.status == core::nrf52::Status::kSwd);
  CHECK(r.nrf.swd == swd::Status::kWaitTimeout);
  CHECK(r.address == 0x26000);
  CHECK(r.wrote_nothing);
  CHECK(sim::nothingWritten(b.chip));
  // **And how long it waited before it gave up**, which is the bound and not a tenth of it.
  CHECK(r.counters.waits == 1);
  CHECK(r.counters.wait_longest_step == Step::kScan);
  CHECK(r.counters.wait_longest_ms >= swd::kWaitBoundMs);
  CHECK(r.counters.wait_longest_at == 0x26000);
  MESSAGE("it gave up after " << r.counters.wait_longest_ms << " ms (the bound is "
                              << swd::kWaitBoundMs << " ms)");
}

TEST_CASE("the whole of one connection's waiting is bounded too: the budget, and then no wait at all") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == core::board::Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  // Every transfer answers WAIT for ever. Each one costs the bound, and the budget is what stops
  // a chip like that from turning a flash into an afternoon: after it, a WAIT is an error at once.
  // A SELECT write and not a CTRL/STAT read: the three requests of B4.2.3 never WAIT, and the
  // simulator holds the specification to that.
  b.target.wait_next = 1000000;
  const unsigned most = swd::kWaitBudgetMs / swd::kWaitBoundMs;  // 60
  for (unsigned i = 0; i < most + 4; ++i) {
    const swd::Result r = b.dp.writeDp(core::swd::dp::kSelect, 0x05000000);
    CHECK(r.status == swd::Status::kWaitTimeout);
  }
  // Spent, and not run away with: a transfer may overshoot its own bound by one pause, because
  // the pause is what the clock is read after.
  CHECK(b.dp.waits().total_ms >= swd::kWaitBudgetMs);
  CHECK(b.dp.waits().total_ms < swd::kWaitBudgetMs + swd::kWaitBoundMs);
  CHECK(b.dp.waits().gave_up);
  // The last transfers waited for nothing at all: the budget was spent.
  const std::uint32_t transfers_before = b.dp.counters().transfers;
  CHECK(b.dp.writeDp(core::swd::dp::kSelect, 0x05000000).status == swd::Status::kWaitTimeout);
  CHECK(b.dp.counters().transfers - transfers_before == swd::kWaitRetries + 1);
  MESSAGE("one connection waited " << b.dp.waits().total_ms << " ms in all (the budget is "
                                   << swd::kWaitBudgetMs << " ms)");
  b.release();
}

TEST_CASE("a new connection is a new budget and a new record") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == core::board::Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.wait_next = 1000000;
  CHECK(b.dp.writeDp(core::swd::dp::kSelect, 0x05000000).status == swd::Status::kWaitTimeout);
  CHECK(b.dp.waits().transfers == 1);
  b.target.wait_next = 0;
  REQUIRE(b.dp.connect().ok());
  CHECK(b.dp.waits().transfers == 0);
  CHECK(b.dp.waits().total_ms == 0);
  CHECK_FALSE(b.dp.waits().gave_up);
  b.release();
}

// --------------------------------------------- what is not waited for: no target, and a FAULT

TEST_CASE("nothing answers: no target at once, with no wait of any kind") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == core::board::Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.silent = true;
  const std::uint32_t before = b.dp.counters().transfers;
  const swd::Result r = b.dp.writeDp(core::swd::dp::kSelect, 0x05000000);
  CHECK(r.status == swd::Status::kNoTarget);
  CHECK(b.dp.counters().transfers - before == 1);  // one try, and no retry of any kind
  CHECK(b.dp.waits().transfers == 0);
  CHECK(b.dp.waits().total_ms == 0);
  b.target.silent = false;
  b.release();
}

TEST_CASE("a sticky error: FAULT at once, read and cleared, and nothing waited") {
  sim::SwdBench b;
  sim::StubAp ap;
  b.target.attachAp(0, ap);
  REQUIRE(b.acquire() == core::board::Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.setStickyError();
  const swd::Result r = b.dp.readAp(0, 0x00);
  CHECK(r.status == swd::Status::kFault);
  CHECK(b.dp.counters().faults == 1);
  CHECK(b.dp.waits().transfers == 0);
  CHECK(b.dp.waits().total_ms == 0);
  b.release();
}
