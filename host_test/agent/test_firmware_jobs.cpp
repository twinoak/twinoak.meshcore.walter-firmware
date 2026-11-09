// The four firmware jobs (task p4-05, walter_agent/firmware_jobs.h), on the wake bench with a
// simulated nRF52840 for the radio to run from, the image store, the two update machines and the
// scripted meshmaster.dk serving a release as it does: the offer in ranges with a drop and the
// budget ending the wake with no result, the next wake finishing it; a tampered manifest at check
// 1 and no image for the slot at check 3; the apply committed after the proving period, rolled
// back with the journal in the result and the candidate on the bad list, refused for a hash the
// server did not name, refused `inProgress` on trial; the Walter's own apply across a boot; the
// state against the store; the power cycle within its bounds and refused below the floor. Every
// result read through the fixtures' shapes.
#include <doctest.h>

#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "core/version.h"
#include "fixtures/coredump_fixture.h"
#include "fixtures/manifest_files.h"

using bench::AgentBench;
using bench::Candidate;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;
using core::image::Which;
namespace power = core::power;

namespace {

// A unit ready for an update: the golden image on the chip and in the store, running.
void readyUnit(AgentBench& bench, const Candidate& golden) {
  REQUIRE(bench.world.images.put(Which::kGolden, bench::Slot{0}, golden) == bench::StoreError::kNone);
  bench.world.chip->installOnChip(golden);
  bench.world.chip->radioBoots();
  bench.modemWorks();
}

std::vector<std::string> fixtureKeys(const char* name) { return bench::topKeys(bench::contractFixture(name)); }

}  // namespace

TEST_CASE("twinoak.fw.state: the store, the running image, the Walter's slots and the supervisor, in the fixture's shape") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  bench.server().queue(bench::ownJob(1, agent::kStateType, "{}").json());
  const Pass& pass = bench.oneWake();
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kDone);
  const std::string result = bench.lastResult(1);
  REQUIRE_FALSE(result.empty());
  CHECK(bench::member(result, "outcome") == "ok");
  const std::string data = bench::objectOf(result, "data");
  // The shape: the fixture's top-level keys, in its order.
  CHECK(bench::topKeys(data) == fixtureKeys("FirmwareStateResult"));
  const std::string slots = bench::objectOf(data, "slots");
  const std::string slot = slots.substr(1, slots.size() - 2);
  CHECK(bench::topKeys(slot) == std::vector<std::string>{"slot", "role", "board", "band", "running", "golden", "candidate"});
  CHECK(bench::member(slot, "role") == "repeater");
  CHECK(bench::member(slot, "board") == "rak4630-single");
  CHECK(bench::member(slot, "band") == "868");
  CHECK(bench::member(bench::objectOf(slot, "running"), "version") == golden.version);
  CHECK(bench::member(bench::objectOf(slot, "golden"), "version") == golden.version);
  CHECK(bench::member(bench::objectOf(slot, "golden"), "sha256") == bench::sha256Hex(golden.file));
  CHECK(bench::member(bench::objectOf(slot, "candidate"), "sha256") == bench::sha256Hex(candidate.file));
  const std::string walter = bench::objectOf(data, "walter");
  CHECK(bench::member(walter, "version") == "0.0.0-dev");
  CHECK(bench::member(walter, "runningSlot") == "ota_0");
  CHECK(bench::member(walter, "confirmed") == "true");
  CHECK(bench::member(bench::objectOf(data, "supervisor"), "incidents") == "0");
}

