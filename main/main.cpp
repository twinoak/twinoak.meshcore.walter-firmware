// app_main: construct the real things, hand them to the core, run (docs/ARCHITECTURE.md §1).
// This file is platform code and may include ESP-IDF; the logic it hands things to is in
// components/core/. Task p0-03: the pins go to the safe state before anything else, the boot's
// reason is recorded, and the console runs on the module's USB.
#include <cinttypes>
#include <cstdio>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "sdkconfig.h"

#include "core/board/radio_pins.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/image/this_image.h"
#include "core/version.h"
#include "platform/boot.h"
#include "platform/clock.h"
#include "platform/pins.h"
#include "platform/retained.h"
#include "platform/usb_console.h"

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

// The real things, with static storage: they live as long as the firmware runs, and nothing here
// is allocated after start-up.
platform::EspPins g_pins;
platform::EspClock g_clock;
platform::RtcRetained g_retained;
platform::EspRestart g_restart;
platform::UsbConsole g_usb;

}  // namespace

extern "C" void app_main() {
  // First: the seven pins in the safe state (docs/ARCHITECTURE.md §2 rule 1). Constructing
  // RadioPins is safe(); nothing runs before it, not even the banner.
  static core::board::RadioPins radio_pins(g_pins, g_clock, g_retained);

  // Why this boot happened, counted and remembered (core/health).
  static core::health::Health health(g_retained, g_clock);
  health.begin(platform::resetReason(), platform::wakeCause());

  const bool console_up = g_usb.begin();

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

  std::fflush(stdout);

  // The console, on the USB port, in this task: one byte at a time, the uptime kept in retained
  // storage about once a second. Returning is not an option here (the console is this task);
  // the task watchdog watches the idle tasks, which run while this one waits for a byte.
  static core::console::Console console(g_usb, radio_pins, health, g_clock, g_restart);
  console.begin();
  std::uint32_t last_tick_s = 0;
  for (;;) {
    const int c = console_up ? g_usb.readByte(100) : -1;
    if (c >= 0) console.feed(static_cast<char>(c));
    if (!console_up) g_clock.delayMs(100);
    const std::uint32_t up = health.uptimeS();
    if (up != last_tick_s) {
      last_tick_s = up;
      health.tick();
    }
  }
}
