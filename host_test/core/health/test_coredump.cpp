// The `coredump` partition, read on the host over the real thing (task p11-03):
// `tasks/phase-11/evidence/coredump-0.1.3-ota-trial.bin`, the 64 KB partition of LTE Exit Node 1
// as it was read off the board on 9 November 2026 after v0.1.3's trial was rolled back.
//
// Why the fixture and not a dump this test builds: the reader's whole job is to agree with
// ESP-IDF v5.5.5's panic handler about a byte layout nobody here controls - the flash header, the
// ELF, four kinds of note, and `pcTaskName`'s offset in a FreeRTOS TCB. A dump this test wrote
// would agree with the reader by construction and prove nothing. So the one that killed a unit in
// service is the one it is held to, and the three ways a dump can be no good are made out of it.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/health/coredump.h"
#include "core/image/descriptor.h"
#include "fixtures/coredump_fixture.h"
#include "sim/sim_flash.h"

namespace {

using core::health::CoreDump;
using core::health::Dump;
using core::health::DumpState;

// A partition with the fixture in it, and one that is erased.
struct Partition {
  sim::SimFlash flash{fixtures::kCoreDumpPartitionBytes};
  CoreDump reader{flash};

  Dump read() {
    Dump d;
    (void)reader.read(&d);
    return d;
  }
};

}  // namespace

TEST_CASE("the real dump of 9 November: its header, its checksum, its panic line, its tasks") {
  Partition p;
  fixtures::putCoreDumpFixture(p.flash);
  Dump d;
  REQUIRE(p.reader.read(&d));
  CHECK(d.state == DumpState::kPresent);
  CHECK(d.bytes == fixtures::kFixtureDumpBytes);
  CHECK(d.version == fixtures::kFixtureVersion);
  // 0x00090102: chip id 9 (the ESP32-S3), format 1 (ELF), minor 2 (a CRC-32 checksum).
  CHECK(d.chip == 9);
  CHECK(d.format == core::health::kDumpFormatElf);
  CHECK(d.minor == core::health::kDumpFormatCrc32Minor);

  // The panic line, whole: ESP-IDF could not create the main task.
  CHECK(std::string(d.panic) == fixtures::kFixturePanic);
  CHECK_FALSE(d.panic_cut);

  // Which image wrote it. Nine hex characters is all a default build's
  // CONFIG_APP_RETRIEVE_LEN_ELF_SHA puts in the note, and the release build's begins 5c1cb8f37.
  CHECK(std::string(d.elf_sha256) == fixtures::kFixtureElfSha);

  // Two tasks, named out of their TCBs, and the one that crashed named by ESP_EXTRA_INFO.
  CHECK(d.tasks == 2);
  CHECK(d.named == 2);
  CHECK(std::string(d.names[0]) == fixtures::kFixtureCrashedTask);
  CHECK(std::string(d.names[1]) == fixtures::kFixtureOtherTask);
  CHECK(d.crashed == 0);
  CHECK(std::string(d.crashedName()) == fixtures::kFixtureCrashedTask);
  // Nothing of this firmware had run: ipc0 and ipc1 are ESP-IDF's own, made before app_main.
  CHECK(std::string(d.names[0]) != "main");
}

TEST_CASE("an erased partition is not a dump, and nothing is read out of it") {
  Partition p;  // a SimFlash is 0xFF everywhere, as a factory flash leaves a partition
  Dump d;
  CHECK_FALSE(p.reader.read(&d));
  CHECK(d.state == DumpState::kErased);
  CHECK(d.bytes == 0);
  CHECK(d.panic[0] == 0);
  CHECK(d.tasks == 0);
  CHECK(d.crashed == -1);
  CHECK(std::string(core::health::describe(d.state)).find("erased") != std::string::npos);
}

TEST_CASE("one byte of the dump changed: the checksum does not hold and nothing is read out") {
  Partition p;
  std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
  // A byte inside the panic note, so that the reader would have had something to say.
  const std::uint32_t at = 3140;
  REQUIRE(bytes[at] != 0);
  bytes[at] = 0;  // a write that only clears bits, which is what NOR flash can do by itself
  fixtures::putDump(p.flash, bytes);
  Dump d;
  CHECK_FALSE(p.reader.read(&d));
  CHECK(d.state == DumpState::kBadChecksum);
  // The header was read - it is what said how long to sum - and nothing after it was trusted.
  CHECK(d.bytes == fixtures::kFixtureDumpBytes);
  CHECK(d.panic[0] == 0);
  CHECK(d.elf_sha256[0] == 0);
  CHECK(d.tasks == 0);
}

TEST_CASE("a truncated dump: the header says more than the partition holds") {
  // The dump's own length raised past the partition's size, which is what a dump cut short by a
  // partition smaller than the one it was written in would look like.
  sim::SimFlash flash{2048, 2048};
  CoreDump reader{flash};
  std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
  bytes.resize(2048);
  REQUIRE(flash.write(0, bytes.data(), 2048));
  Dump d;
  CHECK_FALSE(reader.read(&d));
  CHECK(d.state == DumpState::kTruncated);
  CHECK(d.bytes == fixtures::kFixtureDumpBytes);
  CHECK(d.panic[0] == 0);
  CHECK(std::string(core::health::describe(d.state)).find("truncated") != std::string::npos);
}

TEST_CASE("a dump cut off where the panic handler was interrupted: the checksum is what catches it") {
  Partition p;
  std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
  // The power goes with the ELF half written: the header is there (the writer puts it first), the
  // bytes after the cut are still erased. This is the case the checksum exists for.
  for (std::size_t i = 1600; i < bytes.size(); ++i) bytes[i] = 0xFF;
  fixtures::putDump(p.flash, bytes);
  Dump d;
  CHECK_FALSE(p.reader.read(&d));
  CHECK(d.state == DumpState::kBadChecksum);
  CHECK(d.panic[0] == 0);
}