TEST_CASE("twinoak.fw.state with no golden image: HELLO's role, board and band, and no band from a 1.0 radio") {
  // 6 November, job 20990: with no golden image the state came back `{"slot": 0}` and nothing
  // else, so meshmaster.dk - which picks an image by role, board **and** band
  // (FirmwareRollout.PickImage) - could never offer that radio its first image over LTE
  // (phase-6/FOLLOWUPS.md). Link 1.1 ends HELLO with the band, and this is the slot reporting it
  // from a radio alone: the image runs on the chip, the store holds nothing.
  SUBCASE("a 1.1 radio says its band, and the slot reports all three") {
    AgentBench bench;
    const Candidate running("update-golden-link-1-1");
    bench.world.chip->installOnChip(running);
    bench.world.chip->radioBoots();
    bench.modemWorks();
    REQUIRE_FALSE(bench.world.images.read(Which::kGolden).present);
    bench.server().queue(bench::ownJob(1, agent::kStateType, "{}").json());
    const Pass& pass = bench.oneWake();
    CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kDone);
    const std::string slots = bench::objectOf(bench.lastData(1), "slots");
    const std::string slot = slots.substr(1, slots.size() - 2);
    CHECK(bench::topKeys(slot) == std::vector<std::string>{"slot", "role", "board", "band", "running"});
    CHECK(bench::member(slot, "role") == "repeater");
    CHECK(bench::member(slot, "board") == "rak4630-single");
    CHECK(bench::member(slot, "band") == running.band);
    CHECK(bench::member(bench::objectOf(slot, "running"), "version") == running.version);
  }

  SUBCASE("a 1.0 radio says none, and the slot reports the two it has") {
    AgentBench bench;
    const Candidate running("update-golden");  // a link 1.0 image: repeater v0.1.0's case
    bench.world.chip->installOnChip(running);
    bench.world.chip->radioBoots();
    bench.modemWorks();
    bench.server().queue(bench::ownJob(1, agent::kStateType, "{}").json());
    const Pass& pass = bench.oneWake();
    CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kDone);
    const std::string slots = bench::objectOf(bench.lastData(1), "slots");
    const std::string slot = slots.substr(1, slots.size() - 2);
    CHECK(bench::topKeys(slot) == std::vector<std::string>{"slot", "role", "board", "running"});
    CHECK(bench::member(slot, "role") == "repeater");
    CHECK(bench::member(slot, "board") == "rak4630-single");
    CHECK(bench::member(slot, "band").empty());
  }

  SUBCASE("a silent radio says nothing at all, as before") {
    AgentBench bench;
    bench.world.chip->radioBoots();  // a blank chip: nothing answers HELLO
    bench.modemWorks();
    bench.server().queue(bench::ownJob(1, agent::kStateType, "{}").json());
    const Pass& pass = bench.oneWake();
    CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kDone);
    const std::string slots = bench::objectOf(bench.lastData(1), "slots");
    const std::string slot = slots.substr(1, slots.size() - 2);
    CHECK(bench::topKeys(slot) == std::vector<std::string>{"slot"});
  }
}

