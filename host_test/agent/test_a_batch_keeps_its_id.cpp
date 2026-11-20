// A batch keeps its id across a sleep, on this board (task p15-04).
//
// The library's own suite holds the rule (`host_test/agent-core/test/test_loop/test_loop.cpp`,
// `ABatchWhoseAnswerWasLostGoesOutAgainUnderItsIdAfterASleep`): a batch that reached the server
// and whose answer was lost must go out again under the id it was built with, because the server
// de-duplicates on that id. This file is the same thing **on this board's bench** - the real
// backend, the real library, the §7 drain out of a simulated RAK, the kept image in a simulated
// flash window, the loop's state in a simulated NVS, and a wake that really deep-sleeps and
// really boots again - because that is where the fault was: the adverts crossed the sleep in the
// image and the id did not, so every wake posted everything it held as a new batch and
// meshmaster.dk stored it again. On 16 November 2026 this unit sent batches of 13, 14, 15, 16, 17
// and 18 adverts in six wakes running, and the hundred newest rows the server held from it were
// 19 different adverts.
//
// And the half of it that is this board's and not the library's: **since which wake** a batch has
// been waiting, which `agent obs` and the report's `observations` section say
// (walter_agent/pending_batch.h). The library has no notion of a wake and its image's layout is
// not edited here, so that one figure is a small value of the settings store, written only when
// the answer changes.
#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <agent/KeptImage.h>

#include "agent/agent_bench.h"
#include "sim/quiet_repeater.h"
#include "walter_agent/pending_batch.h"
#include "walter_agent/region_store.h"

using bench::AgentBench;
using core::cycle::Ending;
using core::cycle::Pass;

namespace {

bool has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// One advert the radio heard, as §7 hands it over: MeshCore's payload with flags `0x80` (a name)
// and `0x02` (a repeater), then the name's bytes - the shape
// `host_test/rails/test_rail_agent_month.cpp` builds one in.
sim::SimRak::Advert heard(std::uint8_t first_key_byte, const char* name) {
  sim::SimRak::Advert a;
  a.pubkey.assign(32, first_key_byte);
  a.age_s = 30;
  a.snr_q4 = 21;
  a.rssi = -91;
  a.path_len = 0;
  a.app_data.push_back(0x82);
  for (const char* c = name; *c != 0; ++c) a.app_data.push_back(static_cast<std::uint8_t>(*c));
  return a;
}

// How many adverts a batch body carries: one `publicKey` each, as the server's own reader counts
// them (host_test/agent/meshmaster_script.h).
int itemsIn(const std::string& batch) {
  int n = 0;
  std::size_t at = 0;
  while ((at = batch.find("\"publicKey\":", at)) != std::string::npos) {
    ++n;
    at += 12;
  }
  return n;
}

// Wake until the server has been posted `wanted` observation batches, or `at_most` wakes have
// gone by: a failed post leaves a backoff behind it, and how many sleeps that takes is the
// library's arithmetic and not this test's business to pin.
std::size_t wakeUntilBatches(AgentBench& bench, std::size_t wanted, int at_most = 8) {
  for (int i = 0; i < at_most && bench.server().batches.size() < wanted; ++i) {
    const Pass& pass = bench.oneWake();
    CHECK(pass.ending == Ending::kSlept);
  }
  return bench.server().batches.size();
}

}  // namespace

// -- the batch itself ----------------------------------------------------------------------------

