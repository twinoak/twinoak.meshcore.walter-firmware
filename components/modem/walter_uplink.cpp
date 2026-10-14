#include "modem/walter_uplink.h"

#include "WalterModem.h"
#include "driver/uart.h"

#include "core/uplink/clock_window.h"
#include "core/uplink/psm.h"

namespace modem {

namespace {

// What the network granted, as the unsolicited `+CEREG` report brings it. The library's event
// task writes these and the main task reads them; each is a single byte or a flag, so there is
// nothing to tear. The handler has to be short - the library logs a warning past
// CONFIG_WALTER_MODEM_MAX_EVENT_DURATION_MS (500 ms) - so it parses two eight-character strings
// and returns.
volatile bool g_psm_granted = false;
volatile std::uint8_t g_psm_tau_octet = 0;
volatile std::uint8_t g_psm_active_octet = 0;

void networkEvent(WMNetworkEventType event, const WMNetworkEventData* data, void* args) {
  (void)args;
  if (event != WALTER_MODEM_NETWORK_EVENT_REG_STATE_CHANGE || data == nullptr) return;
  if (!data->cereg.hasPsmInfo) return;
  std::uint8_t tau = 0;
  std::uint8_t active = 0;
  // +CEREG's `<Periodic-TAU>` and `<Active-Time>` are the 3GPP octets as bit strings
  // (WalterModem.cpp:1825; 3GPP TS 27.007 §7.2). An unparseable one is dropped rather than
  // guessed at.
  if (!core::uplink::parseTimerBits(data->cereg.periodicTau, &tau)) return;
  if (!core::uplink::parseTimerBits(data->cereg.activeTime, &active)) return;
  g_psm_tau_octet = tau;
  g_psm_active_octet = active;
  g_psm_granted = true;
}

interfaces::SimState fromSimState(WalterModemSIMState state) {
  switch (state) {
    case WALTER_MODEM_SIM_STATE_READY:
      return interfaces::SimState::kReady;
    // Every "a PIN would open it" of the modem's list, personalisation locks included: all of
    // them are opened by sending a code, and this firmware only ever has the SIM's own.
    case WALTER_MODEM_SIM_STATE_PIN_REQUIRED:
    case WALTER_MODEM_SIM_STATE_PHONE_TO_SIM_PIN_REQUIRED:
    case WALTER_MODEM_SIM_STATE_PHONE_TO_FIRST_SIM_PIN_REQUIRED:
    case WALTER_MODEM_SIM_STATE_PIN2_REQUIRED:
    case WALTER_MODEM_SIM_STATE_NETWORK_PIN_REQUIRED:
    case WALTER_MODEM_SIM_STATE_NETWORK_SUBSET_PIN_REQUIRED:
    case WALTER_MODEM_SIM_STATE_SERVICE_PROVIDER_PIN_REQUIRED:
    case WALTER_MODEM_SIM_STATE_CORPORATE_SIM_REQUIRED:
      return interfaces::SimState::kPinRequired;
    // And every "a PUK would": the operator's letter and a person, never this firmware.
    case WALTER_MODEM_SIM_STATE_PUK_REQUIRED:
    case WALTER_MODEM_SIM_STATE_PHONE_TO_FIRST_SIM_PUK_REQUIRED:
    case WALTER_MODEM_SIM_STATE_PUK2_REQUIRED:
    case WALTER_MODEM_SIM_STATE_NETWORK_PUK_REQUIRED:
    case WALTER_MODEM_SIM_STATE_NETWORK_SUBSET_PUK_REQUIRED:
    case WALTER_MODEM_SIM_STATE_SERVICE_PROVIDER_PUK_REQUIRED:
    case WALTER_MODEM_SIM_STATE_CORPORATE_PUK_REQUIRED:
      return interfaces::SimState::kBlocked;
  }
  return interfaces::SimState::kUnknown;
}

// A failed `AT+CPIN?` often carries a CME error that says more than "it did not work"
// (WalterModem.cpp:2087 parses `+CME ERROR: n` into rsp.data.cmeError; the codes are 3GPP TS
// 27.007 §9.2.1's).
interfaces::SimState fromCmeError(const WalterModemRsp& rsp) {
  if (rsp.type != WALTER_MODEM_RSP_DATA_TYPE_CME_ERROR) return interfaces::SimState::kUnknown;
  switch (rsp.data.cmeError) {
    case WALTER_MODEM_CME_SIM_NOT_INSERTED:
    case WALTER_MODEM_CME_SIM_FAILURE:
    case WALTER_MODEM_CME_SIM_WRONG:
      return interfaces::SimState::kAbsent;
    case WALTER_MODEM_CME_PH_SIM_PIN_REQUIRED:
    case WALTER_MODEM_CME_PH_FSIM_PIN_REQUIRED:
    case WALTER_MODEM_CME_SIM_PIN_REQUIRED:
    case WALTER_MODEM_CME_SIM_PIN2_REQUIRED:
    case WALTER_MODEM_CME_NETWORK_PERSONALISATION_PIN_REQUIRED:
    case WALTER_MODEM_CME_NETWORK_SUBSET_PERSONALISATION_PIN_REQUIRED:
    case WALTER_MODEM_CME_SERVICE_PROVIDER_PERSONALISATION_PIN_REQUIRED:
    case WALTER_MODEM_CME_CORPORATE_PERSONALISATION_PIN_REQUIRED:
      return interfaces::SimState::kPinRequired;
    case WALTER_MODEM_CME_PH_FSIM_PUK_REQUIRED:
    case WALTER_MODEM_CME_SIM_PUK_REQUIRED:
    case WALTER_MODEM_CME_SIM_PUK2_REQUIRED:
    case WALTER_MODEM_CME_NETWORK_PERSONALISATION_PUK_REQUIRED:
    case WALTER_MODEM_CME_NETWORK_SUBSET_PERSONALISATION_PUK_REQUIRED:
    case WALTER_MODEM_CME_SERVICE_PROVIDER_PERSONALISATION_PUK_REQUIRED:
    case WALTER_MODEM_CME_CORPORATE_PERSONALISATION_PUK_REQUIRED:
      return interfaces::SimState::kBlocked;
    default:
      return interfaces::SimState::kUnknown;
  }
}

interfaces::Registration fromRegState(WalterModemNetworkRegState state) {
  switch (state) {
    case WALTER_MODEM_NETWORK_REG_NOT_SEARCHING:
      return interfaces::Registration::kNotSearching;
    case WALTER_MODEM_NETWORK_REG_SEARCHING:
    case WALTER_MODEM_NETWORK_REG_REGISTERED_TEMP_CONN_LOSS:
      return interfaces::Registration::kSearching;
    case WALTER_MODEM_NETWORK_REG_DENIED:
      return interfaces::Registration::kDenied;
    // Home and roaming are both "we are on a network". The SMS-only and emergency-only states
    // are not: they carry no packet data, which is the only thing this unit wants.
    case WALTER_MODEM_NETWORK_REG_REGISTERED_HOME:
    case WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING:
      return interfaces::Registration::kRegistered;
    case WALTER_MODEM_NETWORK_REG_REGISTERED_SMS_ONLY_HOME:
    case WALTER_MODEM_NETWORK_REG_REGISTERED_SMS_ONLY_ROAMING:
    case WALTER_MODEM_NETWORK_REG_ATTACHED_EMERGENCY_ONLY:
    case WALTER_MODEM_NETWORK_REG_REGISTERED_CSFB_NOT_PREFERRED_HOME:
    case WALTER_MODEM_NETWORK_REG_REGISTERED_CSFB_NOT_PREFERRED_ROAMING:
    case WALTER_MODEM_NETWORK_REG_UNKNOWN:
      return interfaces::Registration::kUnknown;
  }
  return interfaces::Registration::kUnknown;
}

interfaces::Rat fromRat(WalterModemRAT rat) {
  switch (rat) {
    case WALTER_MODEM_RAT_LTEM: return interfaces::Rat::kLteM;
    case WALTER_MODEM_RAT_NBIOT: return interfaces::Rat::kNbIot;
    case WALTER_MODEM_RAT_AUTO: return interfaces::Rat::kAuto;
    case WALTER_MODEM_RAT_UNKNOWN: return interfaces::Rat::kUnknown;
  }
  return interfaces::Rat::kUnknown;
}

bool toRat(interfaces::Rat rat, WalterModemRAT* out) {
  switch (rat) {
    case interfaces::Rat::kLteM: *out = WALTER_MODEM_RAT_LTEM; return true;
    case interfaces::Rat::kNbIot: *out = WALTER_MODEM_RAT_NBIOT; return true;
    case interfaces::Rat::kAuto: *out = WALTER_MODEM_RAT_AUTO; return true;
    case interfaces::Rat::kUnknown: return false;
  }
  return false;
}

void copyInto(char* out, std::size_t size, const char* text) {
  std::size_t i = 0;
  if (text == nullptr) {
    out[0] = 0;
    return;
  }
  for (; text[i] != 0 && i + 1 < size; ++i) out[i] = text[i];
  out[i] = 0;
}

}  // namespace

interfaces::UplinkState WalterUplink::state() const {
  if (!started_ || op_ != interfaces::OpState::kFull) return interfaces::UplinkState::kDown;
  // getNetworkRegState() is the library's buffered +CEREG state: no command, no air time.
  if (fromRegState(WalterModem::getNetworkRegState()) != interfaces::Registration::kRegistered) {
    return interfaces::UplinkState::kSearching;
  }
  return data_ ? interfaces::UplinkState::kUp : interfaces::UplinkState::kRegistered;
}

bool WalterUplink::present(std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return false;
  if (!started_) {
    // 0: the library must not take over this firmware's task watchdog (walter_uplink.h).
    if (!WalterModem::begin(static_cast<uart_port_t>(kModemUartPort), 0)) return false;
    started_ = true;
    op_ = interfaces::OpState::kOff;
    WalterModem::setNetworkEventHandler(networkEvent, nullptr);
  }
  if (!before(deadline_ms)) return false;
  return WalterModem::checkComm();
}

bool WalterUplink::resetModem(std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return false;
  if (!started_) return present(deadline_ms);
  // The physical reset, not softReset: a modem that is not answering will not answer `AT^RESET`
  // either (WalterModem.cpp, reset() pulses the module's reset pin and waits for +SYSSTART).
  const bool ok = WalterModem::reset();
  // Whatever came of it, nothing the modem knew survives it.
  op_ = interfaces::OpState::kOff;
  data_ = false;
  g_psm_granted = false;
  return ok;
}

interfaces::SimState WalterUplink::sim(std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return interfaces::SimState::kUnknown;
  WalterModemRsp rsp = {};
  if (!WalterModem::getSIMState(&rsp)) return fromCmeError(rsp);
  if (rsp.type != WALTER_MODEM_RSP_DATA_TYPE_SIM_STATE) return interfaces::SimState::kUnknown;
  return fromSimState(rsp.data.simState);
}

interfaces::SimUnlock WalterUplink::unlockSim(const char* pin, std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return interfaces::SimUnlock::kFailed;
  if (pin == nullptr || pin[0] == 0) return interfaces::SimUnlock::kFailed;
  WalterModemRsp rsp = {};
  if (WalterModem::unlockSIM(&rsp, nullptr, nullptr, pin)) return interfaces::SimUnlock::kAccepted;
  // The card said no - that is one of its three tries gone - as against the command not getting
  // through at all, which costs nothing and may be retried. The CME code is how they are told
  // apart; anything else is read as "the command failed", the safe side (core/uplink/policy.cpp
  // retries a kFailed at the next wake and never retries a kRefused).
  if (rsp.type == WALTER_MODEM_RSP_DATA_TYPE_CME_ERROR) {
    switch (rsp.data.cmeError) {
      case WALTER_MODEM_CME_INCORRECT_PASSWORD:
      case WALTER_MODEM_CME_SIM_PIN_REQUIRED:
      case WALTER_MODEM_CME_SIM_PUK_REQUIRED:
      case WALTER_MODEM_CME_SIM_WRONG:
        return interfaces::SimUnlock::kRefused;
      default:
        break;
    }
  }
  return interfaces::SimUnlock::kFailed;
}

bool WalterUplink::rat(std::uint64_t deadline_ms, interfaces::Rat* out) {
  if (!before(deadline_ms) || out == nullptr) return false;
  WalterModemRsp rsp = {};
  if (!WalterModem::getRAT(&rsp)) return false;
  if (rsp.type != WALTER_MODEM_RSP_DATA_TYPE_RAT) return false;
  *out = fromRat(rsp.data.rat);
  return true;
}

bool WalterUplink::setRat(interfaces::Rat rat, std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return false;
  WalterModemRAT wanted = WALTER_MODEM_RAT_LTEM;
  if (!toRat(rat, &wanted)) return false;
  return WalterModem::setRAT(wanted);
}

bool WalterUplink::opState(interfaces::OpState state, std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return false;
  WalterModemOpState wanted = WALTER_MODEM_OPSTATE_MINIMUM;
  switch (state) {
    case interfaces::OpState::kOff: wanted = WALTER_MODEM_OPSTATE_MINIMUM; break;
    case interfaces::OpState::kNoRf: wanted = WALTER_MODEM_OPSTATE_NO_RF; break;
    case interfaces::OpState::kFull: wanted = WALTER_MODEM_OPSTATE_FULL; break;
  }
  if (!WalterModem::setOpState(wanted)) return false;
  op_ = state;
  if (state != interfaces::OpState::kFull) data_ = false;
  if (state == interfaces::OpState::kFull) {
    // Automatic operator selection: the examples' `lteConnect` does this right after the radio
    // goes on, and without it a modem whose NVM holds a manual selection from a previous life
    // searches for a network that is not there.
    WalterModem::setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC);
  }
  return true;
}

