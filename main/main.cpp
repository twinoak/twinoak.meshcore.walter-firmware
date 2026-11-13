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
#include "core/cycle/network_clock.h"
#include "core/cycle/null_backend.h"
#include "core/cycle/telemetry_ring.h"
#include "core/cycle/wake_counters.h"
#include "core/cycle/wake_cycle.h"
#include "core/health/coredump.h"
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
#include "modem/modem_trace.h"
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
#include "platform/entropy.h"
#include "platform/heap.h"
#if WALTER_AGENT
#include <agent/AgentLoop.h>
#include <agent/JobRunner.h>
#include <agent/Version.h>

#include "walter_agent/agent_backend.h"
#include "walter_agent/agent_probe.h"
#include "walter_agent/agent_uplink_check.h"
#include "walter_agent/firmware_jobs.h"
#include "walter_agent/identity.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/mesh_probe.h"
#include "walter_agent/modem_uplink.h"
#include "walter_agent/observations.h"
#include "walter_agent/region_store.h"
#include "walter_agent/uplink_probe.h"
#include "walter_agent/walter_platform.h"
#endif

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
// The modem library's own TX/RX trace, off until somebody types `trace modem on` (task p10-06).
// It carries no state of its own - the hook esp_log_set_vprintf installs is the image's - and it
// is here with the other real things because the console takes it as a seam
// (core/console/modem_trace.h).
modem::WalterModemTrace g_modem_trace;
platform::RadioUart g_uart;
// Deep sleep and its two wake sources (task p3-04): the timer and a start bit from the radio on
// GPIO8. USB is not one of them - this chip cannot wake from deep sleep on a cable - so a boot
// that finds a host stays awake instead (components/platform/sleeper.cpp).
platform::EspSleeper g_sleeper;
// The two the copied agent library asks of a platform (task p4-04, interfaces/entropy.h and
// interfaces/heap.h): thirty-two random bits and the heap's two figures.
platform::EspEntropy g_entropy;
platform::EspHeap g_heap;
// The four figures of the internal heap behind the console's seam (task p11-02,
// core/console/heap_report.h): what `heap` prints, and the one line below. It holds no state and
// has no initialiser that runs - a vtable pointer is constant-initialised - so nothing of it is
// in the heap programme tools/heap_at_start.py replays.
platform::EspHeapReport g_heap_report;
platform::FlashPartition g_rakfw;
platform::FlashPartition g_journal_partition;
// The `coredump` partition (task p11-03): what the panic handler wrote the last time this board
// died, read by core/health/coredump.h so that a rolled-back trial can say what killed it and so
// that `coredump` on the console says it too. Read at the boot that makes a rollback's finding
// and when a command asks; **erased by nothing but a typed `coredump erase confirm`**.
platform::FlashPartition g_coredump_partition;
core::health::CoreDump* g_coredump = nullptr;
// The settings (task p3-03): NVS behind interfaces::KeyValue, and the table over it. The store
// is above the application slots in partitions.csv, so a factory image cannot reach it
// (docs/ARCHITECTURE.md §11).
platform::NvsKeyValue g_nvs;
// The two application slots and ESP-IDF's rollback (task p2-04). The trial's uplink condition
// is below, with the HTTPS client: a TLS handshake with the configured server (task p3-02), or -
// with the agent built - one heartbeat the server answered (task p4-05); the server is the
// `server.url` setting (task p3-03): unset, the host is empty and the condition does not apply
// (core/update/uplink_check.h says why that is the rule and not a gap).
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

// The time of day (task p3-05, core/cycle/network_clock.h): filled from the cellular network by
// the wake cycle's step 6 and given to the radio by its step 7. It answers "I have no time to
// give" until the modem has been asked and the answer passed task p3-01's window - and again
// after every wake from deep sleep, because the monotonic clock this is kept against restarts at
// zero and a time carried across a sleep by arithmetic over a counter that restarted is exactly
// the clock spec/LINK-PROTOCOL.md §5.4 warns about. `rak time set <epoch>` is still a person's
// way in, and it does not go through here.
core::cycle::NetworkClock g_wall(g_clock);