TEST_CASE("a batch the server took and whose answer was lost goes out again under its own id after a sleep") {
  sim::QuietRepeater repeater;
  AgentBench bench(&repeater, false);
  bench.modemWorks();
  for (std::uint8_t i = 0; i < 5; ++i) {
    bench.world.simrak->observations.push_back(heard(static_cast<std::uint8_t>(0xA0 + i), "Risskov Garden"));
  }
  // The post reaches the server, which stores it; the answer never reaches this board, which is
  // the failure of 16 November 2026 as the unit met it.
  bench.server().loseTheAnswerToObservations(1);

  REQUIRE(wakeUntilBatches(bench, 1) == 1);
  const std::string id = bench::member(bench.server().batches[0], "clientId");
  REQUIRE_FALSE(id.empty());
  CHECK(itemsIn(bench.server().batches[0]) == 5);
  // Nothing is consumed until the server says it has them, and the batch is the loop's to keep.
  CHECK(bench.half().loop.pendingBatchCount() == 5u);
  CHECK(std::string(bench.half().loop.pendingBatchId()) == id);
  CHECK(bench.half().loop.ring().size() == 5u);
  // And the image a sleep keeps holds the id, which is the whole of what a sleep used to lose.
  CHECK(bench.half().backend.counters().kept_written >= 1u);
  std::vector<std::uint8_t> image(agent::kKeptScratchBytes, 0);
  const std::size_t read = bench.half().kept.read(image.data(), image.size());
  REQUIRE(read != 0u);
  mm::KeptImageReader reader;
  REQUIRE(reader.check(image.data(), read) == nullptr);
  CHECK(reader.version() == mm::kKeptImageVersion);
  CHECK(reader.advertCount() == 5u);
  CHECK(reader.batchCount() == 5u);
  CHECK(std::string(reader.batchId()) == id);

  // Three more adverts heard while the board slept: they are not the batch, and the batch is
  // still the ring's five oldest.
  for (std::uint8_t i = 0; i < 3; ++i) {
    bench.world.simrak->observations.push_back(heard(static_cast<std::uint8_t>(0xB0 + i), "Grenaa Chimney"));
  }

  // The sleep, and the wakes after it. The first post of the next wake that reaches the server is
  // that batch under that id.
  REQUIRE(wakeUntilBatches(bench, 2) >= 2u);
  const std::string again = bench.server().batches[1];
  CHECK(bench::member(again, "clientId") == id);
  CHECK(itemsIn(again) == 5);
  // The server answers *had it already*: the five leave the ring and nothing of them is stored
  // twice. What was heard since goes out in the same wake, as a batch of its own under a new id.
  REQUIRE(bench.server().batches.size() == 3u);
  CHECK(bench::member(bench.server().batches[2], "clientId") != id);
  CHECK(itemsIn(bench.server().batches[2]) == 3);
  CHECK(bench.half().loop.pendingBatchCount() == 0u);
  CHECK(std::string(bench.half().loop.pendingBatchId()).empty());
  CHECK(bench.half().loop.ring().empty());
  CHECK(bench.world.agent->log.find("the server had this batch already") != std::string::npos);

  // And the figure the whole task is about: a server that de-duplicates on the id stored eight
  // adverts for the eight this unit heard, where before the bump it stored thirteen.
  CHECK(bench.server().seenBatches().size() == 2u);
  CHECK(bench.server().storedAdverts() == 8u);
}

TEST_CASE("a batch whose answer is lost twice is still that batch, and costs the server one row each") {
  sim::QuietRepeater repeater;
  AgentBench bench(&repeater, false);
  bench.modemWorks();
  for (std::uint8_t i = 0; i < 4; ++i) {
    bench.world.simrak->observations.push_back(heard(static_cast<std::uint8_t>(0xC0 + i), "Risskov Garden"));
  }
  bench.server().loseTheAnswerToObservations(2);

  REQUIRE(wakeUntilBatches(bench, 3) >= 3u);
  const std::string id = bench::member(bench.server().batches[0], "clientId");
  REQUIRE_FALSE(id.empty());
  CHECK(bench::member(bench.server().batches[1], "clientId") == id);
  CHECK(bench::member(bench.server().batches[2], "clientId") == id);
  for (std::size_t i = 0; i < 3; ++i) CHECK(itemsIn(bench.server().batches[i]) == 4);
  // Three posts, one batch, four rows: the count of adverts the server holds is the count heard.
  CHECK(bench.server().seenBatches().size() == 1u);
  CHECK(bench.server().storedAdverts() == 4u);
  CHECK(bench.half().loop.pendingBatchCount() == 0u);
  CHECK(bench.half().loop.ring().empty());
}

// -- an image written before the bump --------------------------------------------------------------

