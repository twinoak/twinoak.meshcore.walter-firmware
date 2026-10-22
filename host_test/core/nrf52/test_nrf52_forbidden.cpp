// What does not exist (task p1-04, S-5): no code under components/ refers to NVMC.ERASEALL,
// NVMC.ERASEUICR, the control port's ERASEALL or UICR's registers except the header that names
// them - a grep over the tree, shown failing on purpose on a snippet - and the simulator's
// tripwire, shown firing once for each of the four, with nothing erased. Since task p5-02
// (S-5a) one more file may name them, the door, core/nrf52/provision_door.cpp - an exact list
// of two, as tools/check_rails.py R3 has it - and the door's header is included by
// core/provision/ and nothing else (R3b), which the last test here reads off the tree as well.
#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sim/nrf52_bench.h"

namespace fs = std::filesystem;
namespace regs = core::nrf52;

namespace {

// The names regs.h gives the registers nothing may write. A reference anywhere else under
// components/ is a hit, whatever it is doing with it: there is no legitimate reader of them
// outside the simulator and the tests.
const char* const kForbidden[] = {
    "kEraseAll",           // nvmc::kEraseAll and ctrl_ap::kEraseAll
    "kEraseUicr",          // nvmc::kEraseUicr
    "kErasePagePartial",   // not a chip erase, but not this firmware's either
    "uicr::",              // every UICR address
    "kEraseAllStatus",     // only meaningful after an ERASEALL
};

struct Hit {
  std::string file;
  int line;
  std::string text;
};

std::vector<Hit> forbiddenReferences(const std::string& source, const std::string& name) {
  std::vector<Hit> hits;
  std::istringstream in(source);
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    number++;
    for (const char* word : kForbidden) {
      if (line.find(word) != std::string::npos) {
        hits.push_back(Hit{name, number, line});
        break;
      }
    }
  }
  return hits;
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

TEST_CASE("no file under components/ but regs.h and the door names a forbidden register") {
  const fs::path root = fs::path(TWINOAK_REPO_ROOT) / "components";
  const fs::path header = root / "core" / "include" / "core" / "nrf52" / "regs.h";
  const fs::path door = root / "core" / "nrf52" / "provision_door.cpp";
  REQUIRE(fs::exists(header));
  REQUIRE(fs::exists(door));
  std::vector<Hit> hits;
  unsigned files = 0;
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    const std::string ext = e.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".c") continue;
    if (fs::equivalent(e.path(), header) || fs::equivalent(e.path(), door)) continue;
    files++;
    const std::vector<Hit> found = forbiddenReferences(readFile(e.path()), e.path().generic_string());
    hits.insert(hits.end(), found.begin(), found.end());
  }
  CHECK(files > 40);
  for (const Hit& h : hits) {
    FAIL_CHECK("a forbidden register is named outside regs.h and provision_door.cpp: " << h.file << ":" << h.line
                                                                                      << ": " << h.text);
  }
  CHECK(hits.empty());
  // And the header itself does name them - the grep is looking for the right words - and the
  // door names the two it is for (the CTRL-AP's ERASEALL, UICR) and never the NVMC's erases.
  const std::vector<Hit> in_header = forbiddenReferences(readFile(header), "regs.h");
  CHECK(in_header.size() >= 5);
  const std::string door_text = readFile(door);
  CHECK(door_text.find("ctrl_ap::kEraseAll") != std::string::npos);
  CHECK(door_text.find("uicr::kApprotect") != std::string::npos);
  CHECK(door_text.find("nvmc::kEraseAll") == std::string::npos);
  CHECK(door_text.find("nvmc::kEraseUicr") == std::string::npos);
  CHECK(door_text.find("kErasePagePartial") == std::string::npos);
  MESSAGE(files << " source files under components/ read; " << hits.size()
                << " forbidden references outside regs.h and provision_door.cpp; " << in_header.size()
                << " lines of regs.h name them");
}

