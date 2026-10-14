// interfaces::Uplink over `walter-modem` (task p3-01): the one place in this firmware that names
// a WalterModem symbol. Thin on purpose - each method is the library calls and the mapping of
// their results - because nothing here can be tested without a SIM and a tower. The sequence,
// the deadlines, the budget and the backoff are core/uplink/policy.h, which is tested on the
// host against host_test/sim/fake_modem.h.
//
// -- what the library is, read at 1.5.0 --------------------------------------------------------
//
// It is a class of static methods with one UART, three FreeRTOS tasks and state in RTC memory.
// `begin(uart_port_t, watchdogTimeout)` (WalterModem.cpp:4152):
//
//   * installs the UART driver on the port it is given with hardware flow control and a
//     256-byte receive buffer, and sets the pins from its own Kconfig - RX 14, TX 48, RTS 21,
//     CTS 47, reset 45, which are the module's internal wiring (the Walter datasheet's table of
//     internal pins: LTE_UART0_RX/TX/RTS/CTS and LTE_RESET), none of them one of our seven;
//   * starts three static tasks: `uart_rx_task`, `queueProcessingTask` and
//     `eventProcessingTask`;
//   * sets the direction of the modem's reset pin and enables the chip's deep-sleep pad hold for
//     it, and holds the pad again inside `reset()`. **The deep-sleep hold enable is a chip-wide
//     switch** and platform/pins.cpp uses it for the radio's pads (docs/ARCHITECTURE.md §2 rule
//     5); the order the duty cycle has to keep is in tasks/phase-3/reports/01.md. (The three
//     function names are deliberately not written here: tools/verify.sh greps the whole of
//     components/ and main/ for a GPIO call outside components/platform/, and that grep is not
//     weakened to let a comment past it.)
//   * on a cold boot calls `reset()` - a 10 ms pulse on the modem's reset pin and a wait for
//     `+SYSSTART`, seconds - and on a wake from deep sleep `_sleepWakeup()` instead;
//   * sets `AT+CMEE=1` and `AT+CEREG=5`, which is what makes the PSM timers the network grants
//     arrive in the unsolicited `+CEREG` report at all.
//
// **watchdogTimeout is passed 0, deliberately.** A non-zero one makes the library call
// `esp_task_wdt_reconfigure()` and `esp_task_wdt_add(NULL)` (WalterModem.cpp:4157-4184), which
// replaces the task watchdog this firmware configured in sdkconfig.defaults - 30 s, panic, both
// idle tasks - with one of the library's choosing. The header says "a minimum timeout of 300 is
// enforced"; the code enforces `CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS/1000 + 5`, which is 35.
// Both facts are in tasks/phase-3/reports/01.md.
//
// -- why this firmware sets the library's command attempts to 1 --------------------------------
//
// The command task retransmits a command that answered `ERROR` or `+CME ERROR`, not only one
// that timed out: `_processModemRSP` sets `WALTER_MODEM_CMD_STATE_RETRY_AFTER_ERROR` on both
// (WalterModem.cpp:2079 and :2093) and `_processModemCMD` then sends it again until
// `cmd->maxAttempts` (:1731-1748). `maxAttempts` is `CONFIG_WALTER_MODEM_DEFAULT_CMD_ATTEMPTS`
// for every public call - `_queueModemCMD` takes it as an argument but is private, and
// `unlockSIM` does not pass one.
//
// So at the library's default of 3, **one call to `unlockSIM` with a wrong PIN sends
// `AT+CPIN=<pin>` three times, and three refusals block the SIM card**: a PUK, the operator's
// letter and a person at the mast. The task's rule - "this firmware sends a PIN at most once per
// PIN value" - cannot be kept with a library that turns one send into three, and there is no
// per-command way to ask for fewer. `sdkconfig.defaults` therefore sets
// `CONFIG_WALTER_MODEM_DEFAULT_CMD_ATTEMPTS=1`, with the reason beside it. What it costs: a
// command lost to noise is not retransmitted by the library. That is the cheaper half - the
// modem's UART is a trace inside the module with hardware flow control, and the retry this
// firmware really relies on is the next wake's (core/uplink/failure.h).
//
// -- the deadlines, honestly -------------------------------------------------------------------
//
// Every blocking call of the library waits on a condition variable **with no timeout of its own**
// (WalterDefines.h, `_returnAfterReply`); what gives up is the command task, after
// CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS per attempt and CONFIG_WALTER_MODEM_DEFAULT_CMD_ATTEMPTS
// attempts. So a deadline cannot cut a call short. What this class does is check the wall
// *before* each call and refuse to begin one it is already past; core/uplink/policy.h checks it
// after as well and reports the overrun. The static_assert below holds the library's own
// configuration to the ceiling the policy's budget was sized for, so that a Kconfig change
// cannot quietly make the budget a lie.
#pragma once

