// A silent radio is not a scanner (task p9-03), on the wake bench with the scripted meshmaster.dk
// and a simulated nRF52840 behind the RAK. The review of phase 8 (S1) found p8-04's one flag -
// `radio_silent`, set whenever there was no frame session - standing in for a classification of
// the supervisor's. A slot running NoiseScope and a slot awaiting its first install have no frame
// session either, and the supervisor rules on purpose that neither is *the link is known down*,
// so that flag lifted an apply's two refusals and waived a self-switch's trial condition.
//
// **The two facts this file keeps apart:**
//
//   the key           *this end has no key to tell a job for its own repeater by* - what the
//                     **claim** reads, and it follows the radio's own answer to `HELLO` and
//                     `IDENTITY` in this wake (task p8-04, test_radio_before_a_job.cpp);
//   the classification *the link is known down* / *the radio will not answer a frame `HELLO`* -
//                     the **supervisor's**, through `core::supervisor::linkKnownDown()` and
//                     `radioKnownDown()` (host_test/core/supervisor/test_known_down.cpp), and
//                     what an apply's preconditions and a self-switch's trial read.
//
// They are two facts because they come apart in both directions, and the last two cases of each
// section are those two directions: a radio that answers everything in a wake that has classified
// nothing, and a radio the supervisor has given up on that answers again by the time the job runs.
#include <doctest.h>

#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "fixtures/manifest_files.h"

using bench::AgentBench;
using bench::Candidate;
using core::image::Which;
namespace power = core::power;
namespace sup = core::supervisor;
namespace upd = core::update;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// A unit ready for an update (test_firmware_jobs.cpp's own helper): the golden image on the chip
// and in the store, running, and a modem that works.
void readyUnit(AgentBench& bench, const Candidate& golden) {
  REQUIRE(bench.world.images.put(Which::kGolden, bench::Slot{0}, golden) == bench::StoreError::kNone);
  bench.world.chip->installOnChip(golden);
  bench.world.chip->radioBoots();
  bench.modemWorks();
}

// What the radio is, as against what the supervisor has decided about it.
enum class Radio { kAnswers, kScanner, kDeaf };

void putRadio(AgentBench& bench, Radio radio) {
  // Nothing the radio was going to say is said: a module that has just become a scanner has no
  // event of the repeater's still in flight, and a `HELLO` that read one would come back
  // *malformed* rather than *the link is down* and so would never reach §9's probe
  // (sim/scripted_radio.h's `forgetPending`, written for turning a repeater into a scanner).
  bench.world.rak.forgetPending();
  bench.world.rak.deaf = radio == Radio::kDeaf;
  bench.world.rak.scanner = radio == Radio::kScanner;
}

// The supervisor's own classification, as a timer wake's step 2 gets it: `step()` until it has
// decided, and **no further** - the first step of a radio that is not answering classifies the
// silence and then enters the climb, and a test that wanted a classification did not ask for a
// reset pulse.
//
// A whole wake would do it too and is not used here: a wake whose radio is silent has no battery
// reading either, so its band is `no-reading`, and `no-reading` refuses an update before the
// update machine's own preconditions are ever reached (core/power/interval_policy.cpp). The
// exchanges below are made by hand at `plenty` for exactly that reason.
sup::Health classify(AgentBench& bench) {
  // The session first, as step 1 opens one reading power over the link (core/cycle/
  // duty_cycle.cpp, `readPower`): §9's text mode is found by a `HELLO` that goes unanswered three
  // times and a probe that comes back a `{` line, and a supervisor asked before any of that sees
  // a radio that is merely quiet.
  (void)bench.v().link.hello();
  // Two steps and no third. `stepIdle` asks and, when nothing answers, enters *classify*; it is
  // `stepClassify` that decides, and the step after **that** would be the ladder's first rung,
  // which this file has no business asking for. The health is already `silent` after the first
  // step, so "step until it is not unknown" would stop one step too early - which is how the
  // slot awaiting its first install first read as a dead radio here.
  (void)bench.v().supervisor->step();
  if (bench.v().supervisor->climbing()) (void)bench.v().supervisor->step();
  return bench.v().supervisor->health();
}