TEST_CASE("twinoak.fw.offer: the budget ends the wake mid-download with no result, the next wake resumes and finishes") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate big("store-resume");  // 150 000 bytes: three of the download's 64 KB ranges
  readyUnit(bench, golden);
  bench::serveRelease(bench.world.http.server, big);

  // The download may fetch one range this wake: a byte cap, because the library prices a job by
  // transactions and a download is minutes, so the wake's time cannot stop a half-done one - it
  // would stop the library starting the offer at all (WakeContext::download_bytes).
  bench.half().backend.setDownloadBytesPerWake(core::net::kRangeBytes);
  bench.server().queue(bench::ownJob(7, agent::kOfferType, bench::offerPayload(big)).json());
  interfaces::BackendAnswer out;
  interfaces::Exchanged how = bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 600, &out);
  CHECK(how == interfaces::Exchanged::kAsked);
  // No result: the lease is left to expire (unit-protocol.md §5), the store holds a whole range.
  CHECK(bench.lastResult(7).empty());
  CHECK(bench.half().firmware.counters().offers_left_leased == 1);
  CHECK(bench.half().server.uplinkCounters().withheld == 1);
  bench::Record held = bench.world.images.read(Which::kCandidate);
  CHECK_FALSE(held.present);
  CHECK(held.transfer);
  CHECK(held.transferred == core::net::kRangeBytes);
  // The lease was extended before the first byte went.
  REQUIRE(bench.server().extends[7].size() >= 1);
  CHECK(out.did == interfaces::askBit(interfaces::Ask::kFetchRelease));
  CHECK(out.outside_ms > 0);

  // The next wake: the plan can pay for the rest, the server re-queues the expired lease under
  // the same id, and the extension recognises the file by the store's record and resumes from
  // 65536.
  bench.half().backend.setDownloadBytesPerWake(2u * 1024u * 1024u);
  bench.server().queue(bench::ownJob(7, agent::kOfferType, bench::offerPayload(big)).json());
  how = bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 600, &out);
  CHECK(how == interfaces::Exchanged::kAsked);
  const std::string file = std::string(bench::kBase) + "/files/" + bench::swdName(big.manifest);
  bool resumed_range = false;
  for (const sim::SeenRequest& r : bench.world.http.server.requests) {
    if (r.path == file && r.header("Range") == "bytes=65536-131071") resumed_range = true;
  }
  CHECK(resumed_range);
  const std::string result = bench.lastResult(7);
  REQUIRE_FALSE(result.empty());
  CHECK(bench::member(result, "outcome") == "ok");
  const std::string data = bench::objectOf(result, "data");
  CHECK(bench::topKeys(data) == std::vector<std::string>{"stored", "imageId", "sha256", "bytes", "resumed"});
  CHECK(bench::member(data, "stored") == "true");
  CHECK(bench::member(data, "imageId") == "repeater.rak4630-single.868");
  CHECK(bench::member(data, "sha256") == bench::sha256Hex(big.file));
  CHECK(bench::member(data, "bytes") == "150000");
  CHECK(bench::member(data, "resumed") == "true");
  held = bench.world.images.read(Which::kCandidate);
  CHECK(held.present);
  CHECK(bench.world.images.store.verify(Which::kCandidate, bench::Slot{0}) == bench::StoreError::kNone);
  CHECK(bench.server().extends[7].size() >= 2);
  CHECK(bench.server().progress[7].size() >= 1);
  // The token travelled in the Authorization header of every request to the release, and nowhere else.
  int in_authorization = 0;
  int elsewhere = 0;
  bench.server().tokenHeaders("mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV", &in_authorization, &elsewhere);
  CHECK(in_authorization > 0);
  CHECK(elsewhere == 0);
  CHECK_FALSE(bench.server().anyRequestCarries("mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV"));
}

TEST_CASE("twinoak.fw.offer stopped by the budget: the two sentences, one per product") {
  // Until task p10-04 both of them promised a resume for either box (tasks/phase-9/FOLLOWUPS.md).
  // A radio image's does resume - the image store keeps a progress record and the test above walks
  // the second wake finishing the file - while the Walter's other application slot is erased when a
  // write opens on it, so its download starts again from zero and the plan pays twice.
  //
  // Two things are read per product: the line a cable shows (the library's log, where
  // `AgentWorld::logLine` puts every line the copied library formatted) and the `reason` of the
  // result the extension wrote. The result of a budget stop is **withheld on purpose** - no result,
  // the lease is left to expire (unit-protocol.md §5) - so the gate is taken off here, which is the
  // only way to read the body; nothing else in the branch depends on it.
  SUBCASE("a radio image: the bytes stay, so both sentences still say it resumes") {
    AgentBench bench;
    const Candidate golden("update-golden");
    const Candidate big("store-resume");  // 150 000 bytes
    readyUnit(bench, golden);
    bench::serveRelease(bench.world.http.server, big);
    bench.boot();
    bench.v().begin();
    bench.half().server.setResultGate(nullptr);
    bench.half().backend.setDownloadBytesPerWake(4096);
    bench.server().queue(bench::ownJob(41, agent::kOfferType, bench::offerPayload(big)).json());
    interfaces::BackendAnswer out;
    CHECK(bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 600, &out) == interfaces::Exchanged::kAsked);
    CHECK(bench.half().firmware.counters().offers_left_leased == 1);
    const std::string& log = bench.world.agent->log;
    INFO(log);
    CHECK(log.find("the wake's budget ended the download at 4096 of 150000 bytes: no result, the lease is "
                   "left to expire and the next wake continues it") != std::string::npos);
    const std::string data = bench.lastData(41);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "reason") == "the wake's budget ended the download; the next wake resumes it");
  }

  SUBCASE("the Walter's own image: neither sentence promises a resume") {
    AgentBench bench;
    const Candidate golden("update-golden");
    const Candidate self("self-a");  // 12 288 bytes of the Walter's own product
    readyUnit(bench, golden);
    bench::serveRelease(bench.world.http.server, self, bench::kBase, nullptr, nullptr, nullptr, true);
    bench.boot();
    bench.v().begin();
    bench.half().server.setResultGate(nullptr);
    bench.half().backend.setDownloadBytesPerWake(4096);
    bench.server().queue(bench::ownJob(42, agent::kOfferType, bench::offerPayload(self, agent::kProductWalter)).json());
    interfaces::BackendAnswer out;
    CHECK(bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 600, &out) == interfaces::Exchanged::kAsked);
    CHECK(bench.half().firmware.counters().offers_left_leased == 1);
    const std::string& log = bench.world.agent->log;
    INFO(log);
    CHECK(log.find("the wake's budget ended the download at 4096 of 12288 bytes: no result, the lease is "
                   "left to expire and the next wake starts the file again from zero") != std::string::npos);
    CHECK(log.find("the next wake continues it") == std::string::npos);
    const std::string data = bench.lastData(42);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "reason") ==
          "the wake's budget ended the download; this slot keeps no progress, so the next wake starts the "
          "file again from zero");
    // And nothing of the file is left to resume into: the slot holds no image and no open write.
    core::update::SelfPayload none;
    CHECK_FALSE(bench.v().self.received(&none));
  }
}

