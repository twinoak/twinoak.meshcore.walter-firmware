// The real core dump of 9 November 2026 (task p11-03), read off disk rather than transcribed:
// `tasks/phase-11/evidence/coredump-0.1.3-ota-trial.bin`, the whole 64 KB `coredump` partition
// of LTE Exit Node 1 as `esptool read_flash 0xFF0000 0x10000` brought it back after v0.1.3's
// trial was rolled back (that directory's README.md is the trial).
//
// Everything in it that a test asserts on is named here, so that one place says what the fixture
// is and a test that disagrees with the file fails rather than passing against a copy:
//
//   - 3 204 bytes of dump in the partition, version 0x00090102 (chip id 9 - the ESP32-S3 -
//     format 1.2: ELF with a CRC-32), and the CRC holds;
//   - `ESP_PANIC_DETAILS`: *assert failed: esp_startup_start_app app_startup.c:86 (res ==
//     pdTRUE)* - ESP-IDF could not create the main task;
//   - `ESP_CORE_DUMP_INFO`'s `app_elf_sha256`: `5c1cb8f37`, the release build's;
//   - two tasks, `ipc0` and `ipc1`, and `ESP_EXTRA_INFO`'s `crashed_task_tcb` is `ipc0`'s:
//     nothing of this firmware had run.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sim/sim_flash.h"

namespace fixtures {

// The partition's size, and what the dump inside it is.
constexpr std::uint32_t kCoreDumpPartitionBytes = 0x10000;
constexpr std::uint32_t kFixtureDumpBytes = 3204;
constexpr std::uint32_t kFixtureVersion = 0x00090102u;
constexpr const char* kFixtureElfSha = "5c1cb8f37";
constexpr const char* kFixturePanic =
    "assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)";
constexpr const char* kFixtureCrashedTask = "ipc0";
constexpr const char* kFixtureOtherTask = "ipc1";

// The whole 64 KB, as it was read off the board.
inline std::vector<std::uint8_t> coreDumpFixture() {
  const std::filesystem::path path = std::filesystem::path(TWINOAK_REPO_ROOT) / "tasks" /
                                     "phase-11" / "evidence" / "coredump-0.1.3-ota-trial.bin";
  REQUIRE_MESSAGE(std::filesystem::is_regular_file(path), path.string());
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), path.string());
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  REQUIRE(bytes.size() == kCoreDumpPartitionBytes);
  return bytes;
}

// Bytes into a simulated flash that is erased: NOR flash only clears bits, so this is the write a
// panic handler's would have been.
inline void putDump(sim::SimFlash& flash, const std::vector<std::uint8_t>& bytes) {
  REQUIRE(bytes.size() <= flash.size());
  REQUIRE(flash.write(0, bytes.data(), static_cast<std::uint32_t>(bytes.size())));
}

// The fixture into a simulated flash, whole.
inline void putCoreDumpFixture(sim::SimFlash& flash) { putDump(flash, coreDumpFixture()); }

}  // namespace fixtures