bool WalterUplink::definePdp(const char* apn, std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return false;
  // Context 1, the primary one, as the library's own default and every example uses. An empty
  // APN is passed as an empty string, which is `AT+CGDCONT=1,"IP",""` - "let the network
  // choose".
  return WalterModem::definePDPContext(1, apn != nullptr ? apn : "");
}

bool WalterUplink::attach(std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return false;
  return WalterModem::setNetworkAttachmentState(true);
}

interfaces::Registration WalterUplink::registration() {
  if (!started_) return interfaces::Registration::kNotSearching;
  return fromRegState(WalterModem::getNetworkRegState());
}

bool WalterUplink::dataReady(std::uint64_t deadline_ms) {
  if (!before(deadline_ms)) return false;
  WalterModemRsp rsp = {};
  data_ = false;
  if (!WalterModem::getPDPAddress(&rsp)) return false;
  if (rsp.type != WALTER_MODEM_RSP_DATA_TYPE_PDP_ADDR) return false;
  // An address is the proof: registered with no address is a network that let us in and gave us
  // nothing, which is the `no-data` of the taxonomy.
  const char* address = rsp.data.pdpAddressList.pdpAddress;
  data_ = address != nullptr && address[0] != 0;
  return data_;
}

bool WalterUplink::networkTime(std::uint64_t deadline_ms, interfaces::NetworkTime* out) {
  if (!before(deadline_ms) || out == nullptr) return false;
  WalterModemRsp rsp = {};
  if (!WalterModem::getClock(&rsp)) return false;
  if (rsp.type != WALTER_MODEM_RSP_DATA_TYPE_CLOCK) return false;
  // epochTime is an int64 and is -1 when the library's own floor (2023-01-01) was not reached.
  // The window that decides whether this is the network's time is core/uplink/clock_window.h -
  // the same one host_test/sim/fake_modem.cpp uses, so the judgement is the same on both sides.
  const std::int64_t epoch = rsp.data.clock.epochTime;
  const bool plausible = epoch > 0 && epoch <= 0xFFFFFFFFll;
  out->epoch = plausible ? static_cast<std::uint32_t>(epoch) : 0;
  out->origin = core::uplink::judgeTime(true, out->epoch);
  if (out->origin != interfaces::TimeOrigin::kNetwork) out->epoch = 0;
  return true;
}

