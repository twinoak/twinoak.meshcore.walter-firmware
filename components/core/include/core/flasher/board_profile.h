// The board profiles (task p1-05): per radio board, the flash the Walter may write and the facts
// the flasher checks the chip against before it writes anything. This table is the authority
// (docs/ARCHITECTURE.md §5): a manifest's `app_region` must lie inside the profile's region and is
// refused if it does not (spec/FIRMWARE-MANIFEST.md §5 item 6: "a manifest cannot widen what may
// be written"), an image must lie inside it, and nothing outside it is ever erased or written -
// except the bootloader's settings page, by the one function in flasher.cpp that can touch only
// that address.
//
// Every number carries the file and line it was read from. The sources, all read 2026-10-11:
//
//   MESHCORE  meshcore-dev/MeshCore at d92964352441e53b93e8667b802e04f6e072b39e (the repeater
//             firmware's pin): boards/rak4631.json, boards/nrf52840_s140_v6.ld and the three
//             other scripts beside it.
//   BL        adafruit/Adafruit_nRF52_Bootloader at c67f0bcf0fa8e841426335b1bbde91cda6ca1f50
//             (master, 2026-05-21, release 0.11.0): lib/sdk11/components/libraries/bootloader_dfu/
//             dfu_types.h, linker/nrf52840.ld, lib/softdevice/s140_nrf52_6.1.1/.../nrf_sdm.h and
//             nrf_mbr.h. oltaco/Adafruit_nRF52_Bootloader_OTAFIX at
//             3a75482e17680169edd86760d122a154b1620c4d (master, 2026-08-20) has the same numbers
//             in the same files.
//   LFS       the Adafruit core's InternalFileSystem.cpp (LFS_FLASH_ADDR 0xED000, as the repeater
//             firmware's phase 0 report 02 cites it) and oltaco/CustomLFS 0.2.3 src/CustomLFS.h:37
//             (LFS_DEFAULT_FLASH_ADDR 0xED000, 7 pages): where the file system holding the node's
//             identity and settings begins.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/nrf52/nrf52.h"
#include "core/nrf52/regs.h"

namespace core {
namespace flasher {

struct BoardProfile {
  const char* name;             // the manifest's `board` (spec/FIRMWARE-MANIFEST.md §2)
  std::uint32_t expected_part;  // FICR.INFO.PART
  std::uint32_t page_size;      // FICR.CODEPAGESIZE
  nrf52::FlashRegion app;       // [begin, end): the application region, page-aligned
  // What the SoftDevice's own information structure must say its size is: the bootloader starts
  // the application right after the SoftDevice (BL dfu_types.h:77, CODE_REGION_1_START =
  // SD_SIZE_GET(MBR_SIZE)), so a chip whose SoftDevice ends elsewhere runs an image linked for
  // this profile nowhere, and is refused before anything is written.
  std::uint32_t softdevice_size;
  std::uint32_t bootloader_start;  // where the bootloader begins; informational, never written
  std::uint32_t settings_page;     // the bootloader settings page: the one page outside `app` the flasher touches

  // A manifest's app_region lies inside this profile's region.
  bool accepts(const nrf52::FlashRegion& manifest_region) const {
    return manifest_region.begin < manifest_region.end && manifest_region.begin >= app.begin &&
           manifest_region.end <= app.end;
  }
  // An image of `size` bytes at `load_address` lies inside the region (checked in 64 bits).
  bool holds(std::uint32_t load_address, std::uint32_t size) const {
    const std::uint64_t end = static_cast<std::uint64_t>(load_address) + size;
    return size > 0 && load_address >= app.begin && end <= app.end;
  }
};

// The single-radio adapter with a RAK4630 (the only board today; S-11).
//
//   app.begin 0x26000   MESHCORE boards/nrf52840_s140_v6.ld:8
//                         FLASH (rx) : ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000
//                       which boards/rak4631.json:4 names ("ldscript": "nrf52840_s140_v6.ld") for
//                       every RAK4631 environment that does not override it - the repeater's does
//                       not. It is the end of the S140 6.1.1 SoftDevice (rak4631.json:34-38:
//                       sd_version 6.1.1, sd_fwid 0x00B6), which the chip itself states in the
//                       SoftDevice information structure (BL nrf_sdm.h:99-138: SD_SIZE_OFFSET
//                       0x2008 from the MBR, nrf_mbr.h:68 MBR_SIZE 0x1000) - softdevice_size, and
//                       the flasher reads it. The v7 scripts (nrf52840_s140_v7.ld:8) start at
//                       0x27000: a chip carrying S140 7.x says so there and is refused.
//   app.end   0xED000   the same line: ORIGIN + LENGTH. The first byte above the application is
//                       the file system (LFS: LFS_FLASH_ADDR 0xED000, seven 4 KB pages to 0xF4000)
//                       - the node's identity, its settings, the ACL. The two *_extrafs.ld scripts
//                       (nrf52840_s140_v6_extrafs.ld:8: LENGTH = 0xD4000 - 0x26000) end 100 KB
//                       lower; they are the companion radio's (platformio.ini: only the
//                       RAK_4631_companion_* environments name them) and the repeater firmware does
//                       not link with them, so its images - and the app_region its release tooling
//                       writes from the script it did link with - reach 0xED000. The task asked for
//                       "the more conservative of the linker scripts in use"; 0xD4000 would refuse
//                       every repeater release, and the report says so.
//   page_size 4096      nRF52840 Product Specification, "Memory" ("256 pages of 4 kB each");
//                       confirmed from FICR at run time by nrf52::Nrf52::identify.
//   bootloader_start    BL dfu_types.h:93 BOOTLOADER_REGION_START 0x000F4000; linker/nrf52840.ld:14
//             0xF4000   FLASH (rx) : ORIGIN = 0xF4000. Never written.
//   settings_page       BL dfu_types.h:96 BOOTLOADER_SETTINGS_ADDRESS 0x000FF000; nrf52840.ld:22
//             0xFF000   BOOTLOADER_SETTINGS (rw) : ORIGIN = 0xFF000, LENGTH = 0x1000; MESHCORE
//                       rak4631.json:41 "settings_addr": "0xFF000". The MBR parameter page below it
//                       (dfu_types.h:95, 0xFE000) is never touched.
inline constexpr BoardProfile kRak4630Single = {
    "rak4630-single",
    nrf52::ficr::kPartNrf52840,
    nrf52::flash::kPageSize,
    nrf52::FlashRegion{0x00026000, 0x000ED000},
    0x00026000,
    0x000F4000,
    0x000FF000,
};

// How many radios this build's adapter has, and what is in each position (task p1-06; S-11:
// "The Walter addresses radios by *slot*; there is one slot today"). The dual adapter's second
// position has no profile here on purpose: what a dual position's manifest `board` is called is
// not decided anywhere, and inventing a name would put it in the firmware before the hardware.
// A unit with two radios is a board, a profile and a release channel, and it will bring them.
constexpr std::size_t kSlotsPresent = 1;

// The board in that slot, or nullptr when this adapter has no such position.
const BoardProfile* profileForSlot(board::Slot slot);

// The profile for a slot *and* a manifest's board name: nullptr when the slot is not there, or
// when the board in it is not the one the manifest was built for (spec/FIRMWARE-MANIFEST.md §5
// item 2). The slot comes first because it decides whether there is anything to look up.
const BoardProfile* findProfile(board::Slot slot, const char* board_name);

}  // namespace flasher
}  // namespace core
