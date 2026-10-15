#include "sim/fake_modem.h"

#include <cstring>

#include "core/uplink/clock_window.h"

namespace sim {

namespace {

void copyInto(char* out, std::size_t size, const char* text) {
  std::size_t i = 0;
  for (; text[i] != 0 && i + 1 < size; ++i) out[i] = text[i];
  out[i] = 0;
}

}  // namespace

interfaces::Signal FakeModem::makeSignal() {
  interfaces::Signal s;
  s.rsrp_known = true;
  s.rsrp_dbm = -92;
  s.rsrq_known = true;
  s.rsrq_tenth_db = -105;  // -10.5 dB
  s.rssi_known = true;
  s.rssi_dbm = -71;
  copyInto(s.operator_name, sizeof s.operator_name, "TDC");
  s.band = 20;
  s.rat = interfaces::Rat::kLteM;
  return s;
}

interfaces::Identity FakeModem::makeIdentity() {
  interfaces::Identity id;
  // Numbers that are obviously invented and obviously searchable: the privacy test greps every
  // line the console printed for exactly these.
  copyInto(id.imei, sizeof id.imei, "357999001112223");
  copyInto(id.iccid, sizeof id.iccid, "8945020000000777666");
  copyInto(id.imsi, sizeof id.imsi, "238010000012345");
  return id;
}

void FakeModem::walterBooted(std::uint64_t world_ms) {
  base_ms_ = world_ms;
  // A wake begins, and nothing of the attach before the sleep is timed from: the radio was taken
  // down on the way out of that wake (core/cycle/sleep.h), so there is nothing left to time.
  attached_ = false;
  attached_at_ms_ = 0;
  registered_at_ms_ = 0;
  was_registered_ = false;
  searching_since_ms_ = 0;
  searching_ = false;
}

bool FakeModem::spend(std::uint32_t cost, std::uint64_t deadline) {
  const std::uint64_t now = this->now();
  if (now >= deadline) {
    ++timeouts;
    return false;
  }
  const std::uint64_t left = deadline - now;
  if (cost > left) {
    clock_.advance(left);
    ++timeouts;
    return false;
  }
  clock_.advance(cost);
  return true;
}

void FakeModem::stopSearching() {
  if (!searching_) return;
  searching_ms += now() - searching_since_ms_;
  searching_ = false;
}

// Is the network holding us right now: one piece of arithmetic, asked by state(),
// registration() and dataReady() so that the three cannot disagree.
bool FakeModem::registeredNow() const {
  if (!started_ || op_ != interfaces::OpState::kFull) return false;
  if (!attached_ || denied || never_registers) return false;
  const std::uint64_t now = this->now();
  const std::uint64_t registered_at = attached_at_ms_ + registers_after_ms;
  if (now < registered_at) return false;
  if (drops_after_registering && now >= registered_at + drops_after_ms) return false;
  return true;
}

bool FakeModem::dataNow() const {
  if (!registeredNow() || !data_ever_ready) return false;
  const std::uint64_t registered_at = attached_at_ms_ + registers_after_ms;
  return now() >= registered_at + data_ready_after_ms;
}

interfaces::UplinkState FakeModem::state() const {
  if (!started_ || op_ != interfaces::OpState::kFull) return interfaces::UplinkState::kDown;
  if (!registeredNow()) return interfaces::UplinkState::kSearching;
  return dataNow() ? interfaces::UplinkState::kUp : interfaces::UplinkState::kRegistered;
}

bool FakeModem::present(std::uint64_t deadline_ms) {
  ++presents;
  // A deaf modem is one the deadline has to catch: that is the whole of `modem-silent`.
  if (!answers()) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  started_ = true;
  return true;
}

bool FakeModem::resetModem(std::uint64_t deadline_ms) {
  ++resets;
  // A modem that is dead rather than merely wedged does not come back from a reset either: the
  // wait for its `+SYSSTART` runs to the deadline, as the real one's would.
  if (deaf) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(reset_cost_ms, deadline_ms)) return false;
  // A reset is what cures `deaf_until_reset`, and it loses everything the modem knew - which is
  // what the real one does too (WalterModem.cpp, reset(): the registration state, the operational
  // state and the PDP contexts all go back to their defaults).
  deaf_until_reset = false;
  forgetEverything();
  started_ = true;
  return true;
}

interfaces::SimState FakeModem::sim(std::uint64_t deadline_ms) {
  ++sims;
  if (!answers() || !sim_answers) {
    spend(0xFFFFFFFFu, deadline_ms);
    return interfaces::SimState::kUnknown;
  }
  if (!spend(cost_ms, deadline_ms)) return interfaces::SimState::kUnknown;
  if (unlocked_ && sim_state == interfaces::SimState::kPinRequired) {
    return interfaces::SimState::kReady;
  }
  return sim_state;
}