bool WalterUplink::signal(std::uint64_t deadline_ms, interfaces::Signal* out) {
  if (out == nullptr) return false;
  *out = interfaces::Signal{};
  bool any = false;

  // RSRP and RSRQ: `AT+CESQ`, which the modem answers whether or not it has registered.
  if (before(deadline_ms)) {
    WalterModemRsp rsp = {};
    if (WalterModem::getSignalQuality(&rsp) &&
        rsp.type == WALTER_MODEM_RSP_DATA_TYPE_SIGNAL_QUALITY) {
      out->rsrp_known = true;
      out->rsrp_dbm = static_cast<std::int16_t>(rsp.data.signalQuality.rsrp);
      out->rsrq_known = true;
      // "The RSRQ in 10ths of dB" (WalterModem.h, WalterModemSignalQuality).
      out->rsrq_tenth_db = static_cast<std::int16_t>(rsp.data.signalQuality.rsrq);
      any = true;
    }
  }
  // RSSI: `AT+CSQ`, in dBm, "in the range [-113dBm, -51dBm]".
  if (before(deadline_ms)) {
    WalterModemRsp rsp = {};
    if (WalterModem::getRSSI(&rsp) && rsp.type == WALTER_MODEM_RSP_DATA_TYPE_RSSI) {
      out->rssi_known = true;
      out->rssi_dbm = static_cast<std::int16_t>(rsp.data.rssi);
      any = true;
    }
  }
  // The operator and the band: `AT+SQNMONI` for the serving cell. Only a registered modem has
  // one, so a failure here is not a failure of the reading.
  if (before(deadline_ms)) {
    WalterModemRsp rsp = {};
    if (WalterModem::getCellInformation(WALTER_MODEM_SQNMONI_REPORTS_SERVING_CELL, &rsp) &&
        rsp.type == WALTER_MODEM_RSP_DATA_TYPE_CELL_INFO) {
      copyInto(out->operator_name, sizeof out->operator_name, rsp.data.cellInformation.netName);
      out->band = rsp.data.cellInformation.band;
      if (!out->rsrp_known) {
        out->rsrp_known = true;
        out->rsrp_dbm = static_cast<std::int16_t>(rsp.data.cellInformation.rsrp);
      }
      any = true;
    }
  }
  if (before(deadline_ms)) {
    interfaces::Rat which = interfaces::Rat::kUnknown;
    if (rat(deadline_ms, &which)) out->rat = which;
  }
  return any;
}

