// The failure-injection sweep (task p3-06): the power-loss loops of four suites, run **together,
// end to end, on one world** - a Walter that loses power at every flash operation of a whole
// update, **download to commit**, and that must always end in one of three states:
//
//   1. **the old image running** - the golden image on the chip, a bootloader starting it;
//   2. **the new image running** - the candidate on the chip, promoted in the store;
//   3. **a journalled state the next wake resumes** - and then, when the next wake comes, 1 or 2.
//
// Nothing else is allowed: not half an application on the chip, not a store with no golden image,
// not a journal that cannot be read, not a flash operation outside the application's pages and
// the settings page. Each suite proves its own half already (host_test/core/image/
// test_store_power_loss.cpp, core/update/test_journal.cpp, core/flasher/test_flasher_power_loss.cpp,
// core/update/test_radio_update_power_loss.cpp); what no suite proved before this one is that the
// *handover* between them survives the same treatment - a download cut, resumed, then flashed; a
// flash cut, resumed from the journal, then committed.
//
// **The three flashes of one unit, and where the plug is pulled.** One update touches three
// flash devices, in this order:
//
//   the Walter's `rakfw` partition   the download's bytes and the store's records
//   the Walter's `journal` partition every state, written before its action
//   the radio's own flash            the pages the flasher erases and writes
//
// The plug is pulled at every write and erase of the first two, and at every erase and a sample
// of the word writes of the third: a page of flash is a thousand word writes, each injection
// costs a whole update through the bit-level SWD simulation, and task p1-05's own loop already
// interrupts every one of them exhaustively. `kChipWriteStride` is that sample, and the first
// and last writes of the run are always taken, because the boundaries are where the interesting
// half-states are.
#include <doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/image/store_bench.h"
#include "core/net/client.h"
#include "core/net/download.h"
#include "core/update/fetch.h"
#include "core/update/update_bench.h"
#include "fakes/capture.h"
#include "sim/fake_http.h"

using bench::Candidate;
using bench::State;
using bench::Which;
using core::board::Slot;
using core::update::FetchResult;
using core::update::FetchVerdict;
namespace upd = core::update;

namespace {

// One in this many of the radio's word writes is interrupted (see the header comment).
constexpr std::uint32_t kChipWriteStride = 97;

// Where the power went, so that a failure says which loop found it.
enum class Where { kStoreFlash, kJournalFlash, kChipFlash };

const char* name(Where where) {
  switch (where) {
    case Where::kStoreFlash: return "the image store's flash (the download)";
    case Where::kJournalFlash: return "the journal's flash";
    case Where::kChipFlash: return "the radio's flash (the flasher)";
  }
  return "?";
}

// How a run ended, which must always be one of the three.
enum class Ended { kOldImage, kNewImage, kResumedThenSettled };

// The whole world of one update: the store's flash, the journal's flash, the simulated nRF52 and
// the radio over it (core/update/update_bench.h), plus the HTTPS the release comes over.
struct SweepRig {
  bench::UpdateWorld world;
  sim::FakeHttp http{world.clock};
  core::net::Client client{http, world.clock};
  core::net::Download download{client, world.clock};
  fakes::CaptureOut out;
  Candidate golden{"update-golden"};
  Candidate candidate{"update-candidate"};
  std::unique_ptr<upd::Fetch> fetch;
  std::unique_ptr<bench::Walter> walter;

  SweepRig() {
    // A unit as an update finds it: the golden image in the store and on the chip, running.
    REQUIRE(world.store.put(Which::kGolden, Slot{0}, golden) == core::image::StoreError::kNone);
    world.installOnChip(golden);
    world.radioBoots();
    // The release on the server: the manifest, its signature and the one file beside them.
    const std::string base = "/release";
    http.server.serve(base + "/manifest.json", candidate.manifest);
    http.server.serve(base + "/manifest.json.sig",
                      std::vector<std::uint8_t>(candidate.signature.begin(),
                                                candidate.signature.end()));
    http.server.serve(base + "/" + fileNameOf(candidate), candidate.file, "\"sweep\"");
    build();
  }

  // The file's name in the manifest: the swd entry's, read out of the manifest text (the same
  // reading host_test/core/update/test_fetch.cpp does).
  static std::string fileNameOf(const Candidate& image) {
    const std::string text(image.manifest.begin(), image.manifest.end());
    const std::size_t use = text.find("\"use\": \"swd\"");
    REQUIRE(use != std::string::npos);
    const std::size_t at = text.find("\"name\": \"", use);
    REQUIRE(at != std::string::npos);
    const std::size_t start = at + 9;
    return text.substr(start, text.find('"', start) - start);
  }

  // A Walter over the world: fresh pins, debug port, flasher, journal, update machine - and a
  // fresh fetch, because the store has read its index again.
  void build() {
    walter.reset();
    walter = std::make_unique<bench::Walter>(world);
    fetch.reset(new upd::Fetch(client, download, world.store.store, world.store.verdict,
                               world.clock, &out));
  }