TEST_CASE("twinoak.fw.offer refused: a tampered manifest at check 1, no image for the slot at check 3, nothing stored") {
  AgentBench bench;
  const Candidate golden("update-golden");
  readyUnit(bench, golden);
  SUBCASE("the specification's tampered manifest with the good signature") {
    const Candidate image("store-a");
    const std::vector<std::uint8_t> tampered = fixtures::readSpec("manifest-repeater-tampered.json");
    const std::string sig = fixtures::asText(fixtures::readSpec("manifest-repeater.json.sig"));
    bench::serveRelease(bench.world.http.server, image, bench::kBase, &tampered, &sig);
    bench.server().queue(bench::ownJob(8, agent::kOfferType, bench::offerPayload(image)).json());
    (void)bench.oneWake();
    const std::string result = bench.lastResult(8);
    REQUIRE_FALSE(result.empty());
    CHECK(bench::member(result, "outcome") == "failed");
    const std::string data = bench::objectOf(result, "data");
    CHECK(bench::member(data, "stored") == "false");
    CHECK(bench::member(data, "check") == "1");
    CHECK_FALSE(bench::member(data, "reason").empty());
    CHECK_FALSE(bench.world.images.read(Which::kCandidate).present);
    CHECK_FALSE(bench.world.images.read(Which::kCandidate).transfer);
    // The image was never asked for: two requests to the release and no more.
    CHECK(bench.server().requestsTo(std::string(bench::kBase) + "/files/" + bench::swdName(image.manifest)) == 0);
  }
  SUBCASE("a release whose only image is a scanner's: no image for the candidate slot") {
    const Candidate scanner("store-scanner");
    bench::serveRelease(bench.world.http.server, scanner);
    bench.server().queue(bench::ownJob(9, agent::kOfferType, bench::offerPayload(scanner)).json());
    (void)bench.oneWake();
    const std::string data = bench::objectOf(bench.lastResult(9), "data");
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "stored") == "false");
    CHECK(bench::member(data, "check") == "3");
    CHECK_FALSE(bench.world.images.read(Which::kCandidate).present);
  }
}

