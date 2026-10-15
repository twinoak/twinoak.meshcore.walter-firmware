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
#include "core/cycle/duty_cycle.h"
#include "core/health/health.h"
#include "core/image/choose.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/image/this_image.h"
#include "core/image/trust.h"
#include "core/image/verdict.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/net/client.h"
#include "core/net/download.h"
#include "core/net/http_over_tls.h"
#include "core/net/trust.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/update/fetch.h"
#include "core/update/tls_check.h"
#include "core/update/journal.h"
#include "core/update/last_good_battery.h"
#include "core/update/link_battery.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "core/update/uplink_check.h"
#include "core/uplink/policy.h"
#include "core/version.h"
#include "modem/walter_tls.h"
#include "modem/walter_uplink.h"
#include "platform/boot.h"
#include "platform/clock.h"
#include "platform/flash_partition.h"
#include "platform/key_value.h"
#include "platform/own_update.h"
#include "platform/pins.h"
#include "platform/retained.h"
#include "platform/sleeper.h"
#include "platform/uart.h"
#include "platform/usb_console.h"

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
// Deep sleep and its two wake sources (task p3-04): the timer and a start bit from the radio on
// GPIO8. USB is not one of them - this chip cannot wake from deep sleep on a cable - so a boot
// that finds a host stays awake instead (components/platform/sleeper.cpp).
platform::EspSleeper g_sleeper;
platform::FlashPartition g_rakfw;
platform::FlashPartition g_journal_partition;
// The settings (task p3-03): NVS behind interfaces::KeyValue, and the table over it. The store
// is above the application slots in partitions.csv, so a factory image cannot reach it
// (docs/ARCHITECTURE.md §11).
platform::NvsKeyValue g_nvs;
// The two application slots and ESP-IDF's rollback (task p2-04). The trial's uplink condition
// is a TLS handshake with the configured server (task p3-02, core/update/tls_check.h), and the
// server is the `server.url` setting (task p3-03): unset, the host is empty and the condition
// does not apply (core/update/uplink_check.h says why that is the rule and not a gap).
platform::EspOwnUpdate g_own;