  // The power back and the Walter built again: what the next wake is. The new Walter attaches
  // the chip to its own debug port (core/update/update_bench.h).
  void nextWake() {
    world.chip.powerOn();
    build();
  }

  // The whole thing, in the order a wake does it: fetch the release into the candidate slot, then
  // run the radio's update over it.
  FetchResult downloadRelease() {
    return fetch->run("https://server.example/release", Which::kCandidate, Slot{0},
                      core::net::Budget{});
  }

  State updateRadio() {
    if (!walter->update.start(request())) return walter->update.state();
    return walter->run();
  }

  upd::Request request() const {
    upd::Request r;
    r.kind = bench::Kind::kUpdate;
    r.slot = Slot{0};
    return r;
  }

  bool chipRunsGolden() const { return world.chipHolds(golden); }
  bool chipRunsCandidate() const { return world.chipHolds(candidate); }
  bool bootloaderStarts() const {
    const bool stock = world.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kStock);
    const bool otafix = world.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kOtafix);
    return stock || otafix;
  }
};

// The clean run's flash operations, per device: how many injection points there are.
struct Clean {
  std::uint32_t store_ops = 0;
  std::uint32_t journal_ops = 0;
  std::uint32_t chip_erases = 0;
  std::uint32_t chip_writes = 0;
};

Clean cleanRun() {
  SweepRig rig;
  const FetchResult fetched = rig.downloadRelease();
  REQUIRE_MESSAGE(fetched.verdict == FetchVerdict::kStored,
                  std::string(upd::describe(fetched.verdict))
                      << ": " << std::string(fetched.why));
  const State end = rig.updateRadio();
  REQUIRE_MESSAGE(end == State::kDone, std::string(upd::describe(end))
                                           << "\n" << rig.walter->transitionText());
  REQUIRE(rig.chipRunsCandidate());
  Clean c;
  c.store_ops = rig.world.store.flash.operations();
  c.journal_ops = rig.world.journal_flash.operations();
  c.chip_erases = rig.world.chip.counters().erases;
  c.chip_writes = rig.world.chip.counters().writes;
  return c;
}

// What must hold after the recovery, whatever the cut did. Returns which of the three it was.
Ended checkSettled(SweepRig& rig, Where where, std::uint32_t op) {
  const bool golden_on_chip = rig.chipRunsGolden();
  const bool candidate_on_chip = rig.chipRunsCandidate();
  const bool one_whole_image = golden_on_chip || candidate_on_chip;
  CHECK_MESSAGE(one_whole_image, std::string(name(where)) << ", operation " << op
                << ": the chip holds neither image whole - half an application");
  if (rig.bootloaderStarts()) {
    CHECK_MESSAGE(one_whole_image, std::string(name(where)) << ", operation " << op
                  << ": a bootloader would start what is on the chip and it is not a whole image");
  }
  // The store always has a golden image to go back to, and it verifies against its own hash.
  CHECK_MESSAGE(rig.world.store.store.verify(Which::kGolden, Slot{0}) ==
                    core::image::StoreError::kNone,
                std::string(name(where)) << ", operation " << op
                    << ": the golden image no longer verifies");
  // Nothing forbidden was ever touched, and nothing outside the region (the chip fails the test
  // itself on either, and the counters say it again here).
  CHECK(rig.world.chip.counters().tripwire == 0);
  CHECK(rig.world.chip.counters().uicr_writes == 0);
  return candidate_on_chip ? Ended::kNewImage : Ended::kOldImage;
}

struct Totals {
  unsigned injections = 0;
  unsigned old_image = 0;
  unsigned new_image = 0;
  unsigned resumed = 0;       // the journal had a run to take up again
  unsigned refetched = 0;     // the download was resumed instead
  unsigned cut_before_any = 0;  // the plug was pulled before anything had been journalled
};