// The backend seam (task p3-05, interfaces/backend.h). With the agent built (WALTER_AGENT, the
// default) the MeshMaster agent is behind it - `agent::AgentBackend`, one wake of the copied
// library's loop, constructed in app_main below (task p4-05, docs/ARCHITECTURE.md §13). Without
// it the implementation does nothing, which is phases 0-3's unit.
#if !WALTER_AGENT
core::cycle::NullBackend g_backend;
#endif

#if WALTER_AGENT
// The agent's two function pointers (task p4-04). `mm::logSetSink` and the library's `pump` take
// plain function pointers, so the console they reach is this one pointer - there is one console
// on this board and it is a function static of app_main, which is why this is set rather than
// initialised.
core::console::Console* g_console = nullptr;

// Where a line the copied library formatted goes: this port, and the four kilobytes `dump`
// prints. The line arrives stamped and with its own CR LF.
void agentLogLine(const char* line) {
  if (g_console != nullptr) g_console->agentLine(line);
}

// What the library's idle waits run: the radio's port read, and one turn of the console unless a
// command of it is what is waiting (core/console/console.h, `pump`). Task p4-05 hands the same
// function to the library's runner.
void agentPump() {
  if (g_console != nullptr) g_console->pump();
}

// `log.level` as the library's own level: the same four words and the same order
// (core/settings/keys.h's Level, agent/Log.h's LogLevel), held together by a static_assert so
// that a fifth on either side is a failed build and not a quiet mismatch.
mm::LogLevel agentLogLevel(core::settings::Level level) {
  static_assert(static_cast<int>(core::settings::Level::kError) == static_cast<int>(mm::LogLevel::Error) &&
                    static_cast<int>(core::settings::Level::kWarn) == static_cast<int>(mm::LogLevel::Warn) &&
                    static_cast<int>(core::settings::Level::kInfo) == static_cast<int>(mm::LogLevel::Info) &&
                    static_cast<int>(core::settings::Level::kDebug) == static_cast<int>(mm::LogLevel::Debug),
                "the console's log levels and the library's are the same four, in the same order");
  return static_cast<mm::LogLevel>(level);
}

// The library's objects that must never be stack locals (docs/library.md §6: the runner alone is
// tens of kilobytes of buffers every job shares, far more than the task stack). The loop is small
// - mostly its 64-entry observation ring - and stays at namespace scope. The runner is ~64 kB and
// the scratch a kept image is built in (agent/IRetained.h) ~20 kB, and this board has no PSRAM
// (sdkconfig.defaults) and too little internal DRAM to hold both in the static `.bss` segment
// beside the console and the update verdict. So they are taken from the heap once at boot - never
// a local, never freed, so the whole-run lifetime the library asks for still holds - and reached
// through a namespace-scope pointer. See tasks/phase-4/reports/05.md, decisions.
//
// *When* they are taken matters as much as where. The runner is one block of ~66 kB, and only the
// heap region right after `.bss` (~94 kB) can hold it. ESP-IDF takes the main task's stack from the
// same heap when it creates the task, which is after the static initialisers and before app_main.
// Taken in app_main, they found the stack already there: with the stack at 16 kB
// (sdkconfig.defaults) the runner no longer fit, and the boot ended in `abort()` inside `operator
// new` (23 October 2026, the first board). Taken here, in the initialisers, the heap is still
// whole: both blocks come first, and the stack goes to a region that has room for it.
mm::JobRunner* g_agent_jobs = new mm::JobRunner();
mm::AgentLoop g_agent_loop;
std::uint8_t* g_kept_scratch = new std::uint8_t[agent::kKeptScratchBytes]();
#endif

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

  // And then the heap, as the second line of the boot and before this task has done anything
  // but open its port (task p11-02; docs/ARCHITECTURE.md §14). v0.1.3 never printed a line at
  // all, because ESP-IDF could not create *this* task, and the figure that would have shown it
  // coming is the largest free block - which nothing of this firmware used to print.
  //
  // It is not the arrangement `tools/heap_at_start.py` judges, and it cannot be: by now the main
  // task's stack and TCB are taken, FreeRTOS's idle and timer tasks are made, the task watchdog
  // is up and the 22 308-byte region the ROM's stack was in has been registered as a heap
  // (`heap_caps_enable_nonos_stack_heaps`, at the top of `main_task`). The model's last section
  // is an upper bound on this line, about 7 kB above it; `docs/bench.md` §1 holds a board to the
  // figures read out of this firmware booting in the pinned image's QEMU, which is where this
  // line's own numbers came from.
  {
    const core::console::HeapReport::Figures h = g_heap_report.read();
    std::printf("heap: %lu bytes free, largest free block %lu (internal, 8-bit), the main task's "
                "%lu-byte stack already taken ('heap')\n", static_cast<unsigned long>(h.free),
                static_cast<unsigned long>(h.largest_block),
                static_cast<unsigned long>(h.stack_bytes));
  }

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
  // The trial's third condition (S-6), which is `server.url` (task p3-03) - the console's
  // begin() hands the host over, and unset it answers "not configured" and the condition does
  // not apply. With the agent built it is **one heartbeat the server answered `ok`**, told by the
  // agent's wake (task p4-05, walter_agent/agent_uplink_check.h); without it, phase 3's TLS
  // handshake with the configured server (core/update/tls_check.h).
