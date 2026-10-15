// Going to sleep: **the one way out of a wake** (task p3-04; docs/ARCHITECTURE.md §10). Nothing
// else in this firmware calls `interfaces::Sleeper::sleep()`, and a host test greps for it.
//
// The order, and why each step is where it is:
//
//   1. **the journal**, when the sleep is worth a line (see *What is written down* below).
//   2. **`SLEEP_NOTICE`, as the last request** (`spec/LINK-PROTOCOL.md` §8): "The Walter sends it
//      as its last request before sleeping. From then until the next valid request arrives, the
//      RAK believes the Walter is asleep (`STATUS` flag bit 2): it sends **no** events except
//      `EV_BOOT` and `EV_ATTENTION`." It carries the seconds we expect to be away, so the radio
//      knows when to stop expecting us. Last, because every request after it would tell the
//      radio we are back.
//   3. **the modem down.** `UplinkPolicy::takeDown()` - always safe to call, and this is the way
//      out of every wake, failures included. The library's own `sleep()` is *not* used: it arms
//      the timer and nothing else, so neither the radio's line nor a reset would bring the Walter
//      back, and it gives nothing a chance to hold a pad, write the journal or tell the radio
//      first (`tasks/phase-3/reports/01.md`, *What its `sleep()` does*, and 04's answer).
//   4. **the pins safe, and then held** (§2 rules 1 and 5): `RadioPins::holdForSleep()`, with the
//      radio's line armed when it is to wake us. TX included - pulled up, so the radio's receiver
//      does not see a break for hours.
//   5. **sleep.** On the board this does not return.
//
// -- when it refuses ----------------------------------------------------------------------------
//
// "If an update of either MCU is mid-step, the step is finished or left at a journalled point
// first." Two of those are refusals here rather than waits, because waiting is the caller's job
// and the caller is the wake cycle:
//
//   the radio's update is running   `RadioUpdate::active()`: it journals before every state it
//                                   enters, so a reset between states is safe - but a step in
//                                   flight is not a journalled point. The caller steps it.
//   a transfer is open              `SelfUpdate::writing()`: the other application slot is open
//                                   for writing.
//   **this firmware is on trial**   and this one is not a nicety. `CONFIG_BOOTLOADER_APP_ROLLBACK_
//                                   ENABLE` is on (sdkconfig.defaults, S-6) and
//                                   `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` is off (its
//                                   own default, and this build does not set it), so **the
//                                   second-stage bootloader runs after a deep-sleep wake** and
//                                   `select_partition_number()` turns every `PENDING_VERIFY`
//                                   slot into `ABORTED`
//                                   (`$IDF_PATH/components/bootloader_support/src/bootloader_utility.c`
//                                   at v5.5.5, the `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` block).
//                                   A sleep during a trial is a rollback of the image being
//                                   tried. So: no sleep while on trial - `self confirm`,
//                                   `self reject` or the trial's own three conditions first.
//
// -- what is written down -----------------------------------------------------------------------
//
// Not every sleep. The journal is 1024 entries in one 64 KB partition and it exists so that an
// *update* is never guessed at ("it wraps without ever losing the entries of the update in
// progress", core/update/journal.h); a unit checking in every quarter of an hour writes 96 wakes
// a day, which would wrap the whole log in ten days and push every update and every incident out
// of it. So an *unremarkable* sleep - the same band and the same interval as the last one - is
// recorded in retained storage only, where `wake` reads it from, and the journal gets a line
// when there is news: the band changed, the interval changed, the radio's line was disarmed or
// armed again, the wake was a crash or a watchdog, or the sleep was refused. In steady state
// that is nothing; in a December it is a handful of entries a week, which is the story a person
// wants. Said again in `tasks/phase-3/reports/04.md` under *Deviations*.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/radio_pins.h"
#include "core/cycle/wake_reason.h"
#include "core/link/client/client.h"
#include "core/power/interval_policy.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "core/uplink/policy.h"
#include "interfaces/clock.h"
#include "interfaces/sleeper.h"
#include "interfaces/wall_clock.h"