// One injection: cut the power at operation `op` of `where`, then let the next wake recover, and
// hold the outcome to the three.
void injectAt(Where where, std::uint32_t op, Totals* totals) {
  SweepRig rig;
  // The cut is the whole unit's power going: the two partitions stop answering, the debug port
  // goes dead, the radio's flash keeps whatever the operation left in it.
  auto die = [&rig] { rig.walter->die(); };
  switch (where) {
    case Where::kStoreFlash:
      rig.world.store.flash.lose_power_at_op = op;
      rig.world.store.flash.on_power_loss = die;
      break;
    case Where::kJournalFlash:
      rig.world.journal_flash.lose_power_at_op = op;
      rig.world.journal_flash.on_power_loss = die;
      break;
    case Where::kChipFlash:
      rig.world.chip.lose_power_at_op = op;
      rig.world.chip.on_power_loss = die;
      break;
  }

  // The wake that was interrupted: fetch, then update. Either may be where the plug is pulled.
  const FetchResult fetched = rig.downloadRelease();
  if (!rig.world.dead) (void)rig.updateRadio();
  REQUIRE_MESSAGE(rig.world.dead, std::string(name(where)) << ", operation " << op
                  << ": the power never went - the injection point is outside the run");
  ++totals->injections;

  // -- the next wake ---------------------------------------------------------------------------
  rig.world.store.flash.on_power_loss = nullptr;
  rig.world.journal_flash.on_power_loss = nullptr;
  rig.world.chip.on_power_loss = nullptr;
  rig.nextWake();
  rig.world.radioBoots();

  // What the journal says is what governs: a run it can take up again is taken up again, and
  // nothing else is started until it is finished (S-7).
  if (rig.walter->update.resume()) {
    ++totals->resumed;
    const State end = rig.walter->run();
    INFO(rig.walter->transitionText());
    const bool terminal = upd::isTerminal(end);
    CHECK_MESSAGE(terminal, std::string(name(where)) << ", operation " << op
                  << ": the resumed run did not reach a terminal state ("
                  << std::string(upd::describe(end)) << ")");
  } else if (fetched.verdict != FetchVerdict::kStored) {
    // The plug was pulled during the download: the bytes that reached flash stay, and the same
    // fetch continues from the progress record rather than starting over.
    ++totals->refetched;
    const FetchResult again = rig.downloadRelease();
    CHECK_MESSAGE(again.verdict == FetchVerdict::kStored,
                  std::string(name(where)) << ", operation " << op
                  << ": the resumed fetch answered " << std::string(upd::describe(again.verdict))
                  << " (" << std::string(again.why) << ")");
    if (again.verdict == FetchVerdict::kStored) {
      const State end = rig.updateRadio();
      INFO(rig.walter->transitionText());
      CHECK_MESSAGE(upd::isTerminal(end), std::string(name(where)) << ", operation " << op
                    << ": the update after a resumed fetch ended at "
                    << std::string(upd::describe(end)));
    }
  } else {
    // Stored, and nothing journalled: the cut came before the update wrote its first entry.
    ++totals->cut_before_any;
    const State end = rig.updateRadio();
    INFO(rig.walter->transitionText());
    CHECK(upd::isTerminal(end));
  }

  const Ended ended = checkSettled(rig, where, op);
  if (ended == Ended::kNewImage) ++totals->new_image;
  if (ended == Ended::kOldImage) ++totals->old_image;
}

}  // namespace

TEST_CASE("the sweep: a whole update, download to commit, with the power lost at every flash operation") {
  const Clean clean = cleanRun();
  REQUIRE(clean.store_ops > 2);
  REQUIRE(clean.journal_ops > 8);
  REQUIRE(clean.chip_erases >= 1);
  REQUIRE(clean.chip_writes > 100);
  MESSAGE("one clean update, download to commit: " << clean.store_ops
          << " operations on the image store's flash, " << clean.journal_ops
          << " on the journal's, " << clean.chip_erases << " erases and " << clean.chip_writes
          << " word writes on the radio's");

  Totals totals;
  for (std::uint32_t op = 1; op <= clean.store_ops; ++op) injectAt(Where::kStoreFlash, op, &totals);
  for (std::uint32_t op = 1; op <= clean.journal_ops; ++op) {
    injectAt(Where::kJournalFlash, op, &totals);
  }
  // The radio's flash: every erase, and one word write in kChipWriteStride, with the first and
  // the last always taken (the chip counts erases and word writes together, in order).
  const std::uint32_t chip_ops = clean.chip_erases + clean.chip_writes;
  unsigned chip_points = 0;
  for (std::uint32_t op = 1; op <= chip_ops; ++op) {
    const bool edge = op <= 2 || op + 2 > chip_ops;
    const bool sampled = op % kChipWriteStride == 0;
    const bool take = edge || sampled;
    if (!take) continue;
    ++chip_points;
    injectAt(Where::kChipFlash, op, &totals);
  }

  // Every injection ended in one of the three, and the three were all seen.
  CHECK(totals.injections == totals.old_image + totals.new_image);
  CHECK(totals.new_image > 0);
  CHECK(totals.resumed > 0);
  CHECK(totals.refetched > 0);
  MESSAGE("the sweep: " << totals.injections << " injection points ("
          << clean.store_ops << " on the image store's flash, " << clean.journal_ops
          << " on the journal's, " << chip_points << " of the radio's " << chip_ops
          << " flash operations, one in " << kChipWriteStride << " plus the edges); "
          << totals.new_image << " ended with the new image running, " << totals.old_image
          << " with the old one; " << totals.resumed
          << " were taken up again from the journal, " << totals.refetched
          << " resumed the download, " << totals.cut_before_any
          << " were cut before anything was journalled");
}
