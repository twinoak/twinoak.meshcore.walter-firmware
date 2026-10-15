// A modem that fails in every way a real one does (task p3-01). An interfaces::Uplink driven by
// a script rather than by AT commands, so that core/uplink's policy - the sequence, the
// deadlines, the budget, the backoff - can be run against silence, a missing card, a locked
// card, a network that never registers us, one that registers and drops, one that gives no time
// and one that gives 1970. Reused by every later task of this phase.
//
// Two things make it a useful fake and not a stub:
//
//   **Every call costs time.** Each one spends `cost_ms` of the FakeClock it is given, and a
//   call whose cost would pass its deadline advances the clock *to the deadline* and fails
//   there. So "every call returns by its deadline" is a thing the tests can check, and so is
//   "the wake's budget is never exceeded".
//
//   **The network is a function of the clock.** registration() answers from how long ago
//   attach() was called, not from a counter the test winds on; a drop happens at a time; so a
//   policy that polls, waits and gives up is tested against something that is actually moving.
#pragma once

#include <cstdint>

#include "fakes/fake_clock.h"
#include "interfaces/uplink.h"

namespace sim {

class FakeModem final : public interfaces::Uplink {
 public:
  explicit FakeModem(fakes::FakeClock& clock) : clock_(clock) {}

  // -- the script ----------------------------------------------------------------------------

  // Nothing answers at all: every call runs to its deadline and fails there. `deaf_until_reset`
  // is the same until resetModem() is called, which is the case `modem-silent`'s "after a modem
  // reset, once per wake" exists for.
  bool deaf = false;
  bool deaf_until_reset = false;

  // What the card says, and whether the card's own commands get through.
  interfaces::SimState sim_state = interfaces::SimState::kReady;
  bool sim_answers = true;            // false: `AT+CPIN?` itself times out
  bool unlock_answers = true;         // false: `AT+CPIN=<pin>` itself times out
  const char* correct_pin = "1234";   // a PIN equal to this is accepted, anything else refused

  // The radio access technology the modem is set to, and whether it will take a new one.
  interfaces::Rat rat_now = interfaces::Rat::kLteM;
  bool rat_settable = true;

  // The context and the attach.
  bool pdp_definable = true;
  bool attachable = true;             // false: `AT+CGATT=1` fails
  bool never_registers = false;
  bool denied = false;                // the network refuses us outright
  std::uint32_t registers_after_ms = 3000;  // after attach(), how long until registered
  bool drops_after_registering = false;
  std::uint32_t drops_after_ms = 5000;      // after registering, how long until it drops

  // Packet data.
  bool data_ever_ready = true;
  std::uint32_t data_ready_after_ms = 0;  // after registering

  // The clock. `time_answered` false is "the modem said nothing"; otherwise `time_epoch` is
  // whatever it said - 0 for 1970, a sane one for a network that knows the time.
  bool time_answered = true;
  std::uint32_t time_epoch = 1791936000u + 86400u;  // a day after the firmware's own floor
  // The first `asks_before_time` asks answer with `early_time_epoch` instead: a modem that has
  // not heard NITZ yet.
  std::uint32_t asks_before_time = 0;
  std::uint32_t early_time_epoch = 0;

  // The signal, the identity and what the network grants.
  bool signal_answered = true;
  interfaces::Signal signal_reading = makeSignal();
  bool identity_answered = true;
  interfaces::Identity identity_numbers = makeIdentity();
  bool power_saving_answered = true;
  bool power_saving_granted = true;
  std::uint8_t granted_tau_octet = 0x06;     // 60 minutes (unit 000, value 6)
  std::uint8_t granted_active_octet = 0x01;  // 2 seconds (unit 000, value 1)

  // What a call costs when it works. A real AT command is milliseconds; a reset is seconds.
  std::uint32_t cost_ms = 20;
  std::uint32_t reset_cost_ms = 4000;

  // -- what happened -------------------------------------------------------------------------
  int presents = 0;
  int resets = 0;
  int sims = 0;
  int unlocks = 0;       // how many times a PIN was sent to the card
  int op_states = 0;
  int rats_read = 0;
  int rats_set = 0;
  int pdps = 0;
  int attaches = 0;
  int registrations = 0;  // how many times the buffered state was looked at
  int data_asks = 0;
  int time_asks = 0;
  int signals = 0;
  int identities = 0;
  int power_savings = 0;
  int take_downs = 0;
  int timeouts = 0;       // calls that ran to their deadline and failed there
  int overruns = 0;       // calls that returned *after* their deadline: this fake makes none
  // The seconds this modem spent with its radio on and no network: the battery's bill.
  std::uint64_t searching_ms = 0;
  // The last request that reached it, for the power-saving tests.
  interfaces::PowerSavingRequest last_request{};

  // -- interfaces::Uplink ---------------------------------------------------------------------
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

  // -- the Walter's clock, and why this needs telling about it --------------------------------
  //
  // **The deadlines this modem is given are on the Walter's own monotonic clock**, which restarts
  // at zero at every wake from deep sleep (`interfaces/clock.h`, and host_test's benches model it
  // with a boot clock over the world's). This object's arithmetic has to be on the same clock, or
  // a second wake would hand it deadlines in its own past and every call would time out at once.
  // A bench that walks more than one wake calls this at every boot; one that walks a single wake
  // need not, because the two clocks are then the same.
  void walterBooted(std::uint64_t world_ms);

  // -- for the tests --------------------------------------------------------------------------
  // The default readings, as functions so that a test can start from them and change one field.
  static interfaces::Signal makeSignal();
  static interfaces::Identity makeIdentity();
  // A wake of its own: what a deep sleep does to a modem that was left attached. The script is
  // not touched - only what the modem knows.
  void forgetEverything();

 private:
  // Spends `cost` of the clock, bounded by the deadline. False when the deadline came first: the
  // clock is then exactly at the deadline, which is what a call that times out looks like.
  bool spend(std::uint32_t cost, std::uint64_t deadline);
  // Now, on the Walter's clock (see walterBooted()).
  std::uint64_t now() const { return clock_.nowMs() - base_ms_; }
  bool answers() const { return !deaf && !deaf_until_reset; }
  bool registeredNow() const;
  bool dataNow() const;
  void stopSearching();

  fakes::FakeClock& clock_;
  std::uint64_t base_ms_ = 0;  // where the Walter's own clock began, in the world's time
  bool started_ = false;
  bool unlocked_ = false;
  interfaces::OpState op_ = interfaces::OpState::kOff;
  bool attached_ = false;
  std::uint64_t attached_at_ms_ = 0;
  std::uint64_t registered_at_ms_ = 0;
  bool was_registered_ = false;
  std::uint64_t searching_since_ms_ = 0;
  bool searching_ = false;
  std::uint32_t time_asked_ = 0;
};

}  // namespace sim
