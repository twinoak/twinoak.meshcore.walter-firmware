// **Every refusal has a word, and this is the test that counts them** (task p18-06, deliverable 2).
//
// On 24 November a session ended `refused` with no `refusal` member, and meshmaster.dk's door threw
// the whole result away: *A refused session names which precondition stopped it*. The firmware was
// green here, because task p18-05's tests ran against a script that did not hold the door's rule
// and because the refusal that was missing had never had a word. So this file holds the rule the
// server holds, on this side:
//
//   **a session**: every one of `core::update::Refusal`'s values has a word, and every one of those
//   words is in `NoiseScopeRefusals` - exhaustively, with nothing excepted, because the enum is
//   every argument `FirmwareJobs::refuseSession` can be given and every value the machine's own
//   ending can set;
//   **an apply**: every one of `agent::Refusal`'s values is in `FirmwareRefusals`, and every
//   `refuse(Reason::…)` of the radio's update machine - read out of the source at run time - either
//   has one of those words or is one of the **named six** the contract has no word for. A reason
//   added later is in neither list and fails here;
//   **the call sites**: every `refuseSession` of `firmware_jobs.cpp` passes a refusal and never
//   `kNone`; every `refuseApply` that passes `kNone` is one of the named sentences.
//
// The two lists are meshmaster.dk's own, at 13b192e with its task 260 applied:
// `src/MeshMaster.Contracts/NoiseScope.cs` (`NoiseScopeRefusals`) and `UnitJobs.cs`
// (`FirmwareRefusals`). They are transcribed here and nowhere else in this firmware, because a
// contract fixture for a list of words is not a shape the server sends.
#include <doctest.h>

#include <algorithm>
#include <fstream>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include "core/update/radio_update.h"
#include "core/update/scanner_session.h"
#include "walter_agent/firmware_jobs.h"

namespace upd = core::update;

namespace {

// `NoiseScopeRefusals.All` with meshmaster.dk's task 260 applied: the eight it had and the two this
// firmware has had since its own pile 16 that the list had not, `flash` with them.
const std::vector<std::string> kSessionWords = {"battery",  "link",      "clock",   "inProgress",
                                                "noGolden", "noScanner", "program", "noMemory",
                                                "flash",    "journal"};

// `FirmwareRefusals.All` with the same task applied: the seven it had and `flash`.
const std::vector<std::string> kApplyWords = {"battery",  "link",    "clock", "inProgress",
                                              "noGolden", "badList", "order", "flash"};

// **The refusals of the radio's update machine the contract has no word for.** Each is a
// precondition about the *file* rather than about the unit - the slot holds nothing to flash, or it
// holds something that is not for this board - and meshmaster.dk's own task 260 keeps *the unit gave
// no reason* for exactly these: the journal's `check` line carries the sentence. A seventh added
// later has to be put here by hand, which is the point.
const std::vector<std::string> kNoWordForTheseReasons = {
    "kNoProfile",      // no board profile for the slot
    "kNoCandidate",    // the store holds no image to flash
    "kImageUnreadable",// the stored image does not verify against its record
    "kWrongRole",      // the image's role is not one this slot may hold
    "kWrongBoard",     // the image's board is not the slot's
    "kWrongBand",      // the image's band is not the golden image's
};

// The reasons `refuse(Reason::…)` is called with, by name, so that a name read out of the source can
// be turned into the value `refusalFor` is asked about. Every name the source has must be here.
struct NamedReason {
  const char* name;
  upd::Reason reason;
};
const NamedReason kNamedReasons[] = {
    {"kNoStore", upd::Reason::kNoStore},
    {"kNoProfile", upd::Reason::kNoProfile},
    {"kNoCandidate", upd::Reason::kNoCandidate},
    {"kImageUnreadable", upd::Reason::kImageUnreadable},
    {"kOnBadList", upd::Reason::kOnBadList},
    {"kWrongRole", upd::Reason::kWrongRole},
    {"kWrongBoard", upd::Reason::kWrongBoard},
    {"kWrongBand", upd::Reason::kWrongBand},
    {"kRadioUpdateInProgress", upd::Reason::kRadioUpdateInProgress},
    {"kSelfUpdateUnconfirmed", upd::Reason::kSelfUpdateUnconfirmed},
    {"kLinkDown", upd::Reason::kLinkDown},
    {"kBatteryUnknown", upd::Reason::kBatteryUnknown},
    {"kBatteryLow", upd::Reason::kBatteryLow},
    {"kBatteryFalling", upd::Reason::kBatteryFalling},
    {"kNoGolden", upd::Reason::kNoGolden},
    {"kBusy", upd::Reason::kBusy},
    {"kFlashDidNotBegin", upd::Reason::kFlashDidNotBegin},
};

// **The refusals of an apply this firmware itself writes with no word** (the `refuseApply(out,
// Refusal::kNone, …)` call sites of `firmware_jobs.cpp`): a payload it cannot read, a product it is
// not, a build without a machine, a slot holding another file than the one named. The sentence is
// what the journal's `check` line carries, and the server prints *the unit gave no reason* beside
// it. A call site added later is not in this list and fails here.
const std::vector<std::string> kApplySentencesWithNoWord = {
    "a product this unit is not",
    "this build has no update machine",
    "expectedSha256 is not 64 hex digits",
    "the candidate held is not the file the server named",
    "no candidate is held",
    "this build has no self-update machine",
    "nothing is received into the other slot",
    "the other slot holds another file than the one the server named",
    "the journal could not be written: nothing is switched without it",
};

std::string sourceOf(const char* path) {
  const std::string full = std::string(TWINOAK_REPO_ROOT) + "/" + path;
  std::ifstream in(full, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot read " << full);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// The identifier that follows each occurrence of `prefix` in `text`, once each, in the order first
// seen. A reader of C++ by hand and not `<regex>`: GCC 13 cannot compile a `std::regex` here
// without a false `maybe-uninitialized`, which is an error in this suite.
std::vector<std::string> everyNameAfter(const std::string& text, const std::string& prefix) {
  std::vector<std::string> found;
  for (std::size_t at = text.find(prefix); at != std::string::npos;
       at = text.find(prefix, at + prefix.size())) {
    std::size_t i = at + prefix.size();
    std::string name;
    while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) != 0 ||
                               text[i] == '_')) {
      name += text[i++];
    }
    if (name.empty()) continue;
    if (std::find(found.begin(), found.end(), name) == found.end()) found.push_back(name);
  }
  return found;
}