// A battery reading taken while the radio still answered, which is what step 1 of a wake leaves
// behind (core/update/last_good_battery.h). A self-switch asks for a reading before anything else
// and the gauge is on the *radio's* I2C, so without this a switch on a radio that has gone silent
// is refused on the battery and the trial's condition - what this file is about - is never
// reached. On a unit that is the ordinary order of events: the wake reads power, and the radio is
// found silent afterwards.
void primeBattery(AgentBench& bench) {
  (void)bench.v().link.hello();  // §5.8 is asked inside a session
  upd::BatteryReading reading;
  REQUIRE(bench.v().battery.read(&reading));
}

// One exchange of the agent, as the wake cycle's step 8 makes it, at a band that allows an update.
// The wake's record carries `kHealthy` (`knowledgeFor`'s default) throughout this file **on
// purpose**: what the refusals read is the supervisor, and a record that agreed with it would
// prove nothing.
void oneExchange(AgentBench& bench) {
  interfaces::BackendAnswer out;
  (void)bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 1800, &out);
}

// The `data` object of an apply's result, which is where `outcome` and `refusal` are.
std::string applyData(AgentBench& bench, long long id) { return bench.lastData(id); }

}  // namespace

// ---------------------------------------------------------------------- the radio's own apply

TEST_CASE("twinoak.fw.apply: the link and battery refusals read the supervisor's classification, not this wake's HELLO") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  const std::string sha = bench::sha256Hex(candidate.file);

  SUBCASE("a scanner slot: the apply is refused on the link, and nothing is flashed under it") {
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    putRadio(bench, Radio::kScanner);
    bench.boot();
    bench.v().begin();
    REQUIRE(classify(bench) == sup::Health::kScanner);
    const unsigned writes = bench.world.chip->chip.counters().writes;

    bench.server().queue(bench::ownJob(90, agent::kApplyType, bench::applyPayload(sha)).json());
    oneExchange(bench);

    // The claim: no frame session, so no key, so no mesh type - and the four own types all the
    // same, which is how the apply was claimed at all.
    CHECK_FALSE(bench.half().mesh.hasOwnKey());
    // And the refusal is the link's, in the server's word.
    const std::string data = applyData(bench, 90);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "link");
    CHECK(bench.world.chip->chip.counters().writes == writes);
    CHECK(bench.world.images.read(Which::kCandidate).present);
    CHECK(bench.world.images.store.badCount() == 0);
  }

  SUBCASE("a slot awaiting its first install: the same, and the image is not put on the bad list") {
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    putRadio(bench, Radio::kDeaf);
    bench.boot();
    bench.v().begin();
    // `rak provision` left the radio a bootloader and no application; the store still holds the
    // golden image it had before, which is the 6 November shape (a radio recovered under a Walter
    // that never lost its store).
    bench.v().supervisor->markAwaitingFirstInstall();
    REQUIRE(classify(bench) == sup::Health::kAwaitingFirstInstall);
    const unsigned writes = bench.world.chip->chip.counters().writes;

    bench.server().queue(bench::ownJob(91, agent::kApplyType, bench::applyPayload(sha)).json());
    oneExchange(bench);

    const std::string data = applyData(bench, 91);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "link");
    CHECK(bench.world.chip->chip.counters().writes == writes);
    CHECK(bench.world.images.store.badCount() == 0);
  }

  SUBCASE("a dead radio nothing has classified: refused on the link, because nobody has said why") {
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    putRadio(bench, Radio::kDeaf);
    bench.boot();
    bench.v().begin();
    // No `classify()`: this is the console's `agent wake`, which runs no step 2 and carries no
    // health. The radio is as dead as the case below it; the difference is the decision.
    REQUIRE(bench.v().supervisor->health() == sup::Health::kUnknown);

    bench.server().queue(bench::ownJob(92, agent::kApplyType, bench::applyPayload(sha)).json());
    oneExchange(bench);

    const std::string data = applyData(bench, 92);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "link");
  }

  SUBCASE("a dead radio the supervisor has given up on: let past both refusals, which is the unbrick path") {
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    putRadio(bench, Radio::kDeaf);
    bench.boot();
    bench.v().begin();
    REQUIRE(classify(bench) == sup::Health::kSilent);

    bench.server().queue(bench::ownJob(93, agent::kApplyType, bench::applyPayload(sha)).json());
    oneExchange(bench);

    // Not a refusal: the machine ran. There is no gauge either - the INA3221 is on the radio's
    // I2C - and the battery-unknown refusal is lifted by the same classification, which is the
    // whole reason task p2-05's supervisor can put an image on a radio that will not answer.
    const std::string data = applyData(bench, 93);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") != "refused");
    CHECK_FALSE(has(data, "\"refusal\""));
    CHECK(has(data, "\"step\":\"check\""));
    CHECK(has(data, "\"step\":\"flashing-candidate\""));
    CHECK(bench.world.chip->chip.counters().writes > 0);
  }

  SUBCASE("a live radio: nothing is lifted and nothing refuses, and the claim takes every mesh type") {
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    putRadio(bench, Radio::kAnswers);
    bench.boot();
    bench.v().begin();
    REQUIRE(classify(bench) == sup::Health::kHealthy);

    bench.server().queue(bench::ownJob(94, agent::kApplyType, bench::applyPayload(sha)).json());
    oneExchange(bench);

    CHECK(bench.half().mesh.hasOwnKey());
    CHECK(bench.half().mesh.supports("status"));
    const std::string data = applyData(bench, 94);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "committed");
  }

  SUBCASE("the two facts come apart: the supervisor has given up and the radio answers again") {
    // A wake whose step 2 classified the radio silent, after which it came back - a power cycle's
    // rung, or a repeater that finished booting. The **claim** follows the radio: the key is read
    // and every mesh type offered. The **refusals** follow the classification and are lifted; the
    // link is up anyway, so the apply runs and commits, which is what makes this harmless.
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    putRadio(bench, Radio::kDeaf);
    bench.boot();
    bench.v().begin();
    REQUIRE(classify(bench) == sup::Health::kSilent);
    putRadio(bench, Radio::kAnswers);

    bench.server().queue(bench::ownJob(95, agent::kApplyType, bench::applyPayload(sha)).json());
    oneExchange(bench);

    CHECK(bench.half().mesh.hasOwnKey());
    CHECK(bench.half().mesh.supports("status"));
    CHECK(bench.v().supervisor->health() == sup::Health::kSilent);
    const std::string data = applyData(bench, 95);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "committed");
  }
}