interfaces::SimUnlock FakeModem::unlockSim(const char* pin, std::uint64_t deadline_ms) {
  ++unlocks;
  if (!answers() || !unlock_answers) {
    spend(0xFFFFFFFFu, deadline_ms);
    return interfaces::SimUnlock::kFailed;
  }
  if (!spend(cost_ms, deadline_ms)) return interfaces::SimUnlock::kFailed;
  if (pin != nullptr && correct_pin != nullptr && std::strcmp(pin, correct_pin) == 0) {
    unlocked_ = true;
    return interfaces::SimUnlock::kAccepted;
  }
  return interfaces::SimUnlock::kRefused;
}

bool FakeModem::rat(std::uint64_t deadline_ms, interfaces::Rat* out) {
  ++rats_read;
  if (!answers()) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  if (out != nullptr) *out = rat_now;
  return true;
}

bool FakeModem::setRat(interfaces::Rat rat, std::uint64_t deadline_ms) {
  ++rats_set;
  if (!answers() || !rat_settable) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  rat_now = rat;
  return true;
}

bool FakeModem::opState(interfaces::OpState state, std::uint64_t deadline_ms) {
  ++op_states;
  if (!answers()) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  op_ = state;
  if (state == interfaces::OpState::kFull) {
    if (!searching_) {
      searching_ = true;
      searching_since_ms_ = now();
    }
  } else {
    stopSearching();
    attached_ = false;
  }
  return true;
}

bool FakeModem::definePdp(const char* apn, std::uint64_t deadline_ms) {
  ++pdps;
  (void)apn;
  if (!answers() || !pdp_definable) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  return spend(cost_ms, deadline_ms);
}

bool FakeModem::attach(std::uint64_t deadline_ms) {
  ++attaches;
  if (!answers() || !attachable) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  attached_ = true;
  attached_at_ms_ = now();
  return true;
}

interfaces::Registration FakeModem::registration() {
  ++registrations;
  if (!answers()) return interfaces::Registration::kUnknown;
  if (op_ != interfaces::OpState::kFull || !attached_) {
    return interfaces::Registration::kNotSearching;
  }
  if (denied) return interfaces::Registration::kDenied;
  if (!registeredNow()) return interfaces::Registration::kSearching;
  if (!was_registered_) {
    was_registered_ = true;
    registered_at_ms_ = attached_at_ms_ + registers_after_ms;
    stopSearching();
  }
  return interfaces::Registration::kRegistered;
}

bool FakeModem::dataReady(std::uint64_t deadline_ms) {
  ++data_asks;
  if (!answers()) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  return dataNow();
}

bool FakeModem::networkTime(std::uint64_t deadline_ms, interfaces::NetworkTime* out) {
  ++time_asks;
  if (!answers()) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  if (!time_answered) return false;
  const std::uint32_t epoch =
      time_asked_ < asks_before_time ? early_time_epoch : time_epoch;
  ++time_asked_;
  if (out != nullptr) {
    // The fake answers with a number and lets core/uplink/clock_window.h judge it, exactly as
    // the real one does (components/modem/walter_uplink.cpp).
    out->epoch = epoch;
    out->origin = core::uplink::judgeTime(true, epoch);
  }
  return true;
}

bool FakeModem::signal(std::uint64_t deadline_ms, interfaces::Signal* out) {
  ++signals;
  if (!answers() || !signal_answered) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  if (out != nullptr) *out = signal_reading;
  return true;
}

bool FakeModem::identity(std::uint64_t deadline_ms, interfaces::Identity* out) {
  ++identities;
  if (!answers() || !identity_answered) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  if (out != nullptr) *out = identity_numbers;
  return true;
}

bool FakeModem::powerSaving(const interfaces::PowerSavingRequest& request,
                            std::uint64_t deadline_ms, interfaces::PowerSavingGrant* out) {
  ++power_savings;
  last_request = request;
  if (!answers() || !power_saving_answered) {
    spend(0xFFFFFFFFu, deadline_ms);
    return false;
  }
  if (!spend(cost_ms, deadline_ms)) return false;
  if (out != nullptr) {
    out->requested = request.mode;
    out->asked = true;
    // The network answers with timers of its own only when PSM was asked for, and only when it
    // feels like it: the +CEREG report is unsolicited (WalterModem.cpp:1887).
    const bool granted = request.mode == interfaces::PowerSavingMode::kPsm && power_saving_granted;
    out->granted = granted;
    out->tau_octet = granted ? granted_tau_octet : 0;
    out->active_octet = granted ? granted_active_octet : 0;
  }
  return true;
}

void FakeModem::takeDown() {
  ++take_downs;
  stopSearching();
  op_ = interfaces::OpState::kOff;
  attached_ = false;
  was_registered_ = false;
}

void FakeModem::forgetEverything() {
  stopSearching();
  started_ = false;
  unlocked_ = false;
  op_ = interfaces::OpState::kOff;
  attached_ = false;
  attached_at_ms_ = 0;
  registered_at_ms_ = 0;
  was_registered_ = false;
  time_asked_ = 0;
}

}  // namespace sim