namespace core {
namespace cycle {

// Why a sleep did not happen. kNone is the only one that means it did (and on the board, that
// the call never returned).
enum class SleepRefusal : std::uint8_t {
  kNone = 0,
  kNoSeconds,        // neither a time nor a source: a sleep only a reset would end
  kUpdateRunning,    // the radio's update machine has a step in flight
  kTransferOpen,     // the other application slot is open for writing
  kOnTrial,          // this firmware is on trial: a sleep would roll it back (see above)
  kSleeperRefused,   // the chip would not take the plan (the one that does not return, returned)
};

const char* describe(SleepRefusal refusal);

// What a sleep did, for the console and for whoever ran the wake.
struct SleepResult {
  SleepRefusal refusal = SleepRefusal::kNone;
  std::uint32_t seconds = 0;          // what the timer was asked for
  bool radio_line_armed = false;
  power::Band band = power::Band::kUnknown;
  bool notice_sent = false;           // SLEEP_NOTICE was answered
  link::client::Fault notice_fault = link::client::Fault::kClosed;
  bool pins_held = false;             // the pads are held in the safe state
  bool journalled = false;
  std::uint32_t awake_s = 0;          // how long this wake lasted
  std::uint32_t epoch = 0;            // when it is expected back, when there is a time of day
};

// -- the journal entry's payload, 32 bytes ------------------------------------------------------
//
//    0  what        u8   a Wrote
//    1  wake        u8   the Wake this sleep is ending
//    2  band        u8   a power::Band
//    3  flags       u8
//    4  interval_s  u32  what the timer was asked for
//    8  median_mv   u16  the reading behind the band; 0 when there was none
//   10  mean_ma     i16  positive = charging
//   12  at_epoch    u32  0 when there is no time of day
//   16  at_ms       u32  the monotonic ms of this entry, truncated: a gloss, never arithmetic
//   20  awake_s     u32  how long the wake lasted
//   24  attention   u8   §8's reason, when the wake was one
//   25  spurious    u8   spurious line wakes since the last timer wake
//   26  faults      u8   crash or watchdog wakes in a row
//   27  refusal     u8   a SleepRefusal: why it did not sleep
//   28  wakes       u32  wakes counted since the retained record began
constexpr std::size_t kPayloadBytes = 32;
static_assert(kPayloadBytes <= update::kPayloadMax, "the payload has to fit a journal entry");

enum class Wrote : std::uint8_t {
  kNone = 0,
  kSleeping = 1,      // going to sleep, and why it was worth a line
  kRefused = 2,       // the sleep was refused
  kLineDisarmed = 3,  // the radio's line is not a wake source until the next timer wake
  kLineArmed = 4,     // ...and a timer wake has armed it again
};
const char* describe(Wrote what);

constexpr std::uint8_t kFlagBandChanged = 0x01;
constexpr std::uint8_t kFlagIntervalChanged = 0x02;
constexpr std::uint8_t kFlagOverridden = 0x04;   // `wake.interval` was in force
constexpr std::uint8_t kFlagLineArmed = 0x08;
constexpr std::uint8_t kFlagCharging = 0x10;
constexpr std::uint8_t kFlagNoticeSent = 0x20;   // SLEEP_NOTICE was answered
constexpr std::uint8_t kFlagByHand = 0x40;       // `sleep now` or `sleep <seconds>`

struct Payload {
  Wrote what = Wrote::kNone;
  Wake wake = Wake::kUnknown;
  power::Band band = power::Band::kUnknown;
  std::uint8_t flags = 0;
  std::uint32_t interval_s = 0;
  std::uint16_t median_mv = 0;
  std::int16_t mean_ma = 0;
  std::uint32_t at_epoch = 0;
  std::uint32_t at_ms = 0;
  std::uint32_t awake_s = 0;
  std::uint8_t attention = 0;
  std::uint8_t spurious = 0;
  std::uint8_t faults = 0;
  SleepRefusal refusal = SleepRefusal::kNone;
  std::uint32_t wakes = 0;
};

void encode(const Payload& payload, std::uint8_t out[kPayloadBytes]);
bool decode(const update::Entry& entry, Payload* out);  // false unless it is a duty-cycle entry

// -- the object ---------------------------------------------------------------------------------

class Sleep {
 public:
  // The update machine, the self-update and the journal are pointers because a build can be
  // without them (no `rakfw` or no `journal` partition: main/ hands over nothing rather than a
  // machine over flash that is not there). Without a journal nothing is written down and the
  // retained wake log is all there is, which `wake` says.
  Sleep(interfaces::Sleeper& sleeper, board::RadioPins& pins, link::client::Client& radio,
        uplink::UplinkPolicy& uplink, WakeReason& wake, interfaces::Clock& clock,
        interfaces::WallClock& wall, update::RadioUpdate* update = nullptr,
        update::SelfUpdate* self = nullptr, update::Journal* journal = nullptr);

  // Go to sleep for `seconds`, with the radio's line armed when `arm_radio_line` says so, in the
  // band `decision` settled on. **On the board this does not return.** `by_hand` marks a
  // person's `sleep now` in the journal.
  SleepResult go(const power::Decision& decision, bool arm_radio_line, bool by_hand = false);
  // The same, for a number of seconds a person typed: the band is read but its interval is not.
  SleepResult goFor(std::uint32_t seconds, const power::Decision& decision, bool arm_radio_line);

  // Whether a sleep would be refused now, without doing anything about it: what `status` and the
  // wake cycle ask before they start one.
  SleepRefusal wouldRefuse() const;

  const SleepResult& last() const { return last_; }

 private:
  SleepResult sleepFor(std::uint32_t seconds, const power::Decision& decision,
                       bool arm_radio_line, bool by_hand);
  void journal(Wrote what, const SleepResult& r, const power::Decision& d, bool by_hand);
  bool worthALine(const power::Decision& d, const SleepResult& r, bool by_hand) const;

  interfaces::Sleeper& sleeper_;
  board::RadioPins& pins_;
  link::client::Client& radio_;
  uplink::UplinkPolicy& uplink_;
  WakeReason& wake_;
  interfaces::Clock& clock_;
  interfaces::WallClock& wall_;
  update::RadioUpdate* update_ = nullptr;
  update::SelfUpdate* self_ = nullptr;
  update::Journal* journal_ = nullptr;
  SleepResult last_{};
};

}  // namespace cycle
}  // namespace core