TEST_CASE("a header this firmware does not know: a binary dump, and a length that cannot be") {
  SUBCASE("a format other than ELF-with-CRC32 is read as nothing") {
    Partition p;
    std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
    bytes[4] = 3;  // COREDUMP_VERSION_BIN_CURRENT's minor: a binary dump, not this build's
    fixtures::putDump(p.flash, bytes);
    Dump d;
    CHECK_FALSE(p.reader.read(&d));
    CHECK(d.state == DumpState::kNotADump);
    CHECK(d.panic[0] == 0);
  }
  SUBCASE("a length too short to hold an ELF is read as nothing") {
    Partition p;
    std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
    bytes[0] = 40;
    bytes[1] = 0;
    fixtures::putDump(p.flash, bytes);
    Dump d;
    CHECK_FALSE(p.reader.read(&d));
    CHECK(d.state == DumpState::kNotADump);
  }
  SUBCASE("a partition too small to hold a header at all") {
    sim::SimFlash flash{0};
    CoreDump reader{flash};
    Dump d;
    CHECK_FALSE(reader.read(&d));
    CHECK(d.state == DumpState::kNoRegion);
  }
}

TEST_CASE("the ELF inside is checked, not trusted: a broken ELF header reads as a dump with nothing in it") {
  // The checksum is recomputed after the change, so what is tested is the ELF reading and not the
  // checksum: a dump whose checksum holds but whose ELF is not one says nothing rather than
  // reading rubbish out of it.
  Partition p;
  std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
  bytes[core::health::kDumpHeaderBytes + 1] = 'X';  // the ELF magic's second byte
  const std::uint32_t length = fixtures::kFixtureDumpBytes - 4;
  const std::uint32_t crc = core::image::crc32(bytes.data(), length);
  for (int i = 0; i < 4; ++i) bytes[length + i] = static_cast<std::uint8_t>(crc >> (8 * i));
  fixtures::putDump(p.flash, bytes);
  Dump d;
  CHECK(p.reader.read(&d));  // the checksum holds, so there *is* a dump
  CHECK(d.state == DumpState::kPresent);
  CHECK(d.panic[0] == 0);
  CHECK(d.tasks == 0);
  CHECK(d.crashed == -1);
}

TEST_CASE("the checksum this reader computes is the CRC-32 the descriptor's own is") {
  // core/image/descriptor.h's crc32() is the one-shot of the same polynomial; this reader sums a
  // 64 KB partition through a 128-byte window, so the two must agree over the fixture's bytes.
  const std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
  const std::uint32_t length = fixtures::kFixtureDumpBytes - 4;
  const std::uint32_t want = static_cast<std::uint32_t>(bytes[length]) |
                             (static_cast<std::uint32_t>(bytes[length + 1]) << 8) |
                             (static_cast<std::uint32_t>(bytes[length + 2]) << 16) |
                             (static_cast<std::uint32_t>(bytes[length + 3]) << 24);
  CHECK(core::image::crc32(bytes.data(), length) == want);
}

TEST_CASE("elfShaMatches: a prefix of the hex, an odd number of characters, and everything that is not") {
  // 5c1cb8f37 against bytes 0x5c 0x1c 0xb8 0xf3 0x7.: nine characters, so the last compares one
  // nibble. The tenth character decides nothing and the byte's low nibble may be anything.
  std::uint8_t sha[32] = {};
  sha[0] = 0x5c;
  sha[1] = 0x1c;
  sha[2] = 0xb8;
  sha[3] = 0xf3;
  sha[4] = 0x7a;
  CHECK(core::health::elfShaMatches(sha, "5c1cb8f37"));
  CHECK(core::health::elfShaMatches(sha, "5C1CB8F37"));  // either case
  CHECK(core::health::elfShaMatches(sha, "5c1cb8f37a"));
  CHECK(core::health::elfShaMatches(sha, "5"));
  CHECK_FALSE(core::health::elfShaMatches(sha, "5c1cb8f38"));
  CHECK_FALSE(core::health::elfShaMatches(sha, "5c1cb8f37b"));
  // "the dump says nothing" is never "the dump matches".
  CHECK_FALSE(core::health::elfShaMatches(sha, ""));
  CHECK_FALSE(core::health::elfShaMatches(sha, nullptr));
  CHECK_FALSE(core::health::elfShaMatches(sha, "5c1cb8f3g"));  // not hex
  // Longer than a SHA-256 is a prefix of nothing.
  std::string too_long(65, '0');
  CHECK_FALSE(core::health::elfShaMatches(sha, too_long.c_str()));
}

TEST_CASE("erase: the partition is erased, and a dump that was there is gone") {
  Partition p;
  fixtures::putCoreDumpFixture(p.flash);
  Dump d;
  REQUIRE(p.reader.read(&d));
  CHECK(p.reader.erase());
  CHECK_FALSE(p.reader.read(&d));
  CHECK(d.state == DumpState::kErased);
  // Every sector of it, not just the header's: 64 KB of 4 KB sectors.
  CHECK(p.flash.erasesOfSector(15) == 1);
}

TEST_CASE("a flash that cannot be read says so and reads nothing") {
  Partition p;
  fixtures::putCoreDumpFixture(p.flash);
  p.flash.powerOff();
  Dump d;
  CHECK_FALSE(p.reader.read(&d));
  CHECK(d.state == DumpState::kUnreadable);
  CHECK(d.panic[0] == 0);
}
