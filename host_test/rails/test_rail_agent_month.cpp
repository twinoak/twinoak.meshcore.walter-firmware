// The rails phase 4 inherited from the firmware the agent library came from (task p4-06), held
// **over the link and the modem** - where the exit-node firmware's own tests cannot see - through
// a month of wakes as an exit node:
//
//   R9   nothing the mesh said is logged as text
//   R15  a password is in RAM for the length of the exchange and nowhere else
//   R16  one packet per command
//   R17  the token is printed by nothing and is sent to one host, in one header
//   R18  the server never chooses an image and never decides one is good
//   R19  no job a finished wake claimed is left leased
//
// The month is task p4-05's (host_test/agent-backend/test_agent_month.cpp) with everything that
// could leak planted in it: a neighbour whose admin password this unit has to use, a
// `password.set` that puts a new one on the air, a `config.read` that brings a neighbour's own
// settings back, adverts with a stranger's name in them, the token in the settings - and then
// everything this unit wrote down is searched for all of it.
//
// What is searched, and what is deliberately not:
//
//   the console's transcript            what a person at the cable saw over the month
//   the library's log                   every line `mm::log*` formatted (main.cpp's sink, wired
//                                       into the bench by this task: see wake_bench.h)
//   `dump`, `status`, `journal`, `wake` the summaries a person reads first
//   the journal partition's bytes       read out of the simulated flash, not through a printer
//   the retained records' bytes         RTC memory: the wake records, the readings, the caps
//   the kept image's flash              searched for the password and the token only - what it
//                                       holds is a **result** the server would not take yet, and
//                                       that result carries what the mesh said because that is
//                                       what the server asked for (the same bytes, over TLS, one
//                                       wake later)
//   every HTTPS request                 searched for the password and the token only, and for
//                                       the token in a header other than `authorization`. A
//                                       job's result *is* what the mesh said: R9 is **never
//                                       written down**, not never sent to the server that asked
//
// GoogleTest, because the repeater behind the RAK is the library's own FakeMesh. It lives in
// rails/ with the other rails and is compiled into `agent_backend_tests` rather than
// `core_tests`, which host_test/CMakeLists.txt says in one line.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "agent-backend/agent_mesh_bench.h"
#include "core/link/codec/wire.h"