#if WALTER_AGENT
  static agent::AgentUplinkCheck g_uplink;
#else
  static core::update::TlsHandshakeCheck g_uplink(tls_socket, &trust, g_clock);
#endif
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
      // the same verdict the USB transfer uses. The console's `fetch`, and the agent's offer.
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

  // The `coredump` partition (task p11-03): read only, except for a typed `coredump erase
  // confirm`. It is opened before the self-update is built, because the finding at boot is what
  // needs it: the boot that comes back after a trial the bootloader gave up on is the one boot
  // whose reset reason is the reset that ended the trial.
  if (g_coredump_partition.begin(platform::kCoreDumpPartitionName,
                                 platform::kCoreDumpPartitionSubtype)) {
    static core::health::CoreDump dumps(g_coredump_partition);
    g_coredump = &dumps;
    std::printf("coredump: at 0x%06" PRIx32 " (%" PRIu32 " KB); 'coredump' says whether there is "
                "one and what it says\n",
                g_coredump_partition.address(), g_coredump_partition.size() / 1024);
  }

  // The verdict's anchor (task p2-01, core/image/verdict.h). `image receive` now calls
  // judgeManifest() and judgeFile(), so the six checks have a caller at last and --gc-sections
  // would keep them anyway; the anchor stays because it costs one volatile read and because
  // tools/verify.sh's reading of the linker map - no signing symbol, the verifier present - was
  // written against it.
  const void* volatile verdict_anchor = core::image::verdictAnchor();
  (void)verdict_anchor;

#if WALTER_AGENT
  // The agent core (task p4-01, docs/ARCHITECTURE.md §13): the exit-node firmware's job logic at
  // a pinned commit. Since task p4-05 it is behind the backend seam and called by every wake, so
  // nothing has to anchor it in the image.
  std::printf("agent-core: %s, behind the backend seam ('agent wake', 'agent jobs', 'agent state')\n",
              mm::kAgentCoreVersion);
#else
  std::printf("agent: not built (WALTER_AGENT=0): the seam is core/cycle/null_backend.h\n");