bool isIn(const std::vector<std::string>& words, const std::string& word) {
  return std::find(words.begin(), words.end(), word) != words.end();
}

}  // namespace

TEST_CASE("every refusal a session can report has a word, and every word is one the contract has") {
  // The enum, end to end: `kFlash` is the last value, so a tenth refusal added after it fails this
  // loop rather than a session on a unit.
  REQUIRE(static_cast<int>(upd::Refusal::kFlash) == 10);
  int counted = 0;
  for (int v = 1; v <= static_cast<int>(upd::Refusal::kFlash); ++v) {
    const upd::Refusal refusal = static_cast<upd::Refusal>(v);
    const std::string word = upd::word(refusal);
    CAPTURE(v);
    CAPTURE(word);
    CHECK(word != "?");
    CHECK(word != "none");
    CHECK_MESSAGE(isIn(kSessionWords, word),
                  "a session's refusal the server's list has no word for: " << word);
    // And a phrase of its own for the journal's `check` line, which is what a person reads.
    CHECK(std::string(upd::describe(refusal)) != "?");
    ++counted;
  }
  CHECK(counted == 10);
  // The three the server's task 260 adds, by name.
  CHECK(std::string(upd::word(upd::Refusal::kFlash)) == "flash");
  CHECK(std::string(upd::word(upd::Refusal::kNoMemory)) == "noMemory");
  CHECK(std::string(upd::word(upd::Refusal::kJournal)) == "journal");
  MESSAGE("a session's ten refusals, every one of them a word of the contract's list");
}

