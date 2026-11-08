// What a wake *means*, worked out before anything is done about it (task p3-04;
// docs/ARCHITECTURE.md §10). The chip says "the timer", "a pin", "a panic"; this says "the
// check-in", "the radio wants us for reason 4", "a chattering line", "a person with a cable",
// "we power-cycled and reset the radio with us".
//
//   timer            the check-in.
//   the radio's line a start bit on WALTER_RX woke us (§8: the radio sends 0x00 0x00, waits
//                    300 ms, then the event every 1000 ms, ten times, "and a Walter coming out of
//                    deep sleep needs a few hundred milliseconds before its UART listens, which
//                    is why the event is repeated and why losing the first ones is expected").
//                    So the line is listened to for the *whole* repeat window; a valid
//                    EV_ATTENTION gives the wake its reason, and nothing valid inside the window
//                    is a **spurious wake**, counted. Spurious wakes are rate-limited: after
//                    kSpuriousPerWindow of them the line is disarmed as a wake source until the
//                    next timer wake, and that is said in the journal and by `wake`. A
//                    chattering line - a loose UART, a radio being flashed, a scanner talking to
//                    nobody - must not be able to keep the Walter awake.
//   USB              a person. This board cannot *wake* on a cable (see below), so this is a
//                    boot that found a host: stay awake while it is there, plus a grace period.
//   power-on, reset  our own power-up glitches GPIO1-14 low for about 60 us and that reaches
//                    nRESET (docs/ARCHITECTURE.md §2 rule 5), so the radio was reset with us and
//                    is still booting: it is not to be judged yet. The supervisor already knows
//                    this (core/supervisor/supervisor.h, Silence::kJustWoke) and this says the
//                    same thing to whoever runs the wake.
//   a crash, a
//   watchdog         noted, and counted: faults in a row **lengthen** the next sleep rather than
//                    shortening it (core/power/interval_policy.h, kMaxFaultDoublings).
//
// **USB is not a wake source on this board.** `esp_sleep_enable_usb_wakeup()` answers
// `ESP_ERR_NOT_SUPPORTED` on the ESP32-S3 - `sleep_modes.c` guards it on `SOC_PM_SUPPORT_USB_WAKEUP`
// and `components/soc/esp32s3/include/soc/soc_caps.h` does not define it - and the one USB wake
// ESP-IDF has is light sleep's anyway (`esp_sleep.h`: `ESP_SLEEP_WAKEUP_USB`, "light sleep
// only"). So the rule is: **a person presses reset, and a boot that finds a USB host stays
// awake.** docs/bench.md §15 and tasks/phase-3/reports/04.md say it where a person will look.
//
// What crosses the sleep is one record in `interfaces::RetainedSlot::kDutyCycle` - the last ten
// wakes with their reasons and durations, the spurious count, the disarm, the faults in a row,
// and the band `core/power/interval_policy.h` was last in (that policy is a pure function and
// keeps nothing itself). A power cut clears it, and `recordSurvived()` says so.
//
// Logic: no ESP-IDF include, no heap, every loop bounded.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/health/health.h"
#include "core/link/codec/wire.h"
#include "core/power/interval_policy.h"
#include "interfaces/clock.h"
#include "interfaces/retained.h"
#include "interfaces/sleeper.h"