// The image store and the USB transfer (task p2-02), when the partition is there: the objects
// themselves are function statics in app_main (the verdict alone is ~30 KB of parser tokens and
// parsed manifest, the store has a sector buffer, and neither belongs on the main task's
// 3584-byte stack), and these are what the console is given. The update journal and the radio's
// update machine (task p2-03) the same way, when the `journal` partition is there too.
core::image::Store* g_store = nullptr;
core::image::Receiver* g_receiver = nullptr;
core::update::Journal* g_journal = nullptr;
core::update::RadioUpdate* g_update = nullptr;
core::update::Fetch* g_fetch = nullptr;

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

  // And then rule 5's other half, in this order (task p3-04; the comment in
  // core/board/radio_pins.h and components/platform/sleeper.cpp carry the documents):
  //
  //   1. constructing RadioPins was safe(): the pads' *registers* now hold the state they were
  //      held in. They are still latched, so nothing on a wire moved.
  //   2. releaseHolds(): safe() again, then the latches off - the pad sees no change, because
  //      what it is released into is what it was held at. This is what keeps nRESET from
  //      glitching across a wake.
  //   3. the sleeper's begin(): the wake cause read, and the pad the radio's line was armed on
  //      routed back from the RTC mux to the IO mux (`rtc_gpio_deinit`), which ESP-IDF requires
  //      before an ext1 wake pin can be a digital GPIO again - the UART needs it back.
  //   4. safe() once more, so that the pad that has just come back from the RTC mux is
  //      configured by the one object that owns these seven pins.
  (void)radio_pins.releaseHolds();
  g_sleeper.begin();
  radio_pins.safe();

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

  // The settings (task p3-03, core/settings/keys.h): the `nvs` partition behind
  // interfaces::KeyValue and the table of eleven keys over it. Read once, here; the console's
  // begin() is what puts them in force (`log.level` before it prints anything, `swd.khz`, the
  // APN, the PIN, the server a self-update's trial must reach). A partition that would not open
  // is one line and every key falling back - which is a unit that was never provisioned, and
  // that is a state this firmware has to survive anyway.
  static core::settings::Settings settings(g_nvs);
  if (g_nvs.begin()) {
    settings.begin();
    std::printf("settings: nvs namespace '%s', %u keys%s; 'get' prints them, "
                "'set <key> <value>' changes one\n", platform::kSettingsNamespace,
                static_cast<unsigned>(core::settings::kKeyCount),
                g_nvs.wasRebuilt() ? " (the partition had to be erased and rebuilt: a new unit, "
                                     "or a layout change)" : "");
    std::printf("settings: NOT encrypted - flash encryption is the owner's choice and is not "
                "assumed: a person with this board and a cable can read lte.pin and "
                "server.token out of it (docs/console.md)\n");
  } else {
    std::printf("settings: the `nvs` partition would not open (esp_err %d): every key falls back "
                "and a 'set' cannot be kept\n", g_nvs.lastError());
  }

  // The uplink (task p3-01, docs/ARCHITECTURE.md §9): `walter-modem` behind
  // interfaces::Uplink (components/modem/walter_uplink.cpp) and the policy over it - the
  // sequence, the deadlines, the wake's budget and the backoff (core/uplink/policy.h).
  //
  // **Nothing here touches the modem.** begin() reads the backoff out of retained storage and
  // that is all; the library itself is started by the first call that needs it, so a wake the
  // backoff skips costs the modem nothing. When the modem runs is the wake cycle's business
  // (task p3-05), and until then `lte up` on the console is the only thing that brings it up.
  static modem::WalterUplink modem_driver(g_clock);
  static core::uplink::UplinkPolicy uplink(modem_driver, g_clock, g_retained);
  uplink.begin();
  {
    const core::uplink::Status& u = uplink.status();
    std::printf("uplink: the modem on UART%d behind interfaces::Uplink; not started yet "
                "('lte', 'lte up')\n", modem::kModemUartPort);
    if (u.tries != 0 || u.last_failure != core::uplink::Failure::kNone) {
      std::printf("uplink: last time: %s; %lu wakes counted, %lu attempts; next try in %lu wake(s)\n",
                  core::uplink::describe(u.last_failure), static_cast<unsigned long>(u.wakes),
                  static_cast<unsigned long>(u.tries),
                  static_cast<unsigned long>(u.wakes_until_next_try));
    }
  }

  // The HTTPS client (task p3-02, docs/ARCHITECTURE.md §9): the modem's TLS over one of its
  // sockets (components/modem/walter_tls.cpp) behind interfaces::TlsSocket; the roots of
  // keys/tls/ and the loader that writes them into the modem's certificate slots once
  // (core/net/trust.h); HTTP/1.1 over the socket (core/net/http_over_tls.h); the client with the
  // rules (core/net/client.h); the download in ranges (core/net/download.h). The one call that
  // configures a TLS profile is WalterTlsStore::configureProfile, and it validates the chain and
  // the host name every time. Nothing here touches the modem: the roots are written at the first
  // request of a boot whose record says they differ, and `trust tls` says what the record says.
  static modem::WalterTlsStore tls_store(modem_driver, g_clock);
  static modem::WalterTlsSocket tls_socket(modem_driver, g_clock);
  static core::net::TrustLoader trust(tls_store, g_retained, g_clock);
  trust.begin();
  static core::net::HttpOverTls https(tls_socket, &trust, g_clock);
  static core::net::Client http_client(https, g_clock);
  static core::net::Download download(http_client, g_clock);
  // The trial's third condition over the real uplink (S-6): a TLS handshake with the configured
  // server, which is `server.url` (task p3-03) - the console's begin() hands it over. Unset, it
  // answers "not configured" and the condition does not apply.
  static core::update::TlsHandshakeCheck g_uplink(tls_socket, &trust, g_clock);
  std::printf("https: %lu TLS roots (keys/tls), %s; 'trust tls', 'http get <url>', 'fetch <base-url>'\n",
              static_cast<unsigned long>(core::net::tlsRootCount()),
              trust.recordLost() ? "the modem's slots have no record and are written at the first request"
                                 : "the modem's slots are recorded as written");

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
      // The fetch of a release into this store (task p3-02): over the client, the download and
      // the same verdict the USB transfer uses. The console's `fetch` and, later, the wake cycle.
      static core::update::Fetch fetch(http_client, download, store, verdict, g_clock, &g_usb);
      g_fetch = &fetch;
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
  static core::update::LinkBattery link_battery(radio);
  // The last good reading, kept (task p2-05, core/update/last_good_battery.h): the one moment a
  // reading is most needed is the one moment it cannot be taken - the radio has gone quiet - so
  // the supervisor's rung 4 and the update machine both read through this rather than straight
  // off the link.
  static core::update::LastGoodBattery battery(link_battery, g_clock);
  static core::flasher::Flasher update_flasher(radio_pins, g_clock, slots);
  core::update::SelfUpdate* self_update_ptr = nullptr;
  if (g_store != nullptr && g_journal != nullptr) {
    static core::update::RadioUpdate update(*g_store, update_flasher, radio, *g_journal, g_clock, battery);
    g_update = &update;
    // The Walter's own update (task p2-04): the other slot as a transfer target, the switch, the
    // trial. Over the real slots (platform/own_update.cpp), the store's bad list, the journal,
    // the link client, the uplink seam, the battery as the radio reports it, and health for the
    // restart's reason.
    static core::update::SelfUpdate self_update(g_own, *g_store, *g_journal, radio, g_uplink, g_clock,
                                                g_restart, battery, health);
    self_update_ptr = &self_update;
  }

  // The supervisor (task p2-05, core/supervisor/supervisor.h; docs/supervisor.md): the radio's
  // heartbeat and the ladder, over the link client, the pins, the flasher's read-only verify, the
  // update machine's rollback path, the journal, the battery's last good reading, retained
  // storage for the day's counts and health for "did this Walter just power up". **Nothing drives
  // it from the loop below**: when the supervisor runs is the wake cycle's business (phase 3), and
  // until then it is `rak ladder` on the console that climbs and `rak watch` that reads. It is
  // constructed all the same, so that its retained record and the journal are read at every boot
  // and a stop in force is announced.
  static core::supervisor::Supervisor supervisor(radio, radio_pins, slots, health, battery, g_clock,
                                                 g_wall, g_retained, g_store, &update_flasher,
                                                 g_update, g_journal);
  // Its retained record and the journal, read here rather than by the console's begin(), because
  // the duty cycle below asks it to listen for §8's attention first and a supervisor that has
  // not read its record cannot record one properly. The console still prints what it found.
  supervisor.begin();

  // The duty cycle (task p3-04, docs/ARCHITECTURE.md §10, docs/power.md): why this wake
  // happened, what the battery allows, the bound on a wake, and the one way out of one. **This
  // is the only object in this firmware that sleeps**, and until the wake cycle exists (task
  // p3-05) the console is the only thing that drives it: `sleep now`, `sleep <seconds>`, `wake`,
  // `power`, `stay`.
  static core::cycle::DutyCycle duty(g_sleeper, radio_pins, radio, uplink, health, g_clock, g_wall,
                                     g_restart, g_retained, &supervisor, g_update,
                                     self_update_ptr, g_journal, &settings);
  // What this wake means, before anything is done about it. For a wake on the radio's line this
  // listens for up to §8's whole repeat window (about eleven seconds), which is why it is here -
  // after the link's port is open - and not at the top of this function.
  duty.begin();
  // The bound on a wake is deliberately **not** armed: nothing in this firmware reaches sleep by
  // itself yet, so a bound that reset the Walter would be a boot loop and not a safety rail.
  // `duty.guard().arm()` is the one line task p3-05 adds (tasks/phase-3/reports/04.md).

  // The console, on the USB port, in this task: one byte at a time, the radio's port read every
  // time round, the uptime kept in retained storage about once a second. Returning is not an
  // option here (the console is this task); the task watchdog watches the idle tasks, which run
  // while this one waits for a byte.
  static core::console::Console console(g_usb, radio_pins, health, g_clock, g_restart, radio,
                                        slots, g_store, g_receiver, g_update, g_journal,
                                        self_update_ptr, &supervisor, &uplink, &http_client,
                                        g_fetch, &trust, &settings, &g_uplink, &duty);
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
