// `coredump` and `coredump erase confirm` (task p11-03): what the panic handler left behind, read
// over the real dump of 9 November, and the one command in this firmware that erases one.
//
// The command exists because reading that dump took a person at the bench with
// `esptool read_flash 0xFF0000 0x10000`. What is held here is everything the console does around
// the reader (core/health/coredump.h, whose own tests are host_test/core/health/test_coredump.cpp):
// the shape of the lines, that a build handed no partition says so instead of printing zeros, that
// the read changes nothing, and that nothing erases a dump without the word.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/console/commands.h"
#include "fixtures/coredump_fixture.h"
#include "settings_console_rig.h"

using bench::hasText;
using bench::SettingsConsole;

TEST_CASE("coredump over the real dump: the header, the image, the panic line and the tasks") {
  SettingsConsole c;
  fixtures::putCoreDumpFixture(c.coredump_flash);
  const std::string out = c.run("coredump");
  CHECK(hasText(out, "coredump: a core dump whose checksum holds"));
  CHECK(hasText(out, "3204 bytes of 65536, format 1.2 (ELF, CRC-32), chip id 9"));
  CHECK(hasText(out, "the checksum holds"));
  // Which image wrote it: the nine hex characters the dump carries.
  CHECK(hasText(out, "written by the image whose ELF sha256 begins 5c1cb8f37"));
  // This rig has no self-update, so which slot that is cannot be said - and it says that rather
  // than guessing.
  CHECK(hasText(out, "which slot that is cannot be said"));
  CHECK(hasText(out, "panic: assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)"));
  CHECK(hasText(out, "2 tasks: ipc0 (crashed), ipc1"));
  CHECK(hasText(out, "nothing erases it by itself: 'coredump erase confirm'"));
  CHECK_FALSE(hasText(out, "error"));
  // The panic line is 69 characters and the console cuts a line at 200 bytes: it must come out
  // whole, newline and all, or the next line runs on.
  CHECK(hasText(out, "(res == pdTRUE)\n"));
}

TEST_CASE("an erased partition: one line, and no figures of a dump that is not there") {
  SettingsConsole c;
  const std::string out = c.run("coredump");
  CHECK(hasText(out, "coredump: the partition is erased"));
  CHECK_FALSE(hasText(out, "panic:"));
  CHECK_FALSE(hasText(out, "task"));
  CHECK_FALSE(hasText(out, "error"));
}

TEST_CASE("a dump whose checksum does not hold: the header is printed and nothing else is trusted") {
  SettingsConsole c;
  std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
  bytes[3140] = 0;
  fixtures::putDump(c.coredump_flash, bytes);
  const std::string out = c.run("coredump");
  CHECK(hasText(out, "the dump's checksum does not hold"));
  // The header is worth printing: it is what tells a dump cut short from an empty partition.
  CHECK(hasText(out, "the header says 3204 bytes, version 0x00090102"));
  CHECK(hasText(out, "the partition is 65536 bytes"));
  CHECK_FALSE(hasText(out, "assert failed"));
  CHECK_FALSE(hasText(out, "ipc0"));
}

TEST_CASE("coredump erase: the word is asked for, and with it the dump is gone") {
  SettingsConsole c;
  fixtures::putCoreDumpFixture(c.coredump_flash);
  SUBCASE("without the word nothing is erased") {
    const std::string out = c.run("coredump erase");
    CHECK(hasText(out, "error: coredump erase forgets the dump; to do it, type: coredump erase confirm"));
    CHECK(c.coredump_flash.erasesOfSector(0) == 0);
    CHECK(hasText(c.run("coredump"), "assert failed"));
  }
  SUBCASE("with it, every sector of the partition") {
    const std::string out = c.run("coredump erase confirm");
    CHECK(hasText(out, "coredump erase: erased; the next panic writes a new one"));
    CHECK(c.coredump_flash.erasesOfSector(0) == 1);
    CHECK(c.coredump_flash.erasesOfSector(15) == 1);
    CHECK(hasText(c.run("coredump"), "the partition is erased"));
  }
  SUBCASE("the word is the same one every command that could cost something asks for") {
    CHECK(std::string(core::console::kConfirmWord) == "confirm");
    CHECK(hasText(c.run("coredump erase please"), "error: coredump takes no argument, or 'erase confirm'"));
    CHECK(c.coredump_flash.erasesOfSector(0) == 0);
  }
}

TEST_CASE("coredump refuses anything else, and erases nothing while refusing") {
  SettingsConsole c;
  fixtures::putCoreDumpFixture(c.coredump_flash);
  CHECK(hasText(c.run("coredump 1024"), "error: coredump takes no argument, or 'erase confirm': 1024"));
  CHECK(hasText(c.run("coredump confirm"), "error: coredump takes no argument"));
  CHECK(c.coredump_flash.erasesOfSector(0) == 0);
}

TEST_CASE("the read only reads: no setting, no write of the partition, nothing of the radio") {
  SettingsConsole c;
  fixtures::putCoreDumpFixture(c.coredump_flash);
  const std::size_t keys = c.kv.size();
  const int writes = c.kv.writes;
  const std::size_t frames = c.rak.requests.size();
  const std::uint32_t ops = c.coredump_flash.operations();
  c.run("coredump");
  CHECK(c.kv.size() == keys);
  CHECK(c.kv.writes == writes);
  CHECK(c.rak.requests.size() == frames);
  CHECK(c.coredump_flash.operations() == ops);  // reads do not count: no write, no erase
  // And `erase confirm`, which forgets every setting, leaves the dump where it is: the partitions
  // are different ones and the document says so.
  c.run("erase confirm");
  CHECK(hasText(c.run("coredump"), "assert failed"));
}

TEST_CASE("a build that was handed no coredump partition says so in one line") {
  SettingsConsole c;
  fixtures::putCoreDumpFixture(c.coredump_flash);
  c.withoutCoreDump();
  const std::string out = c.run("coredump");
  CHECK(hasText(out, "error: this build has no coredump partition"));
  CHECK(hasText(out, "main/main.cpp"));
  CHECK_FALSE(hasText(out, "assert failed"));
  const std::string erase = c.run("coredump erase confirm");
  CHECK(hasText(erase, "error: this build has no coredump partition"));
  CHECK(c.coredump_flash.erasesOfSector(0) == 0);
}

TEST_CASE("help has the entry, under the unit, and it says what the command is for") {
  SettingsConsole c;
  const std::string out = c.run("help");
  CHECK(hasText(out, "coredump [erase confirm]"));
  CHECK(hasText(out, "the core dump the panic handler left behind"));
  // The sentence that is the point of it, as the console's own wrapping leaves it.
  CHECK(hasText(out, "'coredump erase confirm' forgets it, and nothing else"));
  CHECK(hasText(out, "in this firmware ever does"));
  CHECK(hasText(out, "the unit"));
}