namespace core {
namespace cycle {

enum class Wake : std::uint8_t {
  kUnknown = 0,
  kTimer = 1,      // the sleep timer: the check-in
  kAttention = 2,  // the radio's line, and a valid EV_ATTENTION inside the repeat window
  kSpurious = 3,   // the radio's line, and nothing valid inside it
  kUsb = 4,        // a boot that found a USB host: a person
  kPowerOn = 5,    // a power cut or the first power-up - and the glitch reset the radio with us
  kReset = 6,      // the module's reset pin, or a restart somebody asked for
  kCrash = 7,      // a panic, an assertion, a brownout
  kWatchdog = 8,   // a watchdog: the chip's, or this firmware's own bound on a wake
  kCount,
};

const char* name(Wake wake);
const char* describe(Wake wake);  // one line, for `wake` and the boot

// §8: the event is sent 300 ms after the zeros and then every kAttentionRepeatMs, kAttentionRepeats
// times - so the last of them is 300 + 9 * 1000 ms after the line first moved. The window is that
// plus a second's room, because the 300 ms is measured by the radio's clock and not ours.
constexpr std::uint32_t kAttentionWindowMs =
    300 + link::kAttentionRepeats * link::kAttentionRepeatMs;
static_assert(kAttentionWindowMs >=
                  300 + (link::kAttentionRepeats - 1) * link::kAttentionRepeatMs,
              "the window has to outlast the last repeat");

// How often the port is looked at inside that window. 50 ms: a frame is 115200 baud and a few
// tens of bytes, so nothing is missed, and 11 s of waiting is 220 reads and not 220 000.
constexpr std::uint32_t kAttentionPollMs = 50;

// How many spurious line wakes before the line is disarmed. Three. The window is "until the next
// timer wake", which is what the task asks for and the only window a Walter can measure - it has
// no clock it can trust across a deep sleep (interfaces/clock.h). With the shortest interval
// (core/power/interval_policy.h, kPlentyMinutes = 15) that is three in a quarter of an hour at
// worst, each costing the attention window and nothing else; with a longer interval it is three
// a day. A PLACEHOLDER in the same sense as the intervals: it is the interval table that makes
// it a rate.
constexpr std::uint8_t kSpuriousPerWindow = 3;

// How many wakes are remembered, with their reasons and durations: what `wake` prints. Ten, as
// the task asks. Eight bytes each, inside one retained record.
constexpr std::size_t kWakesRemembered = 10;

// **The deep-sleep timer's own error**, as a percentage, taken off the seconds the last sleep
// asked for before `timeAwayMs()` believes any of it (task p10-01). The rule that figure is
// measured under is the agent library's: *never more than really passed* (the exit-node
// firmware's task p1.10-42, `mm::AgentLoop::restore`'s `awayMs`), and the clock a deep sleep is
// timed by is not one this firmware can check afterwards.
//
// What the documents say, read in the build image at v5.5.5:
//
//   * **the source is the internal RC oscillator.**
//     `$IDF_PATH/components/esp_hw_support/port/esp32s3/Kconfig.rtc` has `choice RTC_CLK_SRC`
//     with `default RTC_CLK_SRC_INT_RC`, "Internal 136 kHz RC oscillator", and
//     `sdkconfig.defaults` sets no `CONFIG_RTC_CLK_SRC_*` - so this build takes that default.
//   * **it is calibrated against the main crystal at every boot**, over
//     `CONFIG_RTC_CLK_CAL_CYCLES` cycles - 1024 for that source (the same file) - so what is
//     left is not the oscillator's absolute tolerance but its drift between that calibration and
//     the wake.
//   * **what moves it is temperature.** "The frequency stability of this clock source is
//     affected by temperature fluctuations, so time may drift in both Deep-sleep and Light-sleep
//     modes" (`$IDF_PATH/docs/en/api-reference/system/system_time.rst`, *RTC Timer Clock
//     Sources*). **No tolerance is published for it**: the only figure on that page is the
//     high-resolution timer's APB source, "a frequency deviation of less than ±10 ppm", and that
//     is a different clock which a sleep does not use.
//
// So the figure is not read off a document; what a document settles is the *direction*. Believing
// less than was slept leaves a sliver of a wait owed for one more wake; believing more calls a
// server back before the wait it asked for. **Ten per cent** is the bound taken - an order of
// magnitude above any plausible temperature coefficient of an RC oscillator over one interval -
// and it costs nothing against the waits this exists for: a 30 s to 120 s backoff is spent whole
// by 90 % of the shortest interval this firmware sleeps (`core/power/interval_policy.h`,
// kPlentyMinutes = 15, so 810 s of a 900 s sleep).
constexpr std::uint32_t kTimerErrorPercent = 10;
static_assert(kTimerErrorPercent < 100, "the believed part of a sleep has to be positive");

// §8's attention, as this object keeps it. The same three fields the event carries.
struct Attention {
  std::uint8_t reason = link::attention::kNone;
  std::uint16_t detail = 0;
  std::uint8_t repeat = 0;
};

// Where an attention comes from. The one place in this firmware that decodes an EV_ATTENTION and
// keeps it is the supervisor (core/supervisor/supervisor.h, which §8's "acknowledged by the next
// request" also belongs to), so the adapter in core/cycle/attention_source.h is what a Walter
// uses; a test hands over its own.
class AttentionSource {
 public:
  virtual ~AttentionSource() = default;
  // Reads whatever the radio has sent and records any attention in it. **Climbs nothing** and
  // sends nothing: a wake that is still working out why it woke has not decided anything yet.
  virtual void listen() = 0;
  // The oldest attention held, removed. False when there is none.
  virtual bool take(Attention* out) = 0;
};

// One wake, as it is remembered. Plain data: it is stored as bytes.
struct WakeEntry {
  Wake wake = Wake::kUnknown;
  std::uint8_t attention = link::attention::kNone;
  std::uint16_t detail = 0;
  // How long that wake lasted, in seconds. **0 means it never reached sleep** - it was ended by
  // a reset, a crash or the hard bound - which is itself worth knowing, and `wake` prints it as
  // such rather than as "no time at all".
  std::uint32_t lasted_s = 0;
};

struct WakeInfo {
  Wake wake = Wake::kUnknown;
  Attention attention{};        // when `wake` is kAttention
  bool usb_host = false;        // a host was on the port when begin() looked
  bool radio_may_be_booting = false;  // our own reset went to the radio: wait before judging it
  bool line_disarmed = false;   // the radio's line is not to be armed for the next sleep
  bool line_just_disarmed = false;  // ...and this wake is what disarmed it
  bool line_rearmed = false;    // ...and this wake is what armed it again (a timer wake)
  std::uint32_t spurious_in_window = 0;
  std::uint32_t spurious_total = 0;
  std::uint32_t faults_in_a_row = 0;
  std::uint32_t wakes = 0;      // wakes counted since the record began (a power cut)
  std::uint32_t listened_ms = 0;  // how much of the attention window was spent listening
  interfaces::WokeBy woke_by = interfaces::WokeBy::kNotFromSleep;
  health::ResetReason reset = health::ResetReason::kUnknown;
};

class WakeReason {
 public:
  WakeReason(interfaces::Retained& retained, interfaces::Clock& clock);