TEST_CASE("R3b: the door's header is included by core/provision/ and nothing else; the grant's by it and console.cpp") {
  const fs::path root = fs::path(TWINOAK_REPO_ROOT);
  const fs::path components = root / "components";
  const std::string door_source = "components/core/nrf52/provision_door.cpp";
  const std::string door_header = "components/core/include/core/nrf52/provision_door.h";
  std::vector<std::string> door_users, grant_users;
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(components)) {
    if (!e.is_regular_file()) continue;
    const std::string ext = e.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".c") continue;
    const std::string text = readFile(e.path());
    const std::string rel = fs::relative(e.path(), root).generic_string();
    if (text.find("core/nrf52/provision_door.h\"") != std::string::npos) door_users.push_back(rel);
    if (text.find("core/nrf52/provision_grant.h\"") != std::string::npos) grant_users.push_back(rel);
  }
  // The grep matches something: the door's own source includes both.
  CHECK(std::find(door_users.begin(), door_users.end(), door_source) != door_users.end());
  CHECK(std::find(grant_users.begin(), grant_users.end(), door_source) != grant_users.end());
  // The sequence's sources, and its headers where every header of core/ lives.
  const auto in_sequence = [](const std::string& p) {
    return p.rfind("components/core/provision/", 0) == 0 || p.rfind("components/core/include/core/provision/", 0) == 0;
  };
  for (const std::string& p : door_users) {
    const bool allowed = p == door_source || in_sequence(p);
    if (!allowed) FAIL_CHECK("the door's header is included outside core/provision/: " << p);
  }
  for (const std::string& p : grant_users) {
    const bool allowed = p == door_source || p == door_header || p == "components/core/console/console.cpp" ||
                         in_sequence(p);
    if (!allowed) FAIL_CHECK("the grant's header is included outside core/provision/ and console.cpp: " << p);
  }
  // The directories S-5a names, by name - whether they exist yet or not.
  const char* const never[] = {"supervisor", "update", "flasher", "cycle", "agent"};
  for (const char* d : never) {
    const std::string prefix = "components/core/" + std::string(d) + "/";
    for (const std::string& p : door_users) CHECK(p.rfind(prefix, 0) != 0);
    for (const std::string& p : grant_users) CHECK(p.rfind(prefix, 0) != 0);
  }
  MESSAGE("provision_door.h is included by " << door_users.size() << " file(s), provision_grant.h by "
                                             << grant_users.size() << "; core/provision/ "
                                             << (fs::is_directory(components / "core" / "provision") ? "exists" : "does not exist yet (task p5-03)"));
}

TEST_CASE("the grep fails on purpose: a snippet that writes ERASEALL, and one that touches UICR") {
  const std::string guilty =
      "Result Nrf52::openProtected() {\n"
      "  return fromMem(mem_ap_.write32(ctrl_ap::kEraseAll, 1));\n"
      "}\n"
      "Result Nrf52::keepOpen() {\n"
      "  return fromMem(mem_ap_.write32(uicr::kApprotect, uicr::kApprotectHwDisabled));\n"
      "}\n";
  const std::vector<Hit> hits = forbiddenReferences(guilty, "a-snippet.cpp");
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].line == 2);
  CHECK(hits[0].text.find("kEraseAll") != std::string::npos);
  CHECK(hits[1].line == 5);
  CHECK(hits[1].text.find("uicr::") != std::string::npos);
  MESSAGE("the grep would say: a forbidden register is named outside regs.h: " << hits[0].file << ":" << hits[0].line
                                                                              << ": " << hits[0].text);
  // The innocent shape: the same operation through the guarded API names no register.
  const std::string innocent = "  return nrf.erasePage(region, page);\n  return nrf.writeWords(region, addr, data, n);\n";
  CHECK(forbiddenReferences(innocent, "ok.cpp").empty());
}