bool WalterUplink::identity(std::uint64_t deadline_ms, interfaces::Identity* out) {
  if (out == nullptr) return false;
  *out = interfaces::Identity{};
  bool any = false;
  if (before(deadline_ms)) {
    WalterModemRsp rsp = {};
    if (WalterModem::getIdentity(&rsp) && rsp.type == WALTER_MODEM_RSP_DATA_TYPE_IDENTITY) {
      copyInto(out->imei, sizeof out->imei, rsp.data.identity.imei);
      any = true;
    }
  }
  // The ICCID and the IMSI are read from the card, which needs the modem in FULL or NO_RF
  // (WalterModem.h, getSIMCardID and getSIMCardIMSI say so).
  if (before(deadline_ms)) {
    WalterModemRsp rsp = {};
    if (WalterModem::getSIMCardID(&rsp) && rsp.type == WALTER_MODEM_RSP_DATA_TYPE_SIM_CARD_ID) {
      copyInto(out->iccid, sizeof out->iccid, rsp.data.simCardID.iccid);
      any = true;
    }
  }
  if (before(deadline_ms)) {
    WalterModemRsp rsp = {};
    if (WalterModem::getSIMCardIMSI(&rsp) &&
        rsp.type == WALTER_MODEM_RSP_DATA_TYPE_SIM_CARD_IMSI) {
      copyInto(out->imsi, sizeof out->imsi, rsp.data.imsi);
      any = true;
    }
  }
  return any;
}