TEST_CASE("twinoak.fw.apply: committed after a scripted good candidate, the proving period passing, the lease extended") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  bench.server().queue(bench::ownJob(11, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
  const std::uint64_t before = bench.world.clock.nowMs();
  const Pass& pass = bench.oneWake();
  CHECK(pass.carried == interfaces::Ask::kUpdateRadio);
  CHECK(pass.outcomeOf(Step::kUpdate) == Outcome::kNothing);  // S-7: step 9 had nothing left to do
  CHECK(pass.outside_ms >= core::update::kProvingPeriodMs);
  CHECK(bench.world.clock.nowMs() - before >= core::update::kProvingPeriodMs);
  const std::string result = bench.lastResult(11);
  REQUIRE_FALSE(result.empty());
  CHECK(bench::member(result, "outcome") == "ok");
  const std::string data = bench::objectOf(result, "data");
  CHECK(bench::topKeys(data) == std::vector<std::string>{"outcome", "fromVersion", "toVersion", "journal"});
  CHECK(bench::member(data, "outcome") == "committed");
  CHECK(bench::member(data, "fromVersion") == golden.version);
  CHECK(bench::member(data, "toVersion") == candidate.version);
  const std::string journal = bench::objectOf(data, "journal");
  for (const char* step : {"\"step\":\"check\"", "\"step\":\"flashing-candidate\"", "\"step\":\"booting\"",
                           "\"step\":\"proving\"", "\"step\":\"committing\"", "\"step\":\"done\""}) {
    CHECK_MESSAGE(journal.find(step) != std::string::npos, step);
  }
  CHECK(bench::topKeys(bench::objectOf(journal.substr(1), "at").empty() ? journal.substr(1, journal.find('}')) : "{}")
            .size() >= 0);
  CHECK(bench.server().extends[11].size() >= 1);
  // The store: the candidate is golden, the chip holds it, the radio runs it.
  CHECK(std::string(bench.world.images.read(Which::kGolden).version) == candidate.version);
  CHECK_FALSE(bench.world.images.read(Which::kCandidate).present);
  CHECK(bench.world.chip->chipHolds(candidate));
  CHECK(bench.world.chip->running_version == candidate.version);
  CHECK(bench.half().firmware.counters().committed == 1);
  // And the slot's last update, as `state` reports it afterwards.
  agent::JobEntry last;
  REQUIRE(bench.half().firmware.lastEnded(&last));
  CHECK(last.outcome == agent::ApplyOutcome::kCommitted);
  CHECK(std::string(last.to_version) == "0.3.1");
}

TEST_CASE("twinoak.fw.apply: rolled back with the journal in the result, the candidate on the bad list, a second apply refused") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  // The candidate boots and says the old version: the health check's first step fails.
  bench.world.chip->boot = bench::Boot::kOldVersion;
  bench.world.chip->old_version = golden.version;
  const std::string sha = bench::sha256Hex(candidate.file);
  bench.server().queue(bench::ownJob(12, agent::kApplyType, bench::applyPayload(sha)).json());
  (void)bench.oneWake();
  const std::string data = bench::objectOf(bench.lastResult(12), "data");
  REQUIRE_FALSE(data.empty());
  CHECK(bench::member(bench.lastResult(12), "outcome") == "ok");  // a rollback is the system working
  CHECK(bench::member(data, "outcome") == "rolledBack");
  CHECK(bench::member(data, "fromVersion") == golden.version);
  CHECK(bench::member(data, "toVersion") == golden.version);
  const std::string journal = bench::objectOf(data, "journal");
  CHECK(journal.find("\"step\":\"rolling-back\"") != std::string::npos);
  CHECK(journal.find("\"step\":\"restored\"") != std::string::npos);
  CHECK(bench.world.images.store.badCount() == 1);
  CHECK(std::string(bench.world.images.read(Which::kGolden).version) == golden.version);
  CHECK(bench.world.chip->chipHolds(golden));

  // The same hash again: refused with the bad list's word, nothing touched.
  bench.world.chip->boot = bench::Boot::kNormal;
  bench.server().queue(bench::ownJob(13, agent::kApplyType, bench::applyPayload(sha)).json());
  const unsigned flashes = bench.v().pins.kill_cycles + bench.v().pins.reset_pulses;
  (void)bench.oneWake();
  const std::string again = bench::objectOf(bench.lastResult(13), "data");
  REQUIRE_FALSE(again.empty());
  CHECK(bench::member(bench.lastResult(13), "outcome") == "failed");
  CHECK(bench::member(again, "outcome") == "refused");
  CHECK(bench::member(again, "refusal") == "badList");
  CHECK(bench.v().pins.kill_cycles + bench.v().pins.reset_pulses == flashes);
}

