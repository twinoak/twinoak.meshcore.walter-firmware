// The Adafruit nRF52 bootloader's settings page, as the two bootloaders a RAK4630 may carry read
// it (task p1-05; spec/SYSTEM.md §10 item 3). Both were read at a named commit on 2026-10-11:
//
//   STOCK   adafruit/Adafruit_nRF52_Bootloader c67f0bcf0fa8e841426335b1bbde91cda6ca1f50 (master,
//           2026-05-21; release 0.11.0 is this commit)
//   OTAFIX  oltaco/Adafruit_nRF52_Bootloader_OTAFIX 3a75482e17680169edd86760d122a154b1620c4d
//           (master, 2026-08-20; release 0.9.2-OTAFIX2.3-BP1.4 of 2026-08-19), the variant MeshCore
//           recommends - a fork of stock 0.9.2, so what is said of it below holds for every stock
//           release before 0.11.0 as well
//
// The record (lib/sdk11/components/libraries/bootloader_dfu/bootloader_types.h:45-55, identical
// in both): at BOOTLOADER_SETTINGS_ADDRESS (dfu_types.h:96, 0xFF000),
//
//   offset 0  uint16_t bank_0        "if bank 0 contains a valid application"
//   offset 2  uint16_t bank_0_crc    "a valid CRC of the total image"
//   offset 4  uint16_t bank_1        banked updates only
//   offset 8  uint32_t bank_0_size   "Size of active image in bank0 if present, otherwise 0"
//   offset 12 uint32_t sd_image_size, bl_image_size, app_image_size, sd_image_start
//
// with the bank codes of bootloader_types.h:34-41: BANK_VALID_APP 0x01, BANK_VALID_SD 0xA5,
// BANK_VALID_BOOT 0xAA, BANK_ERASED 0xFE, BANK_INVALID_APP 0xFF - and 0xFFFF, an erased page.
//
// What each bootloader does with it, bootloader_app_is_valid() (STOCK bootloader.c:175-224,
// OTAFIX bootloader.c:148-182):
//
//   1. both: the application is invalid if its first two words (the initial stack pointer and the
//      reset vector at CODE_REGION_1_START) are both 0xFFFFFFFF (STOCK :183-186, OTAFIX :158-163);
//   2. both: if bank_0 == BANK_VALID_APP, the application is valid when bank_0_crc is 0 ("A stored
//      crc value of 0 indicates that CRC checking is not used", STOCK :196-197, OTAFIX :170-171)
//      or when CRC-16 over bank_0_size bytes from the application's start equals it (STOCK
//      :192-204, OTAFIX :166-178);
//   3. STOCK only, since commit 67105f8 of 2026-05-15 ("allow debugger flashing e.g with jlink
//      without flashing the setting page"): if bank_0 == 0xFFFF - the page erased - the
//      application is valid when its vector table is plausible: the stack pointer inside RAM
//      (FICR.INFO.RAM) and word-aligned, the reset vector odd (Thumb) and inside
//      [CODE_REGION_1_START, BOOTLOADER_REGION_START) (:207-222). OTAFIX, and every stock release
//      before 0.11.0, returns false here (OTAFIX :181);
//   4. both: any other bank_0 - BANK_INVALID_APP above all - is invalid, whatever the bytes.
//
// What each writes there itself: every UF2 install (STOCK src/usb/msc_uf2.c:202-241: update_status
// zeroed, status DFU_UPDATE_APP_COMPLETE, so app_crc 0 and app_size 0) and every serial or OTA
// DFU (dfu_single_bank.c:236-248 with m_image_crc, which dfu_init() sets to 0 at :281 and nothing
// ever computes) end in bootloader_dfu_update_process() writing bank_0 = BANK_VALID_APP,
// bank_0_crc = 0, bank_1 = BANK_INVALID_APP (bootloader.c:280-288). So a RAK in the field carries
// BANK_VALID_APP with the CRC check off, and the bootloader starts whatever is at 0x26000 whose
// first two words are not blank. A page with a real CRC comes from other tools (nrfutil's settings
// generator, Nordic's SDK bootloaders); the flasher accepts one that matches and rewrites one that
// does not. Before the DFU erases bank 0 it writes bank_0 = BANK_INVALID_APP (dfu_single_bank.c:
// 167-172 DFU_BANK_0_ERASED, bootloader.c:356-363) - the state in which both bootloaders refuse the
// application by rule 4 alone, and the state this flasher puts the page in before it touches the
// application (flasher.cpp says why that order, and not another, is the safe one).
//
// Nothing here touches flash: these are the record's layout and the two rules, shared by the
// flasher and by the simulator (host_test/sim/nrf52_chip), which evaluates both bootloaders after
// every interrupted flash.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace flasher {
namespace bootloader_settings {

constexpr std::uint16_t kBankValidApp = 0x01;
constexpr std::uint16_t kBankValidSd = 0xA5;
constexpr std::uint16_t kBankValidBoot = 0xAA;
constexpr std::uint16_t kBankErased = 0xFE;
constexpr std::uint16_t kBankInvalidApp = 0xFF;
constexpr std::uint16_t kBankBlank = 0xFFFF;  // an erased settings page

// The record is 28 bytes: seven words.
constexpr std::size_t kRecordWords = 7;
constexpr std::size_t kRecordBytes = kRecordWords * 4;

struct Record {
  std::uint16_t bank_0 = kBankBlank;
  std::uint16_t bank_0_crc = 0xFFFF;
  std::uint16_t bank_1 = kBankBlank;
  std::uint32_t bank_0_size = 0xFFFFFFFF;
  std::uint32_t sd_image_size = 0xFFFFFFFF;
  std::uint32_t bl_image_size = 0xFFFFFFFF;
  std::uint32_t app_image_size = 0xFFFFFFFF;
  std::uint32_t sd_image_start = 0xFFFFFFFF;

  bool blank() const { return bank_0 == kBankBlank; }
  // Rule 2 with the CRC check off: what every UF2 and DFU install leaves, and what this flasher
  // writes after a verified image.
  bool validWithoutCrc() const { return bank_0 == kBankValidApp && bank_0_crc == 0; }
};

// The record as it lies in flash: little-endian, the two bytes of padding at offset 6 left erased.
void encode(const Record& record, std::uint32_t out[kRecordWords]);
Record decode(const std::uint32_t in[kRecordWords]);

// "Bank 0 holds nothing": bank_0 = BANK_INVALID_APP, the CRC and sizes 0, bank_1 =
// BANK_INVALID_APP - the DFU_BANK_0_ERASED record of bootloader.c:356-363.
Record invalidRecord();
// "Bank 0 holds a valid application of `size` bytes, CRC checking not used" - the record every
// UF2 and DFU install ends with (bootloader.c:280-285 with app_crc 0), the size filled in.
Record validRecord(std::uint32_t size);

}  // namespace bootloader_settings
}  // namespace flasher
}  // namespace core