TEST_CASE("the simulator's tripwire fires once for each forbidden write, and nothing is erased") {
  sim::Nrf52Bench b;
  b.chip.strict = false;  // the sentences are what this test is about; strict, each is a failed test
  REQUIRE(b.open().ok());
  b.chip.fillFlash(0x26000, 0x27000, 0x12345678);
  const std::uint32_t approtect_before = b.chip.uicrWord(regs::uicr::kApprotect);
  core::swd::MemAp& mem = b.nrf.memAp();

  // 1. NVMC.ERASEALL, as a host that "just needed the chip clean" would write it.
  CHECK(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigEen).ok());
  CHECK(mem.write32(regs::nvmc::kEraseAll, 1).ok());
  CHECK(b.chip.counters().tripwire == 1);
  REQUIRE(b.chip.errors().size() == 1);
  MESSAGE("tripwire 1, the simulator said: " << b.chip.errors()[0]);
  CHECK(b.chip.countErrors("NVMC.ERASEALL written") == 1);

  // 2. NVMC.ERASEUICR.
  CHECK(mem.write32(regs::nvmc::kEraseUicr, 1).ok());
  CHECK(b.chip.counters().tripwire == 2);
  REQUIRE(b.chip.errors().size() == 2);
  MESSAGE("tripwire 2, the simulator said: " << b.chip.errors()[1]);
  CHECK(b.chip.countErrors("NVMC.ERASEUICR written") == 1);
  CHECK(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigRen).ok());

  // 3. The control access port's ERASEALL, the way a protected chip is opened on a bench.
  CHECK(b.dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kEraseAll, 1).ok());
  CHECK(b.chip.counters().tripwire == 3);
  REQUIRE(b.chip.errors().size() == 3);
  MESSAGE("tripwire 3, the simulator said: " << b.chip.errors()[2]);
  CHECK(b.chip.countErrors("CTRL-AP ERASEALL written") == 1);

  // 4. A UICR word - APPROTECT, the one a well-meaning host might write to keep the port open.
  CHECK(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigWen).ok());
  CHECK(mem.write32(regs::uicr::kApprotect, regs::uicr::kApprotectHwDisabled).ok());
  CHECK(b.chip.counters().tripwire == 4);
  CHECK(b.chip.counters().uicr_writes == 1);
  REQUIRE(b.chip.errors().size() == 4);
  MESSAGE("tripwire 4, the simulator said: " << b.chip.errors()[3]);
  CHECK(b.chip.countErrors("a write to UICR") == 1);
  CHECK(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigRen).ok());

  // Nothing happened to the chip on any of the four.
  CHECK(b.chip.flashWord(0x26000) == 0x12345678);
  CHECK(b.chip.flashWord(0x26FFC) == 0x12345678);
  CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == approtect_before);
  CHECK(b.chip.counters().erases == 0);
  CHECK(b.chip.counters().writes == 0);
  // A zero written to ERASEALL is not an erase and not a trip (the register's "NoOperation").
  CHECK(mem.write32(regs::nvmc::kEraseAll, 0).ok());
  CHECK(b.chip.counters().tripwire == 4);
  b.chip.clearErrors();
  b.close();
}

TEST_CASE("the nRF52 layer has no way to reach them: every write it can make is a flash page or word inside a region, or a debug register") {
  // By construction - the API is erasePage(region, page), writeWords(region, addr, ...), halt(),
  // resetAndHalt(), run(), systemReset() - and by the test above. Here: the whole API, exercised,
  // never trips the wire.
  sim::Nrf52Bench b;
  // This test is about the flash controller's whole reach, not about one board's region, so the
  // region it declares is the whole array (rail R4, task p3-06: a chip flashed by a test that
  // declared none fails that test, and "the whole array" is a declaration a test about the
  // controller may honestly make).
  b.chip.onlyTouch(0, b.chip.flashBytes());
  REQUIRE(b.open().ok());
  const core::nrf52::FlashRegion all{0, 0x100000};
  const std::uint32_t data[2] = {0, 0};
  CHECK(b.nrf.erasePage(all, 0xFF000).ok());  // the last page of flash: still flash, not UICR
  CHECK(b.nrf.writeWords(all, 0xFFFF8, data, 2).ok());
  CHECK(b.nrf.halt().ok());
  CHECK(b.nrf.resetAndHalt().ok());
  CHECK(b.nrf.run().ok());
  CHECK(b.nrf.systemReset().ok());
  CHECK(b.chip.counters().tripwire == 0);
  CHECK(b.chip.counters().uicr_writes == 0);
  CHECK(b.chip.errors().empty());
  b.close();
}