TEST_CASE("twinoak.fw.apply refused: a hash the server did not name, and inProgress while this firmware is on trial") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  SUBCASE("expectedSha256 is another file's") {
    bench.server().queue(bench::ownJob(14, agent::kApplyType, bench::applyPayload(bench::sha256Hex(golden.file))).json());
    (void)bench.oneWake();
    const std::string data = bench::objectOf(bench.lastResult(14), "data");
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(data.find("\"refusal\"") == std::string::npos);  // no word of the seven fits: the check line says why
    CHECK(data.find("not the file the server named") != std::string::npos);
    CHECK(bench.world.images.read(Which::kCandidate).present);
    CHECK(bench.world.chip->chipHolds(golden));
  }
  SUBCASE("an apply while an update of either MCU is in the air is refused inProgress (S-7)") {
    // By hand, with the wake saying this firmware is on trial.
    (void)bench.oneWake();
    interfaces::BackendKnowledge known = bench::knowledgeFor(power::Band::kPlenty);
    known.wake.on_trial = true;
    bench.server().queue(bench::ownJob(15, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
    interfaces::BackendAnswer out;
    bench.boot();
    bench.v().begin();
    (void)bench.exchange(known, 600, &out);
    const std::string data = bench::objectOf(bench.lastResult(15), "data");
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "inProgress");
    CHECK(out.carried == interfaces::Ask::kNothing);
    CHECK(bench.world.chip->chipHolds(golden));
  }
}

TEST_CASE("twinoak.fw.apply of the Walter's own image: offered into the other slot, switched, and the result from the next boot's wake") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate self("self-a");
  readyUnit(bench, golden);
  bench::serveRelease(bench.world.http.server, self, bench::kBase, nullptr, nullptr, nullptr, true);
  bench.server().queue(bench::ownJob(21, agent::kOfferType, bench::offerPayload(self, agent::kProductWalter)).json());
  (void)bench.oneWake();
  const std::string offer = bench::objectOf(bench.lastResult(21), "data");
  REQUIRE_FALSE(offer.empty());
  CHECK(bench::member(offer, "stored") == "true");
  CHECK(bench::member(offer, "imageId") == "manager.walter.lte-m");
  core::update::SelfPayload received;
  REQUIRE(bench.v().self.received(&received));

  // The apply: the switch asks for a restart, the result is owed, nothing is posted yet.
  bench.server().queue(bench::ownJob(22, agent::kApplyType, bench::applyPayload(bench::sha256Hex(self.file), agent::kProductWalter)).json());
  (void)bench.oneWake();
  CHECK(bench.v().restart.asked == 1);
  CHECK(bench.lastResult(22).empty());
  CHECK(bench.half().firmware.resultOwed());

  // The restart: the bootloader's rules, the new image on trial; its first wake posts a heartbeat
  // - the trial's third condition - the trial confirms inside the exchange, and the result goes.
  bench.world.own.reboot();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  CHECK(bench.v().self.onTrial());
  const Pass& pass = bench.v().run();
  CHECK_FALSE(bench.v().self.onTrial());
  CHECK(bench.v().self.status().state == core::update::SelfState::kConfirmed);
  CHECK(pass.on_trial);
  const std::string result = bench.lastResult(22);
  REQUIRE_FALSE(result.empty());
  CHECK(bench::member(result, "outcome") == "ok");
  const std::string data = bench::objectOf(result, "data");
  CHECK(bench::member(data, "outcome") == "committed");
  CHECK(bench::member(data, "toVersion") == "0.4.0");
  CHECK(bench::objectOf(data, "journal").find("\"step\":\"confirmed\"") != std::string::npos);
  CHECK_FALSE(bench.half().firmware.resultOwed());
  CHECK(bench.half().firmware.counters().owed_posted == 1);
}

