// The way out of the unit, as core/ sees it (task p3-01; docs/ARCHITECTURE.md §9): the steps of
// getting onto the LTE-M network and the four things worth asking once it is there - what time
// is it, how good is the signal, who am I, what power saving did the network grant - each with a
// deadline. Behind it is `walter-modem` over the module's second UART (components/modem/
// walter_uplink.cpp), or - in the host tests - a fake that fails in every way a real modem does
// (host_test/sim/fake_modem.h).
//
// **These are the steps, not the policy.** The order they are taken in, how long each may take,
// what a refusal means and when to try again is core/uplink/ (policy.h, failure.h); what the
// firmware above phase 3 sees is `core::uplink::UplinkPolicy` - bring it up, what time is it,
// how good is the signal, take it down. The split is the same one interfaces/wall_clock.h makes:
// the interface carries what the world can be asked, core/ carries what the answers mean.
//
// A deadline is an absolute monotonic millisecond count - interfaces::Clock::nowMs() plus however
// long the caller is willing to wait - and is the *latest* moment the call may return. An
// implementation that cannot interrupt what it has started says so in its own documentation; the
// real one can only check the wall before and after a library call (components/modem/
// walter_uplink.h, and tasks/phase-3/reports/01.md under "Not verified here").
//
// Nothing here identifies a subscription in passing: identity() exists for `lte id`, a person's
// command, and what comes out of it is never logged (core/uplink/policy.h, *Privacy*).
#pragma once

#include <cstdint>