#include <cstdint>

#include "sdkconfig.h"

#include "core/uplink/policy.h"
#include "interfaces/clock.h"
#include "interfaces/uplink.h"

#if defined(CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS) && \
    defined(CONFIG_WALTER_MODEM_DEFAULT_CMD_ATTEMPTS)
static_assert(static_cast<std::uint64_t>(CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS) *
                      CONFIG_WALTER_MODEM_DEFAULT_CMD_ATTEMPTS <=
                  core::uplink::kModemCallCeilingMs,
              "one blocking modem call can outlast the ceiling core/uplink/policy.h sized the "
              "wake's budget against: raise kModemCallCeilingMs and kWakeBudgetMs together, or "
              "lower CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS");
#endif

namespace modem {

// The ESP32-S3's UART the modem is spoken to on. **Not UART1**: that is the radio's
// (components/platform/uart.cpp), and UART0 is the chip's own console port. The modem's pins go
// through the GPIO matrix, so the port number is ours to choose - QuickSpot's examples use
// UART_NUM_1 only because nothing else in them does.
constexpr int kModemUartPort = 2;

class WalterUplink final : public interfaces::Uplink {
 public:
  explicit WalterUplink(interfaces::Clock& clock) : clock_(clock) {}

  // Whether the library has been started in this boot. present() starts it when it has not,
  // which is how a wake the backoff skips costs the modem nothing at all.
  bool started() const { return started_; }

  interfaces::UplinkState state() const override;
  bool present(std::uint64_t deadline_ms) override;
  bool resetModem(std::uint64_t deadline_ms) override;
  interfaces::SimState sim(std::uint64_t deadline_ms) override;
  interfaces::SimUnlock unlockSim(const char* pin, std::uint64_t deadline_ms) override;
  bool rat(std::uint64_t deadline_ms, interfaces::Rat* out) override;
  bool setRat(interfaces::Rat rat, std::uint64_t deadline_ms) override;
  bool opState(interfaces::OpState state, std::uint64_t deadline_ms) override;
  bool definePdp(const char* apn, std::uint64_t deadline_ms) override;
  bool attach(std::uint64_t deadline_ms) override;
  interfaces::Registration registration() override;
  bool dataReady(std::uint64_t deadline_ms) override;
  bool networkTime(std::uint64_t deadline_ms, interfaces::NetworkTime* out) override;
  bool signal(std::uint64_t deadline_ms, interfaces::Signal* out) override;
  bool identity(std::uint64_t deadline_ms, interfaces::Identity* out) override;
  bool powerSaving(const interfaces::PowerSavingRequest& request, std::uint64_t deadline_ms,
                   interfaces::PowerSavingGrant* out) override;
  void takeDown() override;

 private:
  // Is there any point in beginning a call: false when the deadline has already passed.
  bool before(std::uint64_t deadline_ms) const { return clock_.nowMs() < deadline_ms; }

  interfaces::Clock& clock_;
  bool started_ = false;
  interfaces::OpState op_ = interfaces::OpState::kOff;
  bool data_ = false;
};

}  // namespace modem