TEST_CASE("twinoak.fw.apply of the Walter's own image, rolled back: the result says what killed it") {
  // The 9 November trial, as the server would have seen it had this existed: the apply switches,
  // the image never reaches its main loop, the old image comes back with a panic reset and the
  // real core dump in the partition, and *that* boot's wake posts the owed result.
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate self("self-a");
  readyUnit(bench, golden);
  bench::serveRelease(bench.world.http.server, self, bench::kBase, nullptr, nullptr, nullptr, true);
  bench.server().queue(bench::ownJob(21, agent::kOfferType, bench::offerPayload(self, agent::kProductWalter)).json());
  (void)bench.oneWake();
  REQUIRE(bench::member(bench::objectOf(bench.lastResult(21), "data"), "stored") == "true");
  bench.server().queue(bench::ownJob(22, agent::kApplyType, bench::applyPayload(bench::sha256Hex(self.file), agent::kProductWalter)).json());
  (void)bench.oneWake();
  REQUIRE(bench.half().firmware.resultOwed());

  // The dump the trial image left, and slot 1's own application description naming the same ELF.
  bench.world.own.setElfSha(1, fixtures::kFixtureElfSha);
  fixtures::putCoreDumpFixture(bench.world.coredump_flash);
  // The switch's reboot starts the trial image; it panics before anything of ours runs, so the
  // next reset aborts the slot and the old image is selected again.
  bench.world.own.reboot();
  bench.world.own.reboot();
  REQUIRE(bench.world.own.runningIndex() == 0);
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPanic);
  bench.v().begin();
  REQUIRE(bench.v().self.status().state == core::update::SelfState::kRolledBack);
  (void)bench.v().run();

  const std::string result = bench.lastResult(22);
  REQUIRE_FALSE(result.empty());
  const std::string data = bench::objectOf(result, "data");
  CHECK(bench::member(data, "outcome") == "rolledBack");
  // The sentence a person reads on the job page: the rollback's reason, the reset that ended the
  // trial, the panic line and the task. This is the half of the fault the server could not see.
  const std::string reason = bench::member(data, "reason");
  CHECK(reason.find("rolled back: the image never reached its main loop") != std::string::npos);
  CHECK(reason.find("the reset that ended it: panic") != std::string::npos);
  CHECK(reason.find("panic: assert failed: esp_startup_start_app app_startup.c:86") != std::string::npos);
  CHECK(reason.find("in task ipc0") != std::string::npos);
  // And the same, field by field, for anything that renders rather than reads.
  const std::string trial = bench::objectOf(data, "lastTrial");
  REQUIRE_FALSE(trial.empty());
  CHECK(bench::member(trial, "reset") == "panic");
  CHECK(bench::member(trial, "attributed") == "true");
  CHECK(bench::member(trial, "panic") == fixtures::kFixturePanic);
  CHECK(bench::member(trial, "panicCut") == "false");
  CHECK(bench::member(trial, "task") == "ipc0");
  CHECK(bench::member(trial, "dumpHeld") == "true");
  // The journal lines the result carries end with the same three facts.
  const std::string journal = bench::objectOf(data, "journal");
  CHECK(journal.find("\"step\":\"rolled-back\"") != std::string::npos);
  CHECK(journal.find("\"step\":\"reset\"") != std::string::npos);
  CHECK(journal.find("\"step\":\"panic\"") != std::string::npos);
  CHECK(journal.find("\"step\":\"task\"") != std::string::npos);
  CHECK_FALSE(bench.half().firmware.resultOwed());

  // twinoak.fw.state carries it too, for a unit nobody asked anything of.
  bench.server().queue(bench::ownJob(23, agent::kStateType, "{}").json());
  (void)bench.oneWake();
  const std::string state = bench::objectOf(bench.lastResult(23), "data");
  REQUIRE_FALSE(state.empty());
  const std::string walter = bench::objectOf(state, "walter");
  const std::string from_state = bench::objectOf(walter, "lastTrial");
  REQUIRE_FALSE(from_state.empty());
  CHECK(bench::member(from_state, "reset") == "panic");
  CHECK(bench::member(from_state, "panic") == fixtures::kFixturePanic);
  CHECK(bench::member(from_state, "task") == "ipc0");
}