namespace {

using bench::Candidate;
using bench::MeshBench;
using core::cycle::Ending;
using core::cycle::Pass;
using core::image::Which;

// What must be nowhere. Each is long enough that a stray substring would be found, and nothing
// else any line prints looks like one.
constexpr const char* kNeighbourPw = "n3gh-adm1n-pw";   // the neighbour's admin password (the claim's)
constexpr const char* kNewPw = "n3w-mesh-pw-x9";        // what a `password.set` puts on the air
constexpr const char* kOwnPw = "adm1n-pass";            // the unit's own repeater's, over the local door
constexpr const char* kToken = "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV";  // the bench's server token
constexpr const char* kTokenBody = "zQ7x4LpR";
// What the mesh said: a neighbour's own setting, read back by a `config.read` job, and the name
// a stranger put in an advert. Neither may be written down (spec/SYSTEM.md S-12).
constexpr const char* kNeighbourSaid = "Grenaa Chimney Nine";
constexpr const char* kStrangerName = "Stranger at Grenaa";

struct Needle {
  const char* what;
  const char* text;
};

const Needle kSecrets[] = {
    {"the neighbour's admin password", kNeighbourPw},
    {"the new password a job set", kNewPw},
    {"the own repeater's admin password", kOwnPw},
    {"the server's token", kToken},
    {"the token's body", kTokenBody},
};

const Needle kMeshSaid[] = {
    {"what a neighbour's config.read answered", kNeighbourSaid},
    {"the name a stranger put in an advert", kStrangerName},
};

void searchSecrets(const std::string& text, const char* where) {
  for (const Needle& needle : kSecrets) {
    EXPECT_EQ(text.find(needle.text), std::string::npos) << where << " carries " << needle.what;
  }
}

void searchAll(const std::string& text, const char* where) {
  searchSecrets(text, where);
  for (const Needle& needle : kMeshSaid) {
    EXPECT_EQ(text.find(needle.text), std::string::npos) << where << " carries " << needle.what;
  }
}

// An object's bytes, as a string to search: a password read back out of the buffer it crossed.
bool holds(const void* object, std::size_t size, const char* needle) {
  const std::string image(static_cast<const char*>(object), size);
  return image.find(needle) != std::string::npos;
}

// A simulated flash, byte for byte.
std::string flashBytes(sim::SimFlash& flash) {
  std::string all;
  for (std::uint32_t at = 0; at < flash.size(); at += 64) {
    std::uint8_t buf[64] = {};
    EXPECT_TRUE(flash.read(at, buf, sizeof buf));
    all.append(reinterpret_cast<const char*>(buf), sizeof buf);
  }
  return all;
}

// A job for the neighbour at admin class, with the password the claim carries.
fake::Job neighbourConfigRead(long long id) {
  return fake::Job(id, "config.read")
      .forRepeater(18, "Grenaa Chimney #1", fake::repeaterKey())
      .atClass("admin")
      .leasedUntil(0)
      .withAdminPassword(kNeighbourPw)
      .withAclLevel(mm::kPermAdmin)
      .withPayload("{\"keys\":[\"name\"]}")
      .asDirectNeighbour();
}

// And one that puts a new password on the air (the one thing of S-13 this link does).
fake::Job neighbourPasswordSet(long long id) {
  return fake::Job(id, "password.set")
      .forRepeater(18, "Grenaa Chimney #1", fake::repeaterKey())
      .atClass("admin")
      .leasedUntil(0)
      .withAdminPassword(kNeighbourPw)
      .withNewPassword(kNewPw)
      .withAclLevel(mm::kPermAdmin)
      .withPayload("{\"field\":\"adminPassword\"}")
      .asDirectNeighbour();
}

// One advert the radio heard, with a name in it: MeshCore's payload, flags `0x80` (a name) and
// `0x01` (a repeater), then the name's bytes (host_test/agent/test_agent_observations.cpp).
sim::SimRak::Advert advertNamed(std::uint8_t first_key_byte, const char* name) {
  sim::SimRak::Advert a;
  a.pubkey.assign(32, first_key_byte);
  a.age_s = 60;
  a.snr_q4 = 21;
  a.rssi = -91;
  a.path_len = 0;
  a.app_data.push_back(0x81);
  for (const char* c = name; *c != 0; ++c) a.app_data.push_back(static_cast<std::uint8_t>(*c));
  return a;
}

// The month's own generator: the same month every run (the other benches' LCG).
class Dice {
 public:
  explicit Dice(std::uint32_t seed) : state_(seed) {}
  std::uint32_t next() {
    state_ = state_ * 1664525u + 1013904223u;
    return state_ >> 8;
  }
  std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
  bool chance(std::uint32_t percent) { return below(100) < percent; }

