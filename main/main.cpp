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
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/image/choose.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/image/this_image.h"
#include "core/image/trust.h"
#include "core/image/verdict.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/update/journal.h"
#include "core/update/link_battery.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "core/update/uplink_check.h"
#include "core/version.h"
#include "platform/boot.h"
#include "platform/clock.h"
#include "platform/flash_partition.h"
#include "platform/own_update.h"
#include "platform/pins.h"
#include "platform/retained.h"
#include "platform/uart.h"
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
platform::RadioUart g_uart;
platform::FlashPartition g_rakfw;
platform::FlashPartition g_journal_partition;
// The two application slots and ESP-IDF's rollback (task p2-04), and the trial's uplink
// condition: no server is configured until phase 3, so the condition does not apply
// (core/update/uplink_check.h says why that is the rule and not a gap).
platform::EspOwnUpdate g_own;
core::update::UnconfiguredUplink g_uplink;

// The image store and the USB transfer (task p2-02), when the partition is there: the objects
// themselves are function statics in app_main (the verdict alone is ~30 KB of parser tokens and
// parsed manifest, the store has a sector buffer, and neither belongs on the main task's
// 3584-byte stack), and these are what the console is given. The update journal and the radio's
// update machine (task p2-03) the same way, when the `journal` partition is there too.
core::image::Store* g_store = nullptr;
core::image::Receiver* g_receiver = nullptr;
core::update::Journal* g_journal = nullptr;
core::update::RadioUpdate* g_update = nullptr;

// The time of day, which this firmware does not know yet: phase 3's modem reads it from the
// cellular network and fills this in (interfaces/wall_clock.h). Until then the link client has no
// time to give the radio and does not pretend otherwise - spec/LINK-PROTOCOL.md §5.4 is explicit
// that a clock that goes backwards makes a MeshCore node's own traffic look replayed, and a made-up
// time is how that happens. `rak time set <epoch>` on the console is the way to set it today.
class NoWallClock final : public interfaces::WallClock {
 public:
  bool now(interfaces::WallTime*) override { return false; }
};
NoWallClock g_wall;

}  // namespace