namespace interfaces {

// What the modem is, as far as this interface can tell. The policy's own idea of "up" is
// core/uplink/policy.h's; this is the modem's.
enum class UplinkState : std::uint8_t {
  kDown = 0,        // the radio is off, or the modem has not been started
  kSearching = 1,   // the radio is on and no network has registered us
  kRegistered = 2,  // registered, no packet data proved yet
  kUp = 3,          // registered, with a packet data context that has an address
};

inline const char* describe(UplinkState state) {
  switch (state) {
    case UplinkState::kDown: return "down";
    case UplinkState::kSearching: return "searching";
    case UplinkState::kRegistered: return "registered (no data yet)";
    case UplinkState::kUp: return "up";
  }
  return "?";
}

// What the SIM card says of itself. The modem's own list is longer (a PIN2, a PUK, a network
// lock...); everything that is not "ready" and not "no card" is a lock a person has to open,
// which is the only distinction the policy acts on.
enum class SimState : std::uint8_t {
  kUnknown = 0,     // the question could not be asked, or the answer was not understood
  kAbsent = 1,      // no card, or a card that does not answer
  kReady = 2,       // usable
  kPinRequired = 3, // a PIN would unlock it
  kBlocked = 4,     // a PUK or worse: a person with the operator's letter, never this firmware
};

inline const char* describe(SimState state) {
  switch (state) {
    case SimState::kUnknown: return "unknown";
    case SimState::kAbsent: return "no card";
    case SimState::kReady: return "ready";
    case SimState::kPinRequired: return "a PIN is required";
    case SimState::kBlocked: return "blocked (a PUK is required)";
  }
  return "?";
}

// What came of sending a PIN. kRefused is the one that must never be repeated: three refusals
// block the card (core/uplink/policy.h).
enum class SimUnlock : std::uint8_t {
  kAccepted = 0,
  kRefused = 1,  // the card said no: this PIN is wrong and is never sent again
  kFailed = 2,   // the command itself did not get through; the card said nothing
};

// What the modem's radio is doing - its "operational state", `AT+CFUN` (WalterModemOpState).
// kNoRf is the one worth a name of its own: the modem is awake and the SIM can be read, and
// nothing is transmitting. The modem's own list has a manufacturing state too, which this
// firmware has no business in.
enum class OpState : std::uint8_t {
  kOff = 0,   // minimum: the radio off
  kNoRf = 1,  // awake, no radio: where the SIM is read and the context defined
  kFull = 2,  // the radio on
};

inline const char* describe(OpState state) {
  switch (state) {
    case OpState::kOff: return "off (minimum)";
    case OpState::kNoRf: return "awake, no radio";
    case OpState::kFull: return "full";
  }
  return "?";
}

// 3GPP registration, as +CEREG reports it (TS 27.007 §7.2). The modem's list is longer; these
// are the four outcomes the policy distinguishes.
enum class Registration : std::uint8_t {
  kNotSearching = 0,  // the radio is on and not looking (or off)
  kSearching = 1,
  kRegistered = 2,    // home or roaming
  kDenied = 3,        // the network refused: a subscription matter, not a coverage one
  kUnknown = 4,
};

inline const char* describe(Registration state) {
  switch (state) {
    case Registration::kNotSearching: return "not searching";
    case Registration::kSearching: return "searching";
    case Registration::kRegistered: return "registered";
    case Registration::kDenied: return "denied";
    case Registration::kUnknown: return "unknown";
  }
  return "?";
}

// The radio access technology. LTE-M only unless a setting says otherwise (task p3-01); NB-IoT
// is here because the modem has it and a setting may one day want it.
enum class Rat : std::uint8_t {
  kLteM = 0,
  kNbIot = 1,
  kAuto = 2,
  kUnknown = 3,
};

inline const char* describe(Rat rat) {
  switch (rat) {
    case Rat::kLteM: return "LTE-M";
    case Rat::kNbIot: return "NB-IoT";
    case Rat::kAuto: return "automatic";
    case Rat::kUnknown: return "unknown";
  }
  return "?";
}

// Where a time came from. The modem answers +CCLK with its own default until the network has
// told it otherwise, so "the modem said something" and "the network knows what time it is" are
// different facts and this says which (core/uplink/clock_window.h holds the window a time must
// be inside to be believed at all).
enum class TimeOrigin : std::uint8_t {
  kNone = 0,     // the modem gave no time at all
  kNetwork = 1,  // a time inside the window: the network's, by NITZ
  kDefault = 2,  // the modem's own default (1970, or its build date): not a time
};

inline const char* describe(TimeOrigin origin) {
  switch (origin) {
    case TimeOrigin::kNone: return "none";
    case TimeOrigin::kNetwork: return "the network";
    case TimeOrigin::kDefault: return "the modem's own default (not the network's)";
  }
  return "?";
}

struct NetworkTime {
  std::uint32_t epoch = 0;  // Unix seconds, UTC
  TimeOrigin origin = TimeOrigin::kNone;
};

// What the modem can say about the signal. Each reading has its own "known": a modem that has
// not registered gives RSSI and nothing else, and a zero that means "not measured" is how a
// policy table ends up with a number nobody took.
struct Signal {
  bool rsrp_known = false;
  std::int16_t rsrp_dbm = 0;       // Reference Signal Received Power, dBm
  bool rsrq_known = false;
  std::int16_t rsrq_tenth_db = 0;  // Reference Signal Received Quality, tenths of a dB
  bool rssi_known = false;
  std::int16_t rssi_dbm = 0;       // [-113, -51] dBm
  char operator_name[20] = {};     // empty when the modem does not say
  std::uint8_t band = 0;           // the LTE band number, 0 when unknown
  Rat rat = Rat::kUnknown;
};

// IMEI, ICCID and IMSI: the numbers on the label of the box and on the operator's invoice. They
// identify a subscription, so they are read when a person asks and are written nowhere else
// (core/uplink/policy.h, *Privacy*; a host test greps everything the policy prints).
struct Identity {
  char imei[16] = {};   // WalterModemIdentity::imei
  char iccid[23] = {};  // WalterModemSIMCardID::iccid
  char imsi[16] = {};   // WalterModemRspData::imsi
};

// What power saving to ask the network for. The two timers are the 3GPP octets, not seconds:
// bits 8-6 a unit, bits 5-1 a value (3GPP TS 24.008 §10.5.7.4a for the extended T3412 and
// §10.5.7.3 for T3324). core/uplink/psm.h turns seconds into one and back, and is where the
// units are written down.
enum class PowerSavingMode : std::uint8_t {
  kOff = 0,   // no PSM, no eDRX: the modem is switched off between wakes instead (the default)
  kPsm = 1,   // PSM with the two timers below
};

inline const char* describe(PowerSavingMode mode) {
  switch (mode) {
    case PowerSavingMode::kOff: return "off (the modem is switched off between wakes)";
    case PowerSavingMode::kPsm: return "PSM";
  }
  return "?";
}

struct PowerSavingRequest {
  PowerSavingMode mode = PowerSavingMode::kOff;
  std::uint8_t tau_octet = 0;     // T3412 extended: how long the modem may sleep
  std::uint8_t active_octet = 0;  // T3324: how long it stays reachable after a transmission
};

// What the network granted. The modem learns it from the +CEREG unsolicited report, which only
// arrives when the network says something, so `granted` is false until it has - asking is not
// getting (3GPP TS 24.008: PSM timers are requested, allocated and may be refused).
struct PowerSavingGrant {
  PowerSavingMode requested = PowerSavingMode::kOff;
  bool asked = false;      // the request reached the modem
  bool granted = false;    // the network answered with timers of its own
  std::uint8_t tau_octet = 0;
  std::uint8_t active_octet = 0;
};

class Uplink {
 public:
  virtual ~Uplink() = default;

