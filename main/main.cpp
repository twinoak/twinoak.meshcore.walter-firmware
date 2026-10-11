// app_main: the first thing the Walter says, and nothing else yet (task p0-01). Pins are task 03's,
// the link, the radio and the modem later phases'. This file is platform code and may include
// ESP-IDF; the logic it will one day hand things to is in components/core/.
#include <cinttypes>
#include <cstdio>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "sdkconfig.h"

#include "core/image/this_image.h"
#include "core/version.h"

// components/modem: the modem library is linked and called by nothing. See modem_anchor.cpp.
extern "C" const void* twinoak_modem_library_anchor();

namespace {

// esp_ota_img_states_t, esp_flash_partitions.h (ESP-IDF 5.5.5) - what the bootloader does with
// each is in bootloader_utility.c, and in tasks/phase-0/reports/01.md under rollback.
const char* ota_state_name(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return "new (first boot; the bootloader will mark it pending-verify)";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify (not yet confirmed; aborted at the next reset)";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined (flashed over USB, not by an update; rollback does not apply)";
  }
  return "?";
}

}  // namespace

extern "C" void app_main() {
  // The banner: product and version, one line, first (host_test/core/test_version.cpp).
  std::printf("\n%s\n", core::banner());

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    std::printf("running from %s at 0x%06" PRIx32 " (%" PRIu32 " KB), ota state %s\n", running->label,
                running->address, running->size / 1024, ota_state_name(state));
  } else if (running != nullptr) {
    std::printf("running from %s at 0x%06" PRIx32 "\n", running->label, running->address);
  }

  // The descriptor this image carries (spec/FIRMWARE-MANIFEST.md §4, core/image/this_image.h),
  // read out of the bytes themselves. Printing it is also what keeps it: core::image::kThisImage
  // is referenced from here, so --gc-sections does not drop the object, and the release tooling
  // finds exactly one of it in the file it publishes (tools/release/build_release.py).
  const core::image::Fields self = core::image::this_image();
  const char* role = core::image::role_name(self.role);
  std::printf("image descriptor at %p: role %s, board %s, band %s, version %s, link %u.%u\n",
              static_cast<const void*>(core::image::kThisImage.bytes), role != nullptr ? role : "?",
              self.board, self.band, self.version, static_cast<unsigned>(self.link_major),
              static_cast<unsigned>(self.link_minor));

  // S-6: an image confirms itself only after it has booted, spoken to the radio and reached the
  // server. None of that exists yet, so this image confirms nothing: flashed over USB its state is
  // undefined and it keeps booting; arriving by an update it would be rolled back, which is right.

  // Linked, not called (components/modem/modem_anchor.cpp). The volatile keeps the reference.
  const void* volatile anchor = twinoak_modem_library_anchor();
  (void)anchor;

  std::printf("console on USB; nothing to do yet - idling\n");
  std::fflush(stdout);
  // Returning from app_main leaves FreeRTOS idling; the task watchdog watches the idle tasks.
}