// ---------------------------------------------------------------------- the Walter's own switch

TEST_CASE("twinoak.fw.apply of the Walter's own image: the trial's radio condition reads the supervisor's classification") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate self("self-a");
  readyUnit(bench, golden);
  // The image into the other slot, in a wake with the radio answering: an offer is a download and
  // has nothing to do with the radio, and this is the state every case below starts from.
  bench::serveRelease(bench.world.http.server, self, bench::kBase, nullptr, nullptr, nullptr, true);
  bench.server().queue(bench::ownJob(100, agent::kOfferType, bench::offerPayload(self, agent::kProductWalter)).json());
  (void)bench.oneWake();
  REQUIRE(bench::member(bench.lastData(100), "stored") == "true");
  upd::SelfPayload received;
  REQUIRE(bench.v().self.received(&received));
  const std::string apply = bench::ownJob(101, agent::kApplyType,
                                          bench::applyPayload(bench::sha256Hex(self.file), agent::kProductWalter))
                                .json();

  SUBCASE("a scanner slot: the switch is refused on the battery, which is the gauge a scanner cannot give") {
    putRadio(bench, Radio::kScanner);
    bench.boot();
    bench.v().begin();
    REQUIRE(classify(bench) == sup::Health::kScanner);
    bench.server().queue(apply);
    oneExchange(bench);

    // **A scanner slot and a battery reading cannot be had in one Walter life.** The gauge is the
    // INA3221 on the *radio's* I2C and is read with a §5.8 frame; §9 says a client that has seen a
    // frame reads no text until something resets the RAK (client.cpp's `pump`: `mode != kFrames`
    // is what lets a text byte through), so the only way to `scanner` is a Walter whose link has
    // never had a frame - and that Walter has no reading either. `SelfUpdate::preconditions` asks
    // for one before anything else, so the switch is refused here and the trial's radio condition
    // is never reached. That is why `radioKnownDown(kScanner)` is belt and braces rather than a
    // path: a scanner also counts as *up* in `switchNow`, which would have let it past anyway.
    CHECK(bench.v().restart.asked == 0);
    const std::string data = bench.lastData(101);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "battery");
    CHECK_FALSE(bench.v().self.onTrial());
  }

  SUBCASE("a slot awaiting its first install: the switch goes and the condition is not applicable") {
    bench.boot();
    bench.v().begin();
    primeBattery(bench);
    putRadio(bench, Radio::kDeaf);
    bench.v().supervisor->markAwaitingFirstInstall();
    REQUIRE(classify(bench) == sup::Health::kAwaitingFirstInstall);
    bench.server().queue(apply);
    oneExchange(bench);
    REQUIRE(bench.v().restart.asked == 1);

    bench.world.own.reboot();
    bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
    bench.v().begin();
    REQUIRE(bench.v().self.onTrial());
    // It cannot answer a frame HELLO until it is flashed, and holding the trial to one it can
    // never get would roll back every Walter image a freshly provisioned unit is offered.
    CHECK(bench.v().self.status().radio == upd::Condition::kNotApplicable);
  }

  SUBCASE("a dead radio the supervisor has given up on: the switch goes and the condition is not applicable") {
    bench.boot();
    bench.v().begin();
    primeBattery(bench);
    putRadio(bench, Radio::kDeaf);
    REQUIRE(classify(bench) == sup::Health::kSilent);
    bench.server().queue(apply);
    oneExchange(bench);
    REQUIRE(bench.v().restart.asked == 1);

    bench.world.own.reboot();
    bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
    bench.v().begin();
    REQUIRE(bench.v().self.onTrial());
    CHECK(bench.v().self.status().radio == upd::Condition::kNotApplicable);
  }

  SUBCASE("a dead radio nothing has classified: the switch is refused, and no good image is put on a trial it cannot pass") {
    bench.boot();
    bench.v().begin();
    primeBattery(bench);
    putRadio(bench, Radio::kDeaf);
    REQUIRE(bench.v().supervisor->health() == sup::Health::kUnknown);
    bench.server().queue(apply);
    oneExchange(bench);

    // Nothing was switched: a wake that classified nothing waives nothing.
    CHECK(bench.v().restart.asked == 0);
    const std::string data = bench.lastData(101);
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "link");
    CHECK_FALSE(bench.v().self.onTrial());
  }

  SUBCASE("a live radio: the switch goes and the trial asks the radio, as it always did") {
    bench.boot();
    bench.v().begin();
    primeBattery(bench);
    putRadio(bench, Radio::kAnswers);
    REQUIRE(classify(bench) == sup::Health::kHealthy);
    bench.server().queue(apply);
    oneExchange(bench);
    REQUIRE(bench.v().restart.asked == 1);

    bench.world.own.reboot();
    bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
    bench.v().begin();
    REQUIRE(bench.v().self.onTrial());
    CHECK(bench.v().self.status().radio == upd::Condition::kPending);
  }
}