#endif

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
                                                g_restart, battery, health, g_coredump);
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
  // is the only object in this firmware that sleeps**, and task p3-05's wake cycle below is what
  // drives it; the console's `sleep now`, `sleep <seconds>`, `wake`, `power` and `stay` still
  // reach it directly.
  static core::cycle::DutyCycle duty(g_sleeper, radio_pins, radio, uplink, health, g_clock, g_wall,
                                     g_restart, g_retained, &supervisor, g_update,
                                     self_update_ptr, g_journal, &settings);
  // What this wake means, before anything is done about it. For a wake on the radio's line this
  // listens for up to §8's whole repeat window (about eleven seconds), which is why it is here -
  // after the link's port is open - and not at the top of this function.
  duty.begin();
  // The bound on a wake, armed: this is the one line task p3-05 adds
  // (tasks/phase-3/reports/04.md). From here a wake that has not reached sleep in five minutes -
  // or in the longer bound a climb or an update may raise it to - is ended by a reset, and the
  // next boot's record says who asked. A USB host holds it off, and so does `stay`.
  duty.guard().arm();

  // What woke this unit, counted and kept across a power cut (task p13-02,
  // core/cycle/wake_counters.h): the four totals a heartbeat carries beside the library's own
  // counters. Built here, where the wake has just been worked out, and handed to the wake cycle
  // below - which counts this wake at its first step and writes the totals at its last, and only
  // when one of them moved.
  static core::cycle::WakeCounters g_wake_counters(g_nvs.ready() ? &g_nvs : nullptr);
  g_wake_counters.begin();

  // The readings of every wake's step 4, kept while there is nobody to give them to (task
  // p3-05, core/cycle/telemetry_ring.h): a bounded ring in retained storage, oldest dropped
  // first, with the count of what was dropped - and the asks this firmware refused and has not
  // yet been able to tell anybody about.
  static core::cycle::TelemetryRing readings(g_retained);
  readings.begin();

#if WALTER_AGENT
  // The agent (tasks p4-02 to p4-05; docs/ARCHITECTURE.md §13), constructed here because the
  // wake cycle below takes the backend it is behind:
  //
  //   the mesh       the library's IMesh as one request of spec/LINK-PROTOCOL.md §6 per call,
  //                  over the link client; the unit's own repeater served through the local door
  //                  (task p4-02);
  //   the server     the library's IUplink as the seven calls of the agent API over the HTTPS
  //                  client above, the token read from `server.token` for each call and wiped
  //                  (task p4-03). A namespace-scope object: about 9 kB of buffers, never a local;
  //   the platform   the millisecond clock, the hardware's random bits, the heap's two figures,
  //                  phase 3's network clock as the wall clock, the library's log lines into this
  //                  console's ring; `delayMs` pumps the link and the console (task p4-04);
  //   the attention  the Walter's own, raised by a 401 and by a restart the library asks for and
  //                  does not get (core/cycle/agent_attention.h);
  //   the identity   who this exit node is, from the radio: §5.6's key, HELLO's version and
  //                  `CLI get radio`. Nothing of it is typed by a person (task p4-04);
  //   the ring       the loop's own (`AgentLoop::begin` hands it to the mesh); §7's drain fills
  //                  it at every wake and `agent obs pull` by hand;
  //   the jobs       the four `twinoak.*` job types on the image store and the update machines,
  //                  and the kept image's store in the tail of the `rakfw` partition - the
  //                  48 KB past the image store's layout (walter_agent/region_store.h);
  //   the backend    one `AgentLoop::wake()` per exchange: the seam (task p4-05).
  static core::cycle::AgentAttention agent_attention(&duty.reason(), g_journal);
  static agent::LinkMesh mesh(radio, g_clock);
  static agent::LinkMeshProbe mesh_probe(mesh);
  static agent::ModemUplink server(http_client, settings, g_clock, g_wall, core::version());
  static agent::ModemUplinkProbe agent_probe(server);
  server.raiseAttentionThrough(&agent_attention);
  // What is kept for the asking (task p13-02): the last eight calls that did not come back 2xx,
  // and the four counters a heartbeat carries about what woke this unit. Both are one value of
  // the settings store each, written only when they move; both are read back at `begin()`, which
  // the backend does for the list and the line below does for the counters.
  static agent::CallLog g_calls(g_nvs.ready() ? &g_nvs : nullptr);
  server.recordFailuresIn(&g_calls);
  server.countWakesFrom(&g_wake_counters.totals());
  static agent::WalterPlatform agent_platform(g_clock, g_wall, g_entropy, g_heap,
                                              &agent_attention);
  static agent::Identity agent_identity(radio, core::version());
  static agent::Observations agent_observations(radio, mesh);
  agent::FirmwareJobs::Parts job_parts;
  job_parts.store = g_store;
  job_parts.update = g_update;
  job_parts.self = self_update_ptr;
  job_parts.fetch = g_fetch;
  job_parts.journal = g_journal;
  job_parts.supervisor = &supervisor;
  job_parts.guard = &duty.guard();
  static agent::FirmwareJobs firmware_jobs(job_parts, radio_pins, slots, radio, server, settings,
                                           g_clock, g_wall);
  static agent::IdentityProbe identity_probe(agent_identity, mesh, agent_observations,
                                             agent_platform, &duty, &firmware_jobs);
  identity_probe.countWakesFrom(&g_wake_counters.totals());
  // What a sleep keeps: the window after the image store's layout in the `rakfw` partition
  // (core/image/store.h, kStoreBytes), when the partition is there and has room.
  agent::RegionStore* kept = nullptr;
  if (g_store != nullptr && g_rakfw.size() > core::image::kStoreBytes) {
    static agent::RegionStore kept_store(g_rakfw, core::image::kStoreBytes,
                                         g_rakfw.size() - core::image::kStoreBytes);
    if (kept_store.usable()) kept = &kept_store;
  }
  // The runner and the kept-image scratch were taken from the heap before this task existed (see
  // their declaration): a deep sleep is a full reboot, so they are taken again at every boot.
  agent::AgentBackend::Parts agent_parts;
  agent_parts.mesh = &mesh;
  agent_parts.server = &server;
  agent_parts.platform = &agent_platform;
  agent_parts.identity = &agent_identity;
  agent_parts.observations = &agent_observations;
  agent_parts.jobs = g_agent_jobs;
  agent_parts.loop = &g_agent_loop;
  agent_parts.firmware = &firmware_jobs;
  agent_parts.trial = &g_uplink;
  agent_parts.state_store = g_nvs.ready() ? &g_nvs : nullptr;
  agent_parts.calls = &g_calls;
  agent_parts.kept = kept;
  agent_parts.kept_scratch = g_kept_scratch;
  agent_parts.kept_scratch_len = agent::kKeptScratchBytes;
  agent_parts.duty = &duty;
  agent_parts.self = self_update_ptr;
  agent_parts.update = g_update;
  static agent::AgentBackend backend(agent_parts, g_clock, g_wall);
  interfaces::Backend& the_backend = backend;