  // What the modem is, from what this object has seen. Costs nothing and asks nothing.
  virtual UplinkState state() const = 0;

  // -- getting there -------------------------------------------------------------------------
  // Start the driver if it is not started, and ask the modem whether it is there at all (the
  // bare `AT`). False means no answer by the deadline: `modem-silent`.
  virtual bool present(std::uint64_t deadline_ms) = 0;

  // Reset the modem and wait for it to come back. The one remedy for silence, and at most once
  // per wake (core/uplink/policy.h).
  virtual bool resetModem(std::uint64_t deadline_ms) = 0;

  // The SIM card. Asking needs the modem awake but not its radio on.
  virtual SimState sim(std::uint64_t deadline_ms) = 0;

  // Send a PIN. **The policy sends one at most once per PIN**: a card blocks after three
  // refusals and opening it again needs the operator's PUK and a person.
  virtual SimUnlock unlockSim(const char* pin, std::uint64_t deadline_ms) = 0;

  // The radio access technology the modem should use. Reading it is cheap; setting it makes the
  // modem reset itself, so the policy only sets it when it is wrong.
  virtual bool rat(std::uint64_t deadline_ms, Rat* out) = 0;
  virtual bool setRat(Rat rat, std::uint64_t deadline_ms) = 0;

  // The modem's operational state: the radio off, awake without it, or on.
  virtual bool opState(OpState state, std::uint64_t deadline_ms) = 0;

  // The packet data context this unit uses, by name. An empty APN means "let the network
  // choose", which is what a Danish LTE-M SIM wants until a setting says otherwise.
  virtual bool definePdp(const char* apn, std::uint64_t deadline_ms) = 0;

  // Attach to the packet domain. Returning true is not registration: that is the network's to
  // give, and registration() is how it is waited for.
  virtual bool attach(std::uint64_t deadline_ms) = 0;

  // Registration as the modem last heard it. **Buffered and instant**: this is what a wait for
  // the network is a poll of, which is why that wait is the one step whose deadline is exact.
  virtual Registration registration() = 0;

  // Is there packet data: a context with an address. False means `no-data`.
  virtual bool dataReady(std::uint64_t deadline_ms) = 0;

  // -- what is worth asking once it is up ----------------------------------------------------
  // The time, with where it came from. True when the modem answered at all - `out->origin` says
  // whether what it answered is a time (TimeOrigin::kNetwork) or its own default.
  virtual bool networkTime(std::uint64_t deadline_ms, NetworkTime* out) = 0;

  virtual bool signal(std::uint64_t deadline_ms, Signal* out) = 0;

  // The subscription's numbers, for `lte id` and nothing else.
  virtual bool identity(std::uint64_t deadline_ms, Identity* out) = 0;

  // Ask for power saving and say what came back. `out->granted` is false when the network has
  // not answered with timers; that is not a failure.
  virtual bool powerSaving(const PowerSavingRequest& request, std::uint64_t deadline_ms,
                           PowerSavingGrant* out) = 0;

  // -- and down again ------------------------------------------------------------------------
  // The radio off and the context detached, as far as it goes. Never fails loudly: a take-down
  // is what happens on the way out of a wake, including the ways out that are failures.
  virtual void takeDown() = 0;
};

}  // namespace interfaces