TEST_CASE("twinoak.power.cycle: within the board's bounds the rail is cut and the radio comes back; refused below the floor and in critical") {
  AgentBench bench;
  const Candidate golden("update-golden");
  readyUnit(bench, golden);
  (void)bench.oneWake();
  bench.v().pins.on_kill_cycle = [&bench] { bench.world.rak.restart(false); };
  const std::uint32_t boot_before = bench.world.rak.boot_id;
  bench.server().queue(bench::ownJob(31, agent::kPowerCycleType, bench::powerCyclePayload(5)).json());
  interfaces::BackendAnswer out;
  bench.boot();
  bench.v().begin();
  bench.v().pins.on_kill_cycle = [&bench] { bench.world.rak.restart(false); };
  (void)bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 600, &out);
  const std::string data = bench::objectOf(bench.lastResult(31), "data");
  REQUIRE_FALSE(data.empty());
  CHECK(bench::topKeys(data) == std::vector<std::string>{"offMs", "radioBack", "bootId"});
  CHECK(bench::member(data, "offMs") == "5000");
  CHECK(bench::member(data, "radioBack") == "true");
  CHECK(bench::member(data, "bootId") != std::to_string(boot_before));
  CHECK(bench.v().pins.kill_cycles == 1);
  CHECK(out.outside_ms >= 5000);

  SUBCASE("below the supervisor's floor for a power cycle") {
    bench.server().queue(bench::ownJob(32, agent::kPowerCycleType, bench::powerCyclePayload(5)).json());
    (void)bench.exchange(bench::knowledgeFor(power::Band::kSaving, true, 3450), 600, &out);
    CHECK(bench::member(bench.lastResult(32), "outcome") == "failed");
    CHECK(bench.v().pins.kill_cycles == 1);
  }
  SUBCASE("in critical the rail is nobody's to take") {
    bench.server().queue(bench::ownJob(33, agent::kPowerCycleType, bench::powerCyclePayload(5)).json());
    (void)bench.exchange(bench::knowledgeFor(power::Band::kCritical, true, 4100), 600, &out);
    CHECK(bench::member(bench.lastResult(33), "outcome") == "failed");
    CHECK(bench.v().pins.kill_cycles == 1);
  }
  SUBCASE("outside the board's bounds") {
    bench.server().queue(bench::ownJob(34, agent::kPowerCycleType, bench::powerCyclePayload(45)).json());
    (void)bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 600, &out);
    CHECK(bench::member(bench.lastResult(34), "outcome") == "failed");
    CHECK(bench.v().pins.kill_cycles == 1);
  }
}

TEST_CASE("an offer of each product, through the server's own X-Agent-Version rule: every request of the wake carries the header") {
  // What task p10-05 is about, end to end. The scripted server refuses an `/agent/v1` request
  // without `X-Agent-Version` with meshmaster.dk's 400 (sim/scripted_server.h,
  // `agent_version_required`, from its AgentV1.cs at commit d7b9b3ea9deb), and the base of a
  // release is `/agent/v1/firmware/7` (unit-protocol.md §4). Before this task the download's
  // three requests carried no header, and the offer failed in two seconds - job 22841 of
  // 8 November 2026 - while every host test passed, because the fake took any request.
  const char* product = nullptr;
  bool ota = false;
  const char* fixture = nullptr;
  const char* image_id = nullptr;
  SUBCASE("the radio's image") {
    product = agent::kProductRepeater;
    fixture = "store-a";
    image_id = "repeater.rak4630-single.868";
  }
  SUBCASE("the Walter's own image") {
    product = agent::kProductWalter;
    fixture = "self-a";
    image_id = "manager.walter.lte-m";
    ota = true;
  }
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate offered(fixture);
  readyUnit(bench, golden);
  bench::serveRelease(bench.world.http.server, offered, bench::kBase, nullptr, nullptr, nullptr, ota);
  bench.server().queue(bench::ownJob(55, agent::kOfferType, bench::offerPayload(offered, product)).json());
  (void)bench.oneWake();

  const std::string data = bench::objectOf(bench.lastResult(55), "data");
  REQUIRE_FALSE(data.empty());
  CHECK(bench::member(data, "stored") == "true");
  CHECK(bench::member(data, "imageId") == image_id);
  CHECK(bench::member(data, "bytes") == std::to_string(offered.file.size()));

  // Not one request of the wake went without the header - the heartbeat, the claim, the lease's
  // extensions, the manifest, the signature, every range of the file, and the result.
  const sim::ScriptedServer& server = bench.world.http.server;
  CHECK(server.unversioned_refused == 0);
  REQUIRE(server.requests.size() >= 6);
  int downloads = 0;
  for (const sim::SeenRequest& r : server.requests) {
    CHECK_MESSAGE(r.header("X-Agent-Version") == core::version(), r.path);
    if (r.path.rfind(std::string(bench::kBase) + "/", 0) == 0) ++downloads;
  }
  CHECK(downloads == 3);  // the manifest, its signature, the one range of this fixture's file
}
