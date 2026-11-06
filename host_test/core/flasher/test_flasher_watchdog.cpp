// The task watchdog, and the three long SWD jobs this firmware has (task p8-01).
//
// On 6 November 2026 the first `rak update --first-install` on a board never finished: a whole
// repeater image is minutes of SWD traffic and nothing on that path blocked, so the idle task of
// the core never ran and the 30 s task watchdog reset the Walter - twelve times, after which the
// supervisor ended the run as *stuck* (tasks/phase-6/FOLLOWUPS.md, *The SWD flasher starves the
// task watchdog*). Every host test passed throughout: the fake clock's delays were time passing
// and nothing else.
//
// So the fake clock now models the one thing about the board's delays that decides this
// (fakes/fake_clock.h): below one tick EspClock busy-waits, at a tick or more it blocks and the
// idle task runs. It keeps the longest stretch of simulated time with no blocking delay in it -
// what the watchdog counts - and the three cases below hold the three jobs to **two seconds**,
// fifteen times inside the watchdog's thirty:
//
//   1. a full repeater image flashed into a blank chip (417,284 bytes, 102 pages, 125 kHz);
//   2. the hash pass over the same image afterwards (Flasher::verify, which is also what the
//      supervisor's proof runs);
//   3. `rak provision`'s base written and verified over SWD.
//
// Without the pauses of core/swd/idle_pause.h all three are one unbroken stretch each: the whole
// flash, the whole hash pass, and the base's write and verify together.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/flasher/flasher.h"
#include "core/image/descriptor.h"
#include "core/provision/provision_bench.h"
#include "core/swd/idle_pause.h"
#include "flasher_bench.h"

using sim::digestOf;
using sim::fl::Image;
using sim::fl::MemoryImage;
using sim::fl::Result;

namespace {

// The image of 6 November: repeater v0.1.0, 417,284 bytes, which is 102 pages with the last one
// part full.
constexpr std::uint32_t kRepeaterImageBytes = 417284;
constexpr std::uint32_t kRepeaterImagePages = (kRepeaterImageBytes + 4095) / 4096;

// Fifteen times inside CONFIG_ESP_TASK_WDT_TIMEOUT_S = 30 (sdkconfig.defaults). A stretch longer
// than this is not yet a reset, and is already a design that depends on nothing else going slowly.
constexpr std::uint64_t kLongestStretchAllowedMs = 2000;

// A dense image of that size with a real §4 descriptor where a repeater's is - 0x400, the offset
// the release tooling uses - so that what the flasher is given is the shape of the file that
// failed and not only its length.
std::vector<std::uint8_t> repeaterImage() {
  std::vector<std::uint8_t> bytes = sim::makeImage(kRepeaterImagePages, true, 11);
  bytes.resize(kRepeaterImageBytes);
  const core::image::Built built =
      core::image::build(core::image::Role::kRepeater, 0, 1, 1, 0, "0.1.0+a1b2c3d", "rak4630-single", "868");
  REQUIRE(built.ok);
  for (std::size_t i = 0; i < core::image::kDescriptorSize; ++i) {
    bytes[0x400 + i] = built.descriptor.bytes[i];
  }
  const core::image::Found found = core::image::find(bytes.data(), bytes.size());
  REQUIRE(found.count == 1);
  REQUIRE(found.offset == 0x400);
  return bytes;
}

}  // namespace

TEST_CASE("a whole repeater image over SWD: no stretch of it starves the idle task") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = repeaterImage();
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));

  // ---- the flash ----
  b.clock.resetBusyStretch();
  const std::uint64_t t0 = b.clock.now_ms;
  Result r = b.flasher.flash(img, b.profile, nullptr);
  REQUIRE_MESSAGE(r.ok(), std::string(sim::fl::describe(r.step)));
  const std::uint64_t flash_ms = b.clock.now_ms - t0;
  const std::uint64_t flash_longest = b.clock.longestBusyStretchMs();
  const std::uint64_t flash_pauses = b.clock.blocking_delays;
  MESSAGE("flash: " << image.size() << " bytes, " << kRepeaterImagePages << " pages, " << flash_ms
                    << " ms simulated at 125 kHz; longest stretch with no blocking delay "
                    << flash_longest << " ms; " << flash_pauses << " blocking pauses, "
                    << (flash_pauses * core::swd::kIdlePauseMs) << " ms of them ("
                    << (flash_ms != 0 ? 100 * flash_pauses * core::swd::kIdlePauseMs / flash_ms : 0) << " %)");
  CHECK(b.chipHolds(image, 0x26000));
  const bool flash_inside = flash_longest <= kLongestStretchAllowedMs;
  CHECK_MESSAGE(flash_inside, "the flash ran " << flash_longest << " ms without letting the idle task run");

  // ---- the hash pass on its own: Flasher::verify, the supervisor's proof ----
  b.clock.resetBusyStretch();
  const std::uint64_t t1 = b.clock.now_ms;
  r = b.flasher.verify(img, b.profile);
  REQUIRE_MESSAGE(r.ok(), std::string(sim::fl::describe(r.step)));
  const std::uint64_t verify_ms = b.clock.now_ms - t1;
  const std::uint64_t verify_longest = b.clock.longestBusyStretchMs();
  MESSAGE("hash pass: " << verify_ms << " ms simulated; longest stretch " << verify_longest << " ms; "
                        << b.clock.blocking_delays << " blocking pauses");
  const bool verify_inside = verify_longest <= kLongestStretchAllowedMs;
  CHECK_MESSAGE(verify_inside, "the hash pass ran " << verify_longest << " ms without letting the idle task run");
}

TEST_CASE("`rak provision`'s base over SWD: no stretch of the write or the verify starves the idle task") {
  // base-resume is the largest base fixture a store holds whole: 48 KB of flash bytes against
  // the ~185 KB of a RAK's real base, so the stretches below are a base's shape and the report
  // extrapolates the totals from the per-word figures (as task p5-03's timing test does).
  bench::ProvUnit u(bench::Factory::kImprovedLocked, true, false);
  u.world.putBase("base-resume");
  u.clock().resetBusyStretch();
  const core::provision::Result r = u.run("locked");
  REQUIRE_MESSAGE(r.ok(), std::string(core::provision::describe(r.status)));
  const std::uint64_t longest = u.clock().longestBusyStretchMs();
  const std::uint32_t words = r.flash_words + r.uicr_words;
  MESSAGE("base: " << words << " words (" << r.base.flash_bytes << " flash bytes), write "
                   << r.step_ms[static_cast<std::size_t>(core::provision::Step::kWrite)] << " ms, verify "
                   << r.step_ms[static_cast<std::size_t>(core::provision::Step::kVerify)] << " ms, "
                   << r.total_ms << " ms in all; longest stretch with no blocking delay " << longest << " ms; "
                   << u.clock().blocking_delays << " blocking pauses");
  const bool inside = longest <= kLongestStretchAllowedMs;
  CHECK_MESSAGE(inside, "the base's write ran " << longest << " ms without letting the idle task run");
  u.checkChipHoldsBase();
}