#else
  interfaces::Backend& the_backend = g_backend;
#endif

  // **One wake, start to finish** (task p3-05, docs/cycle.md): the ten steps in order, each
  // bounded, each outcome written down, ending in sleep. Everything above is handed to it as an
  // interface; the backend is the agent's (task p4-05), or the one that does nothing.
  static core::cycle::WakeCycle cycle(duty, radio, uplink, the_backend, readings, g_wall, g_clock,
                                      &supervisor, g_update, self_update_ptr, g_fetch, g_store,
                                      g_journal, &settings);
  cycle.countWakesIn(&g_wake_counters);

  // The console, on the USB port, in this task: one byte at a time, the radio's port read every
  // time round, the uptime kept in retained storage about once a second. Returning is not an
  // option here (the console is this task); the task watchdog watches the idle tasks, which run
  // while this one waits for a byte.
  // The battery is the last argument of phase 5's (task p5-03): `rak provision` refuses on a low
  // or falling reading by the update's rule, and accepts an unknown one as `--first-install`
  // does. The agent's seams into the console (tasks p4-02 to p4-05) come after it.
#if WALTER_AGENT
  static core::console::Console console(g_usb, radio_pins, health, g_clock, g_restart, radio,
                                        slots, g_store, g_receiver, g_update, g_journal,
                                        self_update_ptr, &supervisor, &uplink, &http_client,
                                        g_fetch, &trust, &settings, &g_uplink, &duty, &cycle,
                                        &battery, &mesh_probe, &agent_probe, &identity_probe,
                                        &backend, &tls_socket.rotation(), &g_wall,
                                        &g_modem_trace, &g_heap_report, g_coredump);
  // The library's log goes where every other line of this port goes: the console's print(), and
  // so the four kilobytes `dump` prints. At the `log.level` setting, because the library has a
  // level of its own and a person who turned this port down meant the whole port.
  g_console = &console;
  // What `twinoak.report` reads the unit out of (task p13-01): this console's own sections. It
  // is handed over here and not in `job_parts` above, because the console is built after the
  // firmware jobs - it is given the agent's probes.
  firmware_jobs.readTheUnitFrom(&console);
  agent_platform.beginLog(&agentLogLine, agentLogLevel(settings.level()));
  // The loop begun over the seams (docs/library.md §6): the runner and the loop, the identity,
  // the extension, the state the last sleep left in NVS, the kept image read back. The pump the
  // library's waits run is the console's: the radio's port read and one turn of the console.
  backend.begin(&agentPump);
  // Who this exit node is, as the boot line says it; the backend's begin() has already handed
  // the key to the mesh's local door (task p4-02's `setOwnKey`) and to the observation drain.
  if (agent_identity.known()) {
    std::printf("agent: this exit node is %s, firmware %s%s\n", agent_identity.publicKeyHex(),
                agent_identity.companionFirmware(),
                agent_identity.radio().known ? "" : " (the radio block was not read)");
  } else if (agent_identity.sessionUp()) {
    std::printf("agent: the radio has no identity yet; a heartbeat would carry no key and no job "
                "types ('agent id')\n");
  } else {
    // Nothing has been asked of the radio in this boot, so there is no session to read an
    // identity out of. The wake's step 2 opens one - and so does every wake of the agent, the
    // console's `agent wake` included, before it claims anything (task p8-04).
    std::printf("agent: no identity read yet (the radio has not been asked anything in this "
                "boot); the next wake asks, or 'rak hello' then 'agent id'\n");
  }
  // ...and, when it did, how long this boot was told the board was away (task p10-01): the figure
  // the library took off a wait the server asked for. 0 is a wake whose sleep has no known length
  // - the radio's line, a cable, a power-on - and `agent state` says the same in a sentence.
  std::printf("agent: the loop's state %s the last sleep (%lu s away); the kept image's store %s\n",
              backend.stateSurvived() ? "survived" : "did not survive",
              static_cast<unsigned long>(backend.timeAwayMs() / 1000u),
              kept != nullptr ? "is the rakfw partition's tail" : "is not there");