 private:
  std::uint32_t state_;
};

TEST(RailAgent, AMonthAsAnExitNodeWritesNothingDownAndPutsOnePacketOnTheAirPerCommand) {
  MeshBench bench;
  bench.agent.modemWorks();
  Dice dice(20261023u);
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  // The release whose only image is a scanner's: check 3 of spec/FIRMWARE-MANIFEST.md §5 refuses
  // it for this slot, however firmly the server offered it (R18).
  const Candidate wrong_slot("store-scanner");
  ASSERT_EQ(bench.agent.world.images.put(Which::kGolden, bench::Slot{0}, golden), bench::StoreError::kNone);
  bench.agent.world.chip->installOnChip(golden);
  bench.agent.world.chip->radioBoots();
  bench.rak().console = bench::ownConsole;
  // The neighbour on the air: the password the claim will carry, and a setting of its own that a
  // `config.read` brings back - which is a thing the mesh said.
  bench.repeater().setAdminPassword(kNeighbourPw);
  bench.repeater().grant(bench::kOwnKeyHex, mm::kPermAdmin);
  bench.repeater().setConfig("name", kNeighbourSaid);

  // Everything printed, over the whole month, and the library's own lines at `debug` - the
  // loudest this port can be, which is the hardest case for a rail about what is written down.
  std::string transcript = bench.agent.v().runLine("set log.level debug");
  ASSERT_TRUE(bench.agent.world.agent != nullptr);

  std::uint32_t wakes = 0;
  std::uint32_t slept = 0;
  std::uint32_t mesh_wakes = 0;     // wakes with a packet on the air
  std::uint32_t sends = 0;          // SEND_* answered with a packet sent, over the month
  std::uint32_t password_jobs = 0;
  std::uint32_t adverts_pushed = 0;
  std::uint32_t offers_refused = 0;
  std::uint32_t rolled_back = 0;
  std::uint32_t kept_results = 0;   // results kept across a sleep because the server was away
  long long job_id = 2000;
  long long config_read_job = 0;    // the one whose result carries what the neighbour said

  const int kWakes = 200;
  const int kAwayFrom = 80;   // the week the server is away
  const int kAwayTo = 110;

  for (int i = 0; i < kWakes; ++i) {
    const std::uint32_t coverage = dice.below(100);
    bench.agent.world.modem.deaf = coverage < 8;
    bench.agent.world.modem.never_registers = coverage >= 8 && coverage < 16;
    bench.agent.world.modem.time_answered = !dice.chance(10);
    bench.agent.server().away(i >= kAwayFrom && i < kAwayTo);
    // The wakes that must not lose their job to a dead modem.
    const bool scripted = i == 20 || i == 40 || i == 60 || i == 130 || i == 150 || i == 170;
    if (scripted) {
      bench.agent.world.modem.deaf = false;
      bench.agent.world.modem.never_registers = false;
      bench.agent.world.modem.time_answered = true;
    }

    bench.agent.server().forgetQueued();
    if (i == 20) {
      // A `config.read` on the neighbour: a login with the claim's password, then the read.
      config_read_job = job_id;
      bench.agent.server().queue(neighbourConfigRead(job_id++).json());
      ++password_jobs;
    } else if (i == 40) {
      // A new password on the air, and the far side takes it.
      bench.agent.server().queue(neighbourPasswordSet(job_id++).json());
      ++password_jobs;
    } else if (i == 60) {
      // The server offers an image that is not for this slot. This firmware refuses it (R18).
      bench::serveRelease(bench.agent.world.http.server, wrong_slot);
      bench.agent.server().queue(bench::ownJob(job_id++, agent::kOfferType, bench::offerPayload(wrong_slot)).json());
      ++offers_refused;
    } else if (i == 130) {
      // And one that is: stored as the candidate, by this firmware's own six checks.
      bench::serveRelease(bench.agent.world.http.server, candidate);
      bench.agent.server().queue(bench::ownJob(job_id++, agent::kOfferType, bench::offerPayload(candidate)).json());
    } else if (i == 150) {
      // An apply the server calls good: the candidate boots a version its own descriptor does
      // not name, so **this firmware's** health check fails it and the golden image comes back.
      ASSERT_EQ(bench.agent.world.images.put(Which::kCandidate, bench::Slot{0}, candidate), bench::StoreError::kNone);
      bench.agent.world.chip->boot = bench::Boot::kOldVersion;
      bench.agent.world.chip->old_version = golden.version;
      bench.agent.server().queue(
          bench::ownJob(job_id++, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
    } else if (i == 170) {
      bench.agent.world.chip->boot = bench::Boot::kNormal;
      bench.agent.server().queue(bench::ownJob(job_id++, agent::kStateType, "{}").json());
    } else if (dice.chance(25)) {
      if (dice.chance(60)) {
        bench.agent.server().queue(bench::neighbourStatus(job_id++).json());
      } else {
        bench.agent.server().queue(bench::ownConfigRead(job_id++).json());  // the own door, at admin
      }
    }
    // Adverts the radio heard while we slept, now and then: two of them carry a name.
    if (i % 17 == 3) {
      bench.rak().observations.push_back(advertNamed(static_cast<std::uint8_t>(0xA0 + (i % 16)), kStrangerName));
      bench.rak().observations.push_back(advertNamed(static_cast<std::uint8_t>(0xB0 + (i % 16)), "Risskov Garden"));
      adverts_pushed += 2;
    }

    const std::uint32_t tx_before = bench.rak().client_tx();
    const Pass& pass = bench.agent.oneWake();
    ++wakes;

    // -- the rails, every wake ---------------------------------------------------------------
    EXPECT_EQ(pass.ending, Ending::kSlept) << "wake " << i;
    if (pass.ending == Ending::kSlept) ++slept;
    EXPECT_LE(pass.spent_ms, pass.bound_ms) << "wake " << i;

    // R16, one packet per command: every packet the radio put on the air this wake is a SEND_*
    // this end made and was told went out. Not a packet more, not a packet fewer.
    const std::uint32_t tx = bench.rak().client_tx() - tx_before;
    const agent::LinkMeshCounters& counts = bench.agent.half().mesh.linkCounters();
    EXPECT_EQ(tx, counts.sends) << "wake " << i << ": client_tx moved by " << tx << " for " << counts.sends
                                << " sends the runner made";
    if (tx != 0) ++mesh_wakes;
    sends += counts.sends;
    // A job for the unit's own repeater goes through the local door and puts nothing on the air.
    if (counts.local_requests != 0 && counts.requests == counts.local_requests) {
      EXPECT_EQ(tx, 0u) << "wake " << i;
    }

    // R17, the token: no error text of a call this wake carries it.
    searchSecrets(bench.agent.half().server.lastCall().error, "the last call's error text");
    searchSecrets(bench.agent.half().server.last().error, "the config call's error text");

    // R19, no job left leased: a wake that reached the server posted or kept everything it
    // claimed. A wake whose server was away keeps the result in the flash window and the next
    // reachable wake posts it - which is the one case `queueDepth` is allowed to be above zero.
    if (!bench.agent.server().isAway()) {
      EXPECT_EQ(bench.agent.half().jobs.queueDepth(), 0) << "wake " << i;
    } else if (bench.agent.half().jobs.queueDepth() != 0) {
      ++kept_results;
    }

    rolled_back += bench.agent.half().firmware.counters().rolled_back;
    if (i % 10 == 0) transcript += bench.agent.v().runLine("journal");
  }

  // -- what the month did, so that this is not a test that passed by doing nothing -------------
  EXPECT_EQ(slept, wakes);
  EXPECT_GT(mesh_wakes, 5u) << "no packet ever went on the air: R16 held nothing";
  EXPECT_GT(sends, 10u);
  EXPECT_EQ(password_jobs, 2u);
  EXPECT_GT(adverts_pushed, 10u);
  EXPECT_GT(bench.agent.world.agent->log_lines, 100u) << "the library logged nothing: R9 held nothing";
  // R18: the offer for the wrong slot was refused **here**, with the check number, and nothing
  // was stored; the apply the server called good rolled back on our own health check.
  EXPECT_EQ(offers_refused, 1u);
  EXPECT_GE(rolled_back, 1u);
  EXPECT_GE(bench.agent.world.images.store.badCount(), 1u);
  EXPECT_EQ(std::string(bench.agent.world.images.read(Which::kGolden).version), golden.version);
  // -- the controls: every needle was really said, and really reached the server --------------
  // A search that finds nothing is a rail only if the thing was there to find. The neighbour took
  // the new password; its `name` came back over the air and went to the server in the job's
  // result; the stranger's name went up in an observation batch. All three, and then none of them
  // written down anywhere below.
  EXPECT_EQ(bench.repeater().adminPassword(), std::string(kNewPw));
  ASSERT_NE(config_read_job, 0);
  const std::string read_back = bench.agent.lastResult(config_read_job);
  ASSERT_FALSE(read_back.empty()) << "the config.read on the neighbour posted no result";
  EXPECT_EQ(bench::member(read_back, "outcome"), "ok") << read_back;
  EXPECT_NE(read_back.find(kNeighbourSaid), std::string::npos)
      << "the neighbour's own setting did not come back over the air: " << read_back;
  bool name_in_a_batch = false;
  for (const std::string& batch : bench.agent.server().batches) {
    if (batch.find(kStrangerName) != std::string::npos) name_in_a_batch = true;
  }
  EXPECT_TRUE(name_in_a_batch) << "no advert with a name reached the server: R9 held nothing";

  // -- and the places it could have been left --------------------------------------------------
  const std::string dumped = bench.agent.v().runLine("dump");
  const std::string status = bench.agent.v().runLine("status");
  const std::string wake = bench.agent.v().runLine("wake");
  const std::string jobs = bench.agent.v().runLine("agent jobs");
  const std::string state = bench.agent.v().runLine("agent state");
  const std::string obs = bench.agent.v().runLine("agent obs");
  searchAll(dumped, "`dump` (the console's 4 kB ring)");
  searchAll(status, "`status`");
  searchAll(wake, "`wake` (the last ten wakes)");
  searchAll(jobs, "`agent jobs`");
  searchAll(state, "`agent state`");
  searchAll(obs, "`agent obs`");
  transcript += dumped + status + wake + jobs + state + obs;
  searchAll(transcript, "the console's whole transcript");
  searchAll(bench.agent.world.agent->log, "the library's own log");
  searchAll(flashBytes(bench.agent.world.journal_flash), "the journal partition's bytes");
  const std::vector<std::uint8_t> retained = bench.agent.world.retained.allBytes();
  searchAll(std::string(reinterpret_cast<const char*>(retained.data()), retained.size()),
            "the retained records' bytes");
  // The kept image's window: a result waiting for a server, so what the mesh said may be in it -
  // a password and the token may not.
  searchSecrets(flashBytes(bench.agent.world.agent->kept_flash), "the kept image's flash window");

  // R17, the token's one route: in no path and no body of any request, and in no header but
  // `authorization`.
  EXPECT_FALSE(bench.agent.server().anyRequestCarries(kToken));
  EXPECT_FALSE(bench.agent.server().anyRequestCarries(kTokenBody));
  EXPECT_FALSE(bench.agent.server().anyRequestCarries(kNeighbourPw));
  EXPECT_FALSE(bench.agent.server().anyRequestCarries(kNewPw));
  int in_authorization = 0;
  int elsewhere = 0;
  bench.agent.server().tokenHeaders(kToken, &in_authorization, &elsewhere);
  EXPECT_GT(in_authorization, 10) << "the token reached the server in no request at all";
  EXPECT_EQ(elsewhere, 0);

  std::printf(
      "the agent's month: %u wakes, all slept; %u with a packet on the air; %u sends, every one a "
      "client_tx; %u adverts heard; %u library log lines; %u results kept over the week away; "
      "%u offer refused at check 3, %u apply rolled back - and none of the %u secrets or the %u "
      "things the mesh said written down anywhere\n",
      wakes, mesh_wakes, sends, adverts_pushed, bench.agent.world.agent->log_lines, kept_results,
      offers_refused, rolled_back, static_cast<unsigned>(sizeof kSecrets / sizeof kSecrets[0]),
      static_cast<unsigned>(sizeof kMeshSaid / sizeof kMeshSaid[0]));
}

// R15, the other half: the password is read back out of every buffer it crossed, inside a wake of
// the month's own world (task p4-02's test does it at the library's seam; this does it through a
// whole wake, which is where the console, the journal and the retained records are).
TEST(RailAgent, APasswordIsZeroInEveryBufferItCrossedAfterAWholeWake) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench.rak().console = bench::ownConsole;
  bench.repeater().setAdminPassword(kNeighbourPw);
  bench.repeater().grant(bench::kOwnKeyHex, mm::kPermAdmin);
  bench.agent.server().queue(neighbourPasswordSet(3001).json());
  const Pass& pass = bench.agent.oneWake();
  ASSERT_EQ(pass.ending, Ending::kSlept);
  ASSERT_EQ(bench.repeater().adminPassword(), std::string(kNewPw)) << "the job did not reach the far side";

  // The buffers this end copied it into, in the order it crossed them.
  EXPECT_FALSE(holds(&bench.agent.half().mesh, sizeof(agent::LinkMesh), kNeighbourPw)) << "LinkMesh";
  EXPECT_FALSE(holds(&bench.agent.half().mesh, sizeof(agent::LinkMesh), kNewPw)) << "LinkMesh";
  EXPECT_FALSE(holds(&bench.agent.v().link, sizeof(core::link::client::Client), kNeighbourPw)) << "the link client";
  EXPECT_FALSE(holds(&bench.agent.v().link, sizeof(core::link::client::Client), kNewPw)) << "the link client";
  EXPECT_FALSE(holds(bench.rak().passwordBuffer(), bench.rak().passwordBufferSize(), kNeighbourPw)) << "the RAK";
  // And it crossed the wire exactly once each: the SEND_LOGIN's payload and the sensitive
  // SEND_CLI's, and no other frame.
  int in_login = 0;
  int in_cli = 0;
  int anywhere_else = 0;
  for (const sim::ScriptedRadio::Seen& one : bench.rak().radio.requests) {
    const std::string payload(one.payload.begin(), one.payload.end());
    const bool login = payload.find(kNeighbourPw) != std::string::npos;
    const bool cli = payload.find(kNewPw) != std::string::npos;
    if (login) {
      if (one.type == core::link::type::kSendLogin) ++in_login;
      else ++anywhere_else;
    }
    if (cli) {
      if (one.type == core::link::type::kSendCli) ++in_cli;
      else ++anywhere_else;
    }
  }
  EXPECT_EQ(in_login, 1);
  EXPECT_EQ(in_cli, 1);
  EXPECT_EQ(anywhere_else, 0);
  // Nothing wrote either down: the library's log, the ring, the journal, the retained records.
  searchSecrets(bench.agent.world.agent->log, "the library's own log");
  searchSecrets(bench.agent.v().runLine("dump"), "`dump`");
  searchSecrets(flashBytes(bench.agent.world.journal_flash), "the journal partition's bytes");
  const std::vector<std::uint8_t> retained = bench.agent.world.retained.allBytes();
  searchSecrets(std::string(reinterpret_cast<const char*>(retained.data()), retained.size()),
                "the retained records' bytes");
}

}  // namespace