  // What this wake means. `boot` is `core::health::Health::thisBoot()` - the reset reason, the
  // chip's wake cause and, for a software reset, who asked for it, which is how this firmware's
  // own bound on a wake is told from a person's `reboot`. `attentions` may be nullptr (a build
  // with no supervisor): a line wake is then spurious by definition, which is the honest answer -
  // nothing could have read the event. Blocks for at most kAttentionWindowMs, and only for a
  // wake on the line.
  void begin(const health::BootRecord& boot, interfaces::WokeBy woke, bool usb_host,
             AttentionSource* attentions);
  bool begun() const { return begun_; }
  bool recordSurvived() const { return survived_; }

  const WakeInfo& info() const { return info_; }
  Wake wake() const { return info_.wake; }

  // May the radio's line arm the next sleep? `setting_on` is `wake.radio`
  // (core/settings/keys.h). False when a person turned it off, or when the spurious limit
  // disarmed it until the next timer wake.
  bool lineMayArm(bool setting_on) const { return setting_on && !info_.line_disarmed; }

  // The band the last wake ended in, and `saving`'s every-Nth counter: what
  // core/power/interval_policy.h's Inputs need and does not keep itself.
  power::Band band() const { return band_; }
  std::uint32_t wakesInSaving() const { return wakes_in_saving_; }
  // A decision was taken: the band it settled on is remembered for the next wake's hysteresis.
  void bandDecided(power::Band band);
  // A wake in which the modem was actually used: `saving`'s counter starts again. Called by
  // whoever ran the wake, so that a wake the uplink's backoff skipped does not count as a use.
  void modemWasUsed();
  // A wake in which it was not, and the band was `saving`: one more towards the Nth.
  void modemWasSkipped();

  // The Walter's **own** attention, which is not §8's and has nothing to do with the radio: the
  // agent's two (a token the server refused, a restart the library asked for and did not get).
  // `core/cycle/agent_attention.h` is what raises it and what the bits mean; it lives in this
  // record because it has to cross a sleep and RTC memory has no slot left
  // (`interfaces/retained.h`, kDutyCycle). Raising it is additive; the wake record that carries
  // it takes it, so one raise is reported once.
  void raiseAgentAttention(std::uint8_t bits);
  std::uint8_t agentAttention() const { return record_.agent_attention; }
  void clearAgentAttention();

  // The wake is ending in a sleep: its duration goes into this wake's entry, the seconds the
  // timer is being asked for are written down for the *next* boot to read, and the record is
  // stored. Called by core/cycle/sleep.h on the one way out of a wake.
  //
  // `asked_s` is not a nicety and has no default: it is the only measure of a sleep this board
  // has afterwards. `interfaces/clock.h` says why - `esp_timer` restarts at zero at every wake
  // from deep sleep - and the agent's library has to be told how long the board was away, or a
  // wait the server asked for is spent five seconds at a time across wakes that send nothing
  // (`timeAwayMs()` below; the unit of 8 November 2026).
  void endingInSleep(std::uint32_t lasted_s, std::uint32_t asked_s);