#else
  static core::console::Console console(g_usb, radio_pins, health, g_clock, g_restart, radio,
                                        slots, g_store, g_receiver, g_update, g_journal,
                                        self_update_ptr, &supervisor, &uplink, &http_client,
                                        g_fetch, &trust, &settings, &g_uplink, &duty, &cycle,
                                        &battery, nullptr, nullptr, nullptr, nullptr,
                                        &tls_socket.rotation(), &g_wall, &g_modem_trace,
                                        &g_heap_report, g_coredump);
#endif
  console.begin();
  // One turn of the loop before the wake, because the trial's first condition is "the main loop
  // was reached with the board in the safe state" (core/update/self_update.h) and the console is
  // what says so. A wake that found itself on trial has to be able to meet that condition before
  // its step 9 tries to finish the trial.
  console.tick();

  // And then the wake itself. **With a USB host on the port a person is here**: the console runs
  // and nothing sleeps by itself (task p3-04 - this chip cannot wake from deep sleep on a cable,
  // so a boot that finds a host stays awake while it is there and for a minute after it goes).
  // Without one, this is a unit on a mast: one pass of the wake cycle, which ends in deep sleep
  // and **does not return**. If it does return - the sleep was refused, the Walter is restarting
  // into another slot, or the bound ended the wake - the loop below takes over and the bound is
  // what ends it.
  if (!duty.wake().usb_host && !duty.guard().staying()) {
    const core::cycle::Pass& pass = cycle.run();
    std::printf("the wake did not end in sleep: %s\n", core::cycle::describe(pass.ending));
    std::fflush(stdout);
  } else {
    std::printf("a USB host is on the port, so this wake is a person's: the console runs and "
                "nothing sleeps by itself ('cycle dry' says what a wake would do, 'cycle now' "
                "runs one)\n");
  }
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
    // The bound on the wake, and the USB host it is held off by: asked every time round (task
    // p3-04, core/cycle/wake_guard.h). A wake that has not reached sleep inside its bound is
    // ended here by a reset, and the journal is what carries any unfinished work across it.
    (void)duty.tick();
    const std::uint32_t up = health.uptimeS();
    if (up != last_tick_s) {
      last_tick_s = up;
      health.tick();
    }
  }
}