TEST_CASE("every refusal an apply can report is a word the contract has") {
  REQUIRE(static_cast<int>(agent::Refusal::kFlash) == 8);
  for (int v = 1; v <= static_cast<int>(agent::Refusal::kFlash); ++v) {
    const std::string word = agent::name(static_cast<agent::Refusal>(v));
    CAPTURE(v);
    CAPTURE(word);
    CHECK_FALSE(word.empty());
    CHECK_MESSAGE(isIn(kApplyWords, word),
                  "an apply's refusal the server's list has no word for: " << word);
  }
  CHECK(std::string(agent::name(agent::Refusal::kFlash)) == "flash");
  // And the one this task is about: the machine's reason for a flash that never began maps to it.
  CHECK(agent::refusalFor(upd::Reason::kFlashDidNotBegin) == agent::Refusal::kFlash);
}

TEST_CASE("every way the radio's update machine refuses a run is a word of the list, or one of the six the contract has none for") {
  const std::string source = sourceOf("components/core/update/radio_update.cpp");
  // `refuse()` is the one function that enters `State::kRefused`, and `start()`'s three early
  // answers are the only other way a run is refused before it begins.
  std::vector<std::string> names = everyNameAfter(source, "refuse(Reason::");
  for (const std::string& early : everyNameAfter(source, "reason_ = Reason::")) {
    // `kJournal` is set where the journal refuses an entry **mid-run**, which ends a run `stuck`
    // and not `refused`, and `kNone` is `clear()` forgetting the last run's. The other three are
    // `start()`'s own and are refusals.
    if (early == "kJournal" || early == "kNone") continue;
    if (std::find(names.begin(), names.end(), early) == names.end()) names.push_back(early);
  }
  REQUIRE_FALSE(names.empty());
  for (const std::string& name : names) {
    CAPTURE(name);
    const NamedReason* known = nullptr;
    for (const NamedReason& one : kNamedReasons) {
      if (name == one.name) known = &one;
    }
    REQUIRE_MESSAGE(known != nullptr,
                    "a reason this test does not know: add " << name << " to kNamedReasons");
    const agent::Refusal refusal = agent::refusalFor(known->reason);
    if (refusal != agent::Refusal::kNone) {
      CHECK(isIn(kApplyWords, agent::name(refusal)));
      continue;
    }
    CHECK_MESSAGE(isIn(kNoWordForTheseReasons, name),
                  "a refusal with no word that is not one of the named six: " << name
                      << " - give it a word of the contract's list, or name it in this test");
  }
  // The five `reason_ = Reason::…` of that file, so that a sixth is read by a person before it is
  // let through as a refusal or as a failure.
  CHECK(everyNameAfter(source, "reason_ = Reason::").size() == 5);
  MESSAGE("the machine's refusals: " << names.size() << " of them, "
                                     << kNoWordForTheseReasons.size() << " with no word");
}

TEST_CASE("every refusal of a session's job passes a word, and every wordless refusal of an apply is a named sentence") {
  const std::string source = sourceOf("components/agent/firmware_jobs.cpp");
  // A session's: `kNone` is not one of them. The server throws a `refused` session with no word
  // away whole, so there is no such thing as a session refused for no reason.
  const std::vector<std::string> session =
      everyNameAfter(source, "refuseSession(out, upd::Refusal::");
  REQUIRE_FALSE(session.empty());
  for (const std::string& name : session) {
    CAPTURE(name);
    CHECK(name != "kNone");
  }
  // An apply's: the sentences passed with `Refusal::kNone`, each named above. Every quoted string
  // of the call's own statement, because one of them is a ternary with two.
  int wordless = 0;
  const std::string call = "refuseApply(out, Refusal::kNone,";
  for (std::size_t at = source.find(call); at != std::string::npos;
       at = source.find(call, at + call.size())) {
    const std::size_t end = source.find(");", at);
    REQUIRE(end != std::string::npos);
    const std::string statement = source.substr(at, end - at);
    ++wordless;
    for (std::size_t q = statement.find('"'); q != std::string::npos;
         q = statement.find('"', q + 1)) {
      const std::size_t close = statement.find('"', q + 1);
      if (close == std::string::npos) break;
      const std::string sentence = statement.substr(q + 1, close - q - 1);
      q = close;
      CAPTURE(sentence);
      CHECK_MESSAGE(isIn(kApplySentencesWithNoWord, sentence),
                    "an apply refused with no word and a sentence this test does not know: "
                        << sentence << " - give it a word, or name it in this test");
    }
  }
  CHECK(wordless >= 7);
  MESSAGE("an apply's refusals with no word: " << wordless << " call site(s)");
}