bool WalterUplink::powerSaving(const interfaces::PowerSavingRequest& request,
                               std::uint64_t deadline_ms, interfaces::PowerSavingGrant* out) {
  if (out == nullptr) return false;
  *out = interfaces::PowerSavingGrant{};
  out->requested = request.mode;
  if (!before(deadline_ms)) return false;
  bool asked = false;
  if (request.mode == interfaces::PowerSavingMode::kPsm) {
    char tau[9] = {};
    char active[9] = {};
    core::uplink::timerBits(request.tau_octet, tau);
    core::uplink::timerBits(request.active_octet, active);
    // `AT+CPSMS=1,,,"<tau>","<active>"`: the two 3GPP octets as bit strings (WalterModem.cpp,
    // configPSM). The library's own durationToTAU() would have made them and is wrong at 1.5.0,
    // so core/uplink/psm.h does it instead - with the reason written down there.
    asked = WalterModem::configPSM(WALTER_MODEM_PSM_ENABLE, tau, active);
  } else {
    asked = WalterModem::configPSM(WALTER_MODEM_PSM_DISABLE);
    if (asked) g_psm_granted = false;
  }
  out->asked = asked;
  if (!asked) return false;
  // What the network granted arrives in the unsolicited +CEREG report, which may not have come
  // yet - or at all. `granted` false is "it has not said", not "it refused".
  if (g_psm_granted) {
    out->granted = true;
    out->tau_octet = g_psm_tau_octet;
    out->active_octet = g_psm_active_octet;
  }
  return true;
}

void WalterUplink::takeDown() {
  if (!started_) return;
  // The radio off. **Not WalterModem::sleep()**: that one calls esp_deep_sleep() and takes the
  // whole board down with it (WalterModem.cpp:4452), which is the duty cycle's decision to make
  // and not this one's (task p3-04; tasks/phase-3/reports/01.md).
  WalterModem::setOpState(WALTER_MODEM_OPSTATE_MINIMUM);
  op_ = interfaces::OpState::kOff;
  data_ = false;
}

}  // namespace modem