// A version 1 image, which is what the library wrote before the batch's id was kept: the same
// bytes a version 2 image with no batch pending has, with the version field at 1 and the checksum
// over them again (agent/KeptImage.h's header, which says the two are the same bytes where the
// batch's block is absent). Written the way an older firmware left it in the flash window, for
// the first boot after an update to read.
static std::vector<std::uint8_t> imageOfVersionOne(std::size_t adverts) {
  std::vector<std::uint8_t> bytes(mm::kMaxKeptImageLen, 0);
  mm::KeptImageWriter writer(bytes.data(), bytes.size());
  for (std::size_t i = 0; i < adverts; ++i) {
    mm::HeardAdvert advert;
    std::memset(advert.publicKey, static_cast<int>(0xD0 + i), sizeof advert.publicKey);
    advert.hops = 1;
    advert.advType = 2;
    std::snprintf(advert.name, sizeof advert.name, "Older Firmware %u", static_cast<unsigned>(i));
    REQUIRE(writer.addAdvert(advert, 1791936000u + static_cast<std::uint32_t>(i)));
  }
  const std::size_t len = writer.finish(0, 0, 0);
  REQUIRE(len != 0u);
  bytes.resize(len);
  // The version down to 1, and the checksum over bytes 8..len again - the one the reader checks.
  bytes[8] = static_cast<std::uint8_t>(mm::kKeptImageVersionWithoutBatch);
  bytes[9] = 0;
  const std::uint32_t crc = mm::crc32Of(bytes.data() + 8, len - 8);
  for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu);
  return bytes;
}

TEST_CASE("an image the library wrote before the bump is read at the first boot after the update, with no batch pending") {
  sim::QuietRepeater repeater;
  AgentBench bench(&repeater, false);
  bench.modemWorks();

  // What the old firmware left in the window, written through the same store this firmware reads
  // it with - the window the agent's half opens, at the same offset and length.
  const std::vector<std::uint8_t> old = imageOfVersionOne(2);
  {
    agent::RegionStore store(bench.world.agent->kept_flash, 0, bench.world.agent->kept_flash.size());
    REQUIRE(store.usable());
    REQUIRE(store.write(old.data(), old.size()));
  }

  // The first boot of the new firmware over it: the two adverts come back and no batch is
  // pending, which is what version 1 means to a reader that knows about version 2.
  bench.boot();
  CHECK(bench.half().backend.counters().kept_restored == 1u);
  CHECK(bench.half().loop.ring().size() == 2u);
  CHECK(bench.half().loop.pendingBatchCount() == 0u);
  CHECK(std::string(bench.half().loop.pendingBatchId()).empty());
  CHECK_FALSE(bench.half().batch.waiting());
  CHECK(bench.half().loop.keptImagesRefused() == 0u);

  // And nothing of a waiting batch is printed, because none waits.
  const std::string said = bench.v().runLine("agent obs");
  CHECK_FALSE(has(said, "a batch is waiting"));
  // The two restored adverts go out in the next wake, as any advert the ring holds does.
  bench.v().begin();
  (void)bench.v().run();
  REQUIRE(bench.server().batches.size() == 1u);
  CHECK(itemsIn(bench.server().batches[0]) == 2);
}

// -- what a person reads ---------------------------------------------------------------------------

TEST_CASE("`agent obs` names a batch waiting for its answer and says since which wake") {
  sim::QuietRepeater repeater;
  AgentBench bench(&repeater, false);
  bench.modemWorks();
  for (std::uint8_t i = 0; i < 5; ++i) {
    bench.world.simrak->observations.push_back(heard(static_cast<std::uint8_t>(0xA0 + i), "Risskov Garden"));
  }
  bench.server().loseTheAnswerToObservations(3);

  REQUIRE(wakeUntilBatches(bench, 1) == 1u);
  const std::string id = bench::member(bench.server().batches[0], "clientId");
  const std::uint32_t at = bench.duty().wake().wakes;
  const std::string said = bench.v().runLine("agent obs");
  INFO(said);
  CHECK(has(said, "a batch is waiting for its answer: 5 advert(s) since wake " +
                      std::to_string(at) + ", as " + id));
  // The report's `observations` section is `agent obs` without its pull, so it carries the line.
  CHECK(bench.duty().wake().wakes == at);

  // After the sleep it is still the same batch and still the same wake: the wake it has been
  // waiting **since**, and not the wake that is reading it.
  REQUIRE(wakeUntilBatches(bench, 2) >= 2u);
  const std::uint32_t later = bench.duty().wake().wakes;
  REQUIRE(later > at);
  const std::string again = bench.v().runLine("agent obs");
  INFO(again);
  CHECK(has(again, "a batch is waiting for its answer: 5 advert(s) since wake " +
                       std::to_string(at) + ", as " + id));
  CHECK(bench.half().batch.sinceWake() == at);
  CHECK(bench.half().batch.survived());
}