extern "C" void app_main() {
  // First: the seven pins in the safe state (docs/ARCHITECTURE.md §2 rule 1). Constructing
  // RadioPins is safe(); nothing runs before it, not even the banner.
  static core::board::RadioPins radio_pins(g_pins, g_clock, g_retained);

  // Which radio the seven wires reach (task p1-06; spec/SYSTEM.md S-11). **This is the only
  // place a TargetSelect is constructed, and it is a SingleTarget**: a unit has one radio, slot
  // 0, and selecting it moves no pin at all - so this firmware cannot put the dual adapter's
  // selector sequence on the wires. `core::board::DualTarget` is compiled into this build (it is
  // a file of core/) and tested against a simulated selector on the host, and it is constructed
  // nowhere; host_test/core/board/test_slot_single.cpp fails if that changes by accident.
  static core::board::SingleTarget target;
  static core::board::SlotLock slots(target);

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

  // A build made with -DTWINOAK_TRUST_TEST_KEY=1 trusts the specification's public test key
  // (task p2-01, spec/FIRMWARE-MANIFEST.md §3): it accepts anybody's firmware, its version says
  // +TESTKEY, the release tooling refuses it, and it says so at every boot. A normal build has
  // neither the key nor this line's condition true.
  if (core::image::trustsTestKey()) {
    std::printf("WARNING: this build trusts the specification's public TEST KEY: it is a test build "
                "(+TESTKEY) and accepts anybody's firmware; it must never be released\n");
  }

  // S-6: an image confirms itself only after it has booted, spoken to the radio and reached the
  // server. That is the self-update's trial (task p2-04, core/update/self_update.h), begun by
  // the console's begin() below once the journal is open, and run from the main loop. An image
  // flashed over USB is in no OTA state and runs no trial: the bootloader marks only a slot that
  // an update made the next to boot (tasks/phase-0/reports/01.md, rollback).

  // Linked, not called (components/modem/modem_anchor.cpp). The volatile keeps the reference.
  const void* volatile anchor = twinoak_modem_library_anchor();
  (void)anchor;

  // The image store (task p2-02, docs/ARCHITECTURE.md §6): the `rakfw` partition behind
  // interfaces::FlashRegion, the records in it, and `image receive` on this port. A build whose
  // partition table has no such partition says so and runs without a store - the console's
  // `image ...` commands then answer in one line, which is better than a store over flash that
  // is not there.
  if (g_rakfw.begin()) {
    static core::image::Store store(g_rakfw, g_wall);
    static core::image::ImageVerdict verdict(core::image::buildTrust(), core::image::thisUnit());
    static core::image::Receiver receiver(store, verdict, g_usb, g_clock);
    const core::image::StoreError opened = store.begin();
    if (opened == core::image::StoreError::kNone) {
      g_store = &store;
      g_receiver = &receiver;
      std::printf("image store: rakfw at 0x%06" PRIx32 " (%" PRIu32 " KB), %u bytes laid out for "
                  "%u radio slots x 3 images; 'image list' says what is in it\n",
                  g_rakfw.address(), g_rakfw.size() / 1024,
                  static_cast<unsigned>(core::image::kStoreBytes),
                  static_cast<unsigned>(core::image::kRadioSlots));
    } else {
      std::printf("image store: unusable: %s\n", core::image::describe(opened));
    }
  }

  // The update journal (task p2-03, core/update/journal.h): the `journal` partition, read once
  // at boot. What it says was in progress is resumed by the console's begin() below.
  if (g_journal_partition.begin(platform::kJournalPartitionName, platform::kJournalPartitionSubtype)) {
    static core::update::Journal journal(g_journal_partition);
    const core::update::JournalError opened = journal.begin();
    if (opened == core::update::JournalError::kNone) {
      g_journal = &journal;
      std::printf("journal: at 0x%06" PRIx32 " (%" PRIu32 " KB), %" PRIu32 " entries, next seq %" PRIu32
                  "; 'rak update status' tells the story\n",
                  g_journal_partition.address(), g_journal_partition.size() / 1024,
                  journal.validEntries(), journal.nextSeq());
    } else {
      std::printf("journal: unusable: %s\n", core::update::describe(opened));
    }
  }

  // The verdict's anchor (task p2-01, core/image/verdict.h). `image receive` now calls
  // judgeManifest() and judgeFile(), so the six checks have a caller at last and --gc-sections
  // would keep them anyway; the anchor stays because it costs one volatile read and because
  // tools/verify.sh's reading of the linker map - no signing symbol, the verifier present - was
  // written against it.
  const void* volatile verdict_anchor = core::image::verdictAnchor();
  (void)verdict_anchor;

  std::fflush(stdout);

  // The link to the radio (task p1-02): the UART behind interfaces::ByteStream, the port that
  // takes and gives back the two pins through RadioPins, and the client that speaks
  // spec/LINK-PROTOCOL.md over it. Opening the port is what drives TX; it is open from here on so
  // that an EV_BOOT or an EV_ATTENTION from the radio is heard, and a `rak kill` takes it away and
  // the next call takes it back.
  static core::link::client::LinkPort link_port(radio_pins, g_uart);
  static core::link::client::Client radio(link_port, g_clock, g_wall, slots);
  if (!g_uart.begin()) {
    std::printf("the radio's UART would not start: no link in this boot\n");
  } else {
    const core::board::Outcome opened = link_port.open();
    std::printf("link: UART1 115200 8N1 on the radio's two pins, %s\n",
                opened == core::board::Outcome::kOk ? "open"
                                                    : core::board::describe(opened));
  }

  // The radio's update (task p2-03, core/update/radio_update.h; docs/update.md): over the store,
  // its own flasher (12 KB of page buffers, static like the console's), the link client, the
  // journal and the battery as the radio reports it. Without a store or a journal there is no
  // update, and the console says so.
  static core::update::LinkBattery battery(radio);
  static core::flasher::Flasher update_flasher(radio_pins, g_clock, slots);
  core::update::SelfUpdate* self = nullptr;
  if (g_store != nullptr && g_journal != nullptr) {
    static core::update::RadioUpdate update(*g_store, update_flasher, radio, *g_journal, g_clock, battery);
    g_update = &update;
    // The Walter's own update (task p2-04): the other slot as a transfer target, the switch, the
    // trial. Over the real slots (platform/own_update.cpp), the store's bad list, the journal,
    // the link client, the uplink seam, the battery as the radio reports it, and health for the
    // restart's reason.
    static core::update::SelfUpdate self_update(g_own, *g_store, *g_journal, radio, g_uplink, g_clock,
                                                g_restart, battery, health);
    self = &self_update;
  }

  // The console, on the USB port, in this task: one byte at a time, the radio's port read every
  // time round, the uptime kept in retained storage about once a second. Returning is not an
  // option here (the console is this task); the task watchdog watches the idle tasks, which run
  // while this one waits for a byte.
  static core::console::Console console(g_usb, radio_pins, health, g_clock, g_restart, radio,
                                        slots, g_store, g_receiver, g_update, g_journal, self);
  console.begin();
  std::uint32_t last_tick_s = 0;
  for (;;) {
    const int c = console_up ? g_usb.readByte(100) : -1;
    if (c >= 0) console.feed(static_cast<char>(c));
    // A transfer whose other end went away is ended here (core/image/receive.h's idle timeout):
    // the read above returns after 100 ms whether a byte came or not.
    console.tick();
    if (!console_up) g_clock.delayMs(100);
    // Whatever the radio sent while nobody was asking: events queued, scanner lines kept. It
    // reads what is there and returns - nothing here blocks on the radio.
    radio.poll();
    const std::uint32_t up = health.uptimeS();
    if (up != last_tick_s) {
      last_tick_s = up;
      health.tick();
    }
  }
}
