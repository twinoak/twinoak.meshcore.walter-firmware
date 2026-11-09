// The Walter's own two application slots, as core/ sees them (task p2-04): which one is running
// and in what state, the other one as a place to write an image, making it the next to boot,
// confirming the running image, rejecting it. Behind it is ESP-IDF's OTA machinery over the two
// `ota_0`/`ota_1` partitions of partitions.csv with rollback enabled (platform/own_update.cpp;
// $IDF_PATH/docs/en/api-reference/system/ota.rst "App rollback"), or - in the host tests - a
// fake that carries the bootloader's rules so that a trial, a confirmation and a rollback can be
// run through a simulated reset (host_test/fakes/fake_own_update.h).
//
// The bootloader's rules this interface is written against (ESP-IDF v5.5.5,
// components/bootloader_support/src/bootloader_utility.c,
// bootloader_utility_get_selected_boot_partition, with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE):
//
//   - setNextBoot() writes the other slot's otadata entry with the state NEW
//     (esp_ota_set_boot_partition -> esp_rewrite_ota_data -> set_new_state_otadata);
//   - at the next boot the bootloader selects it and, finding NEW, writes PENDING_VERIFY and
//     starts it: the image is on trial and has this one boot;
//   - any slot still PENDING_VERIFY when the bootloader runs is written ABORTED first and is not
//     selected again: a second boot of an unconfirmed image is a rollback, whatever reset it;
//   - confirm() writes VALID (esp_ota_mark_app_valid_cancel_rollback); reject() writes INVALID
//     and restarts (esp_ota_mark_app_invalid_rollback_and_reboot), and the bootloader then
//     selects the other slot;
//   - an image flashed over USB sits in a slot whose entry is UNDEFINED (0xFFFFFFFF): "app can
//     boot and work without limits" (esp_flash_partitions.h). No trial is ever run on it, which
//     is what lets a factory image boot for ever without a server to reach
//     (tasks/phase-0/reports/01.md, rollback).
//
// Offsets handed to readSlot() are from the start of the slot. Nothing above this interface
// knows where in the flash a slot is, except to compare a manifest's load_address with it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

// esp_ota_img_states_t (components/bootloader_support/include/esp_flash_partitions.h, v5.5.5),
// as words. The platform maps the enumerators; the host fake models the transitions.
enum class OwnImageState : std::uint8_t {
  kUndefined = 0,      // 0xFFFFFFFF: flashed over USB, not by an update; rollback does not apply
  kNew,                // the next to boot, not yet booted (written by setNextBoot)
  kPendingVerify,      // its first boot: on trial, confirmed or aborted at the next reset
  kValid,              // confirmed
  kInvalid,            // rejected by the application itself
  kAborted,            // was pending at a reset: the bootloader gave up on it
  kUnknown,            // the state could not be read
};

// Inline: interfaces/ is headers only (README.md), and a word per state is all this is.
inline const char* describe(OwnImageState state) {
  switch (state) {
    case OwnImageState::kUndefined: return "undefined (flashed over USB, not by an update; rollback does not apply)";
    case OwnImageState::kNew: return "new (the next to boot; its trial begins at the restart)";
    case OwnImageState::kPendingVerify: return "pending-verify (on trial: confirmed in this boot or aborted at the next)";
    case OwnImageState::kValid: return "valid (confirmed)";
    case OwnImageState::kInvalid: return "invalid (rejected by the image itself)";
    case OwnImageState::kAborted: return "aborted (was on trial at a reset; the bootloader gave up on it)";
    case OwnImageState::kUnknown: return "unknown";
  }
  return "?";
}

// One of the two slots. `index` is 0 for ota_0 and 1 for ota_1; `address` and `size` are the
// partition's; `version` is what the image in it says it is (ESP-IDF's own application
// description, esp_app_desc_t.version, which the build fills from PROJECT_VER - the same string
// as core::version() and the descriptor's), empty when the slot holds no readable image.
struct OwnSlot {
  bool known = false;
  std::uint8_t index = 0;
  std::uint32_t address = 0;
  std::uint32_t size = 0;
  OwnImageState state = OwnImageState::kUnknown;
  char version[32] = {};
  // The same application description also carries the SHA-256 of the ELF the image was linked
  // from (esp_app_desc_t.app_elf_sha256, 32 raw bytes, filled by the build). It is how a **core
  // dump** names the image that wrote it - `ESP_CORE_DUMP_INFO`'s `app_elf_sha256` is the first
  // CONFIG_APP_RETRIEVE_LEN_ELF_SHA hex characters of it - so a dump found at the boot after a
  // rolled-back trial can be attributed to the image that was on trial and to no other
  // (core/health/coredump.h, task p11-03). False when the slot holds no readable image.
  bool has_elf_sha256 = false;
  std::uint8_t elf_sha256[32] = {};
};

class OwnUpdate {
 public:
  virtual ~OwnUpdate() = default;

  // The slot this firmware runs from, and the other one. False when the platform cannot say
  // (no OTA data partition, a table without two slots): nothing of the self-update works then.
  virtual bool running(OwnSlot* out) = 0;
  virtual bool other(OwnSlot* out) = 0;

  // -- writing the other slot --------------------------------------------------------------
  // Erases room for `size` bytes in the other slot and opens it for writing (esp_ota_begin).
  // False when the running image is itself unconfirmed (the platform refuses, as esp_ota_begin
  // does), when a write is already open, or when the erase failed.
  virtual bool beginWrite(std::uint32_t size) = 0;
  // The next bytes, in order (esp_ota_write).
  virtual bool write(const void* data, std::size_t size) = 0;
  // The last byte has been given: whatever the platform buffers goes to flash, and the image is
  // checked for being an application at all (esp_ota_end validates the image's own format).
  // False when it is not one; the slot is then not bootable and nothing points at it.
  virtual bool endWrite() = 0;
  // Gives up on an open write (esp_ota_abort). Nothing points at the slot.
  virtual void abortWrite() = 0;
  virtual bool writing() const = 0;

  // `count` bytes from `offset` of a slot, for reading an image back out of flash and judging
  // it (spec/FIRMWARE-MANIFEST.md §5 checks 4 and 5 over the bytes as they lie there). False
  // when the range leaves the slot or the read failed.
  virtual bool readSlot(std::uint8_t index, std::uint32_t offset, void* out, std::uint32_t count) const = 0;

  // -- the boot slot and the running image's verdict ---------------------------------------
  // The other slot becomes the next to boot, with the state NEW (esp_ota_set_boot_partition).
  // False when the slot holds no valid application image.
  virtual bool setNextBoot() = 0;
  // The running image is confirmed: VALID (esp_ota_mark_app_valid_cancel_rollback). True also
  // when it was valid or undefined already.
  virtual bool confirm() = 0;
  // The running image is rejected - INVALID - and the board restarts into the other slot
  // (esp_ota_mark_app_invalid_rollback_and_reboot). On a real board this does not return; a
  // fake may, and returns false when the platform refuses because the other slot holds nothing
  // bootable (ESP_ERR_OTA_ROLLBACK_FAILED) - the image then stays where it is.
  virtual bool rejectAndRestart() = 0;
};

}  // namespace interfaces