TEST_CASE("`agent obs` says nothing of a batch when none is waiting") {
  sim::QuietRepeater repeater;
  AgentBench bench(&repeater, false);
  bench.modemWorks();
  for (std::uint8_t i = 0; i < 3; ++i) {
    bench.world.simrak->observations.push_back(heard(static_cast<std::uint8_t>(0xA0 + i), "Risskov Garden"));
  }
  REQUIRE(wakeUntilBatches(bench, 1) == 1u);
  CHECK(bench.half().loop.pendingBatchCount() == 0u);

  const std::string said = bench.v().runLine("agent obs");
  INFO(said);
  CHECK(has(said, "agent obs: 0 of 64 waiting"));
  CHECK_FALSE(has(said, "a batch is waiting"));
  // ... and the key is not in the store either: a unit whose posts are answered carries no value
  // of this at all.
  CHECK_FALSE(bench.half().batch.survived());
  CHECK(bench.half().batch.writes().writes == 0u);
}

TEST_CASE("the wake a batch has been waiting since is written once and no wake writes it again") {
  sim::QuietRepeater repeater;
  AgentBench bench(&repeater, false);
  bench.modemWorks();
  for (std::uint8_t i = 0; i < 5; ++i) {
    bench.world.simrak->observations.push_back(heard(static_cast<std::uint8_t>(0xA0 + i), "Risskov Garden"));
  }
  bench.server().loseTheAnswerToObservations(4);

  REQUIRE(wakeUntilBatches(bench, 1) == 1u);
  CHECK(bench.half().batch.writes().writes == 1u);
  // Two more wakes whose post fails under the same id: the answer this object gives has not
  // changed, so the flash is not written again.
  REQUIRE(wakeUntilBatches(bench, 3) >= 3u);
  CHECK(bench.half().batch.writes().writes == 0u);
  CHECK(bench.half().batch.survived());
  // And the wake that is answered takes the value with it.
  REQUIRE(wakeUntilBatches(bench, 5) >= 5u);
  CHECK(bench.half().loop.pendingBatchCount() == 0u);
  CHECK(bench.half().batch.writes().writes == 1u);
  CHECK(bench.v().runLine("agent obs").find("a batch is waiting") == std::string::npos);
}

TEST_CASE("a claimed twinoak.report for `observations` names the batch that is waiting") {
  sim::QuietRepeater repeater;
  AgentBench bench(&repeater, false);
  bench.modemWorks();
  for (std::uint8_t i = 0; i < 5; ++i) {
    bench.world.simrak->observations.push_back(heard(static_cast<std::uint8_t>(0xA0 + i), "Risskov Garden"));
  }
  bench.server().loseTheAnswerToObservations(2);
  REQUIRE(wakeUntilBatches(bench, 1) == 1u);
  const std::string id = bench::member(bench.server().batches[0], "clientId");
  const std::uint32_t at = bench.duty().wake().wakes;

  // The report is a job, and a job runs **before** the observations step of the same wake, so the
  // section is written while the batch is still waiting - which is the only time there is
  // anything to write.
  bench.server().queue(
      bench::ownJob(28399, agent::kReportType, "{\"sections\":[\"observations\"]}").json());
  (void)bench.oneWake();

  const std::string data = bench.lastData(28399);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(has(data, "\"name\":\"observations\""));
  CHECK(has(data, "agent obs: "));
  CHECK(has(data, "a batch is waiting for its answer: 5 advert(s) since wake " + std::to_string(at)));
  CHECK(has(data, id));
}