  // What the sleep this boot followed asked its timer for, in seconds. 0 when there was none
  // this firmware wrote down: the first boot, a power cut, or a record of another shape.
  std::uint32_t lastSleepAskedS() const { return asked_s_; }

  // **How long this board was away before this boot**, in milliseconds, and never more than
  // really passed - which is the rule `mm::AgentLoop::restore(state, awayMs)` is handed it under
  // (the exit-node firmware's task p1.10-42).
  //
  //   a timer wake   has slept the interval the last sleep asked for, less the timer's own error
  //                  (`kTimerErrorPercent` above). That is the one sleep this board can put a
  //                  number on.
  //   anything else  **0**. A wake on the radio's line, a spurious one, a person's cable, a
  //                  power-on, a reset, a crash, a watchdog: the sleep was cut short by whatever
  //                  woke us and nothing here knows how much of it had passed. 0 is "no time
  //                  passed", which is the behaviour this firmware had before it measured any,
  //                  and it errs the safe way - a wait is owed a little longer, never less.
  std::uint32_t timeAwayMs() const;

  // The last ten wakes, newest first. 0 is this wake.
  std::size_t count() const { return count_; }
  const WakeEntry& at(std::size_t index) const;

  // The bench and the tests: everything forgotten, as a power cut leaves it.
  void forget();

 private:
  // interfaces::RetainedSlot::kDutyCycle. Plain data, 100 bytes, inside kRetainedRecordMax -
  // which costs nothing, because platform/retained.cpp gives every slot kRetainedRecordMax bytes
  // whatever its record holds (interfaces/retained.h).
  //
  // **It grew by four bytes in task p10-01**, and a record of another size in the same slot is
  // not the same record (interfaces/retained.h), so the first boot of 0.1.3 reads no record and
  // `recordSurvived()` is false for that one wake: the wake log, the band's hysteresis, the
  // spurious count and the faults in a row all start again, exactly as after a power cut. That is
  // the rule working, not failing - the old bytes meant something else - and the cost is one
  // wake at the `unknown` band's interval, named in the report and in docs/bench.md.
  struct Record {
    std::uint32_t wakes;
    std::uint32_t spurious_total;
    // What the sleep before this boot asked its timer for, in seconds; 0 for a boot that followed
    // no sleep of ours. Written by endingInSleep(), read by timeAwayMs(): the whole of what this
    // board knows about a sleep once `esp_timer` has started again at zero.
    std::uint32_t asked_s;
    std::uint8_t count;             // how many of items[] are real
    std::uint8_t spurious_in_window;
    std::uint8_t disarmed;          // 0 or 1
    std::uint8_t faults_in_a_row;
    std::uint8_t band;              // core::power::Band, for the next wake's hysteresis
    std::uint8_t wakes_in_saving;
    // The Walter's own attention (task p4-04, core/cycle/agent_attention.h): one of the two
    // bytes this record kept spare. It is here and not in a slot of its own because
    // interfaces/retained.h has none left and because what it has to do - survive a sleep, be
    // read by the wake that follows - is what this record is for.
    std::uint8_t agent_attention;
    std::uint8_t spare;
    WakeEntry items[kWakesRemembered];
  };
  static_assert(sizeof(WakeEntry) == 8, "a wake entry is eight bytes");
  static_assert(sizeof(Record) <= interfaces::kRetainedRecordMax,
                "the duty cycle's record has to fit a retained slot");

  void load();
  void save();
  void push(Wake wake, const Attention& attention);
  // The attention window: listen, and answer what was heard.
  Wake listenForAttention(AttentionSource* attentions, Attention* out);

  interfaces::Retained& retained_;
  interfaces::Clock& clock_;
  Record record_{};
  WakeInfo info_{};
  power::Band band_ = power::Band::kUnknown;
  std::uint32_t wakes_in_saving_ = 0;
  // The record's `asked_s` as `load()` found it: what the sleep *this boot followed* asked for.
  // Latched, because endingInSleep() overwrites the record's copy with the seconds of the sleep
  // this boot is about to begin, and a reader after that would get the wrong one.
  std::uint32_t asked_s_ = 0;
  std::size_t count_ = 0;
  bool begun_ = false;
  bool survived_ = false;
};

}  // namespace cycle
}  // namespace core
