#include "core/link/client/words.h"

namespace core {
namespace link {
namespace client {

namespace {

// One bit and the word for it, for the two bit fields below.
struct Bit {
  std::uint32_t mask;
  const char* word;
};

// §5.1's capability bits, in the order the specification lists them.
constexpr Bit kCapabilities[] = {
    {cap::kCli, "cli"},                   {cap::kPower, "power"},
    {cap::kEnv, "env"},                   {cap::kMeshClient, "mesh-client"},
    {cap::kObservations, "observations"}, {cap::kAttention, "attention"},
    {cap::kTimePersist, "time-persist"}, {cap::kLocalRequest, "local-request"},
};

// §5.3's STATUS flags.
constexpr Bit kStatusFlags[] = {
    {status_flag::kJobBegun, "a job is begun"},
    {status_flag::kReplyAwaited, "a reply is awaited"},
    {status_flag::kWalterAsleep, "the walter is believed asleep"},
    {status_flag::kAdvertsHeld, "adverts held until the clock is set"},
    {status_flag::kRadioUp, "the radio initialised"},
};

// Appends, bounded, and says where the end is now. No <cstdio> and no heap: these go into a
// caller's small buffer on the stack.
std::size_t append(char* out, std::size_t size, std::size_t at, const char* word) {
  for (std::size_t i = 0; word[i] != 0 && at + 1 < size; ++i) {
    out[at++] = word[i];
  }
  if (at < size) {
    out[at] = 0;
  }
  return at;
}

std::size_t appendHex(char* out, std::size_t size, std::size_t at, std::uint32_t value) {
  at = append(out, size, at, "0x");
  bool leading = true;
  for (int shift = 28; shift >= 0; shift -= 4) {
    const std::uint32_t nibble = (value >> shift) & 0xF;
    if (leading && nibble == 0 && shift != 0) {
      continue;
    }
    leading = false;
    const char digit = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    if (at + 1 < size) {
      out[at++] = digit;
    }
  }
  if (at < size) {
    out[at] = 0;
  }
  return at;
}

std::size_t bitWords(const Bit* bits, std::size_t count, std::uint32_t value, char* out,
                     std::size_t size) {
  if (out == nullptr || size == 0) {
    return 0;
  }
  out[0] = 0;
  std::size_t at = 0;
  std::uint32_t known = 0;
  for (std::size_t i = 0; i < count; ++i) {
    known |= bits[i].mask;
    if ((value & bits[i].mask) == 0) {
      continue;
    }
    if (at > 0) {
      at = append(out, size, at, ", ");
    }
    at = append(out, size, at, bits[i].word);
  }
  const std::uint32_t rest = value & ~known;
  if (rest != 0) {
    // §5.1: unknown bits are ignored - but not hidden, or a radio one minor ahead looks like this
    // one to whoever reads the console.
    if (at > 0) {
      at = append(out, size, at, ", ");
    }
    at = append(out, size, at, "unknown ");
    at = appendHex(out, size, at, rest);
  }
  if (at == 0) {
    at = append(out, size, at, "none");
  }
  return at;
}

}  // namespace

const char* statusName(std::uint8_t value) {
  switch (value) {
    case status::kOk: return "ok";
    case status::kUnsupported: return "unsupported: the radio does not know the type, or lacks the capability";
    case status::kMalformed: return "malformed: the payload is too short or a field is not legal";
    case status::kBusy: return "busy: a mesh exchange is still waiting for its reply";
    case status::kState: return "state: not possible now";
    case status::kRange: return "range: a value is outside what is accepted";
    case status::kFailed: return "failed: tried and failed";
    case status::kDenied: return "denied: this role refuses it";
    default: return "unknown";
  }
}

const char* roleName(std::uint8_t value) {
  switch (value) {
    case role::kUnknown: return "unknown";
    case role::kRepeater: return "repeater";
    case role::kBridge: return "bridge";
    case role::kScanner: return "scanner";
    default: return "unknown";
  }
}

const char* resetReasonName(std::uint8_t value) {
  switch (value) {
    case reset_reason::kUnknown: return "unknown";
    case reset_reason::kPowerOn: return "power-on";
    case reset_reason::kResetPin: return "reset pin";
    case reset_reason::kWatchdog: return "watchdog";
    case reset_reason::kSoftware: return "software";
    case reset_reason::kLockup: return "lockup";
    case reset_reason::kWakeFromSystemOff: return "wake from system-off";
    case reset_reason::kLeftBootloader: return "left the bootloader";
    default: return "unknown";
  }
}

const char* timeStateName(std::uint8_t value) {
  switch (value) {
    case time_state::kUnset: return "unset (the built-in default; never right)";
    case time_state::kRestored: return "restored (carried over a reset; not the real time)";
    case time_state::kSet: return "set over this link since boot";
    case time_state::kRtc: return "read from a hardware rtc";
    default: return "unknown";
  }
}

const char* timeSourceName(std::uint8_t value) {
  switch (value) {
    case time_source::kCellular: return "the cellular network";
    case time_source::kServer: return "the server";
    case time_source::kConsole: return "a person at a console";
    default: return "unknown";
  }
}

const char* rebootModeName(std::uint8_t value) {
  switch (value) {
    case reboot_mode::kApplication: return "restart the application";
    case reboot_mode::kUf2Bootloader: return "the uf2 bootloader";
    case reboot_mode::kSerialDfu: return "serial dfu";
    case reboot_mode::kOtaDfu: return "over-the-air dfu";
    default: return "unknown";
  }
}

const char* sendResultName(std::uint8_t value) {
  switch (value) {
    case send_result::kNothingSent: return "nothing was sent";
    case send_result::kFlood: return "sent as a flood";
    case send_result::kDirect: return "sent direct";
    default: return "unknown";
  }
}

const char* replyStateName(std::uint8_t value) {
  switch (value) {
    case reply_state::kNone: return "nothing held";
    case reply_state::kPending: return "pending";
    case reply_state::kTimeout: return "timed out";
    case reply_state::kResponse: return "a response";
    case reply_state::kText: return "a text reply";
    case reply_state::kTrace: return "a trace";
    default: return "unknown";
  }
}

const char* pathModeName(std::uint8_t value) {
  switch (value) {
    case path_mode::kNone: return "none: the next send floods";
    case path_mode::kDirect: return "direct: zero hops";
    case path_mode::kHops: return "hops";
    default: return "unknown";
  }
}

const char* attentionName(std::uint8_t value) {
  switch (value) {
    case attention::kNone: return "none";
    case attention::kRestarted: return "the node restarted";
    case attention::kBatteryLow: return "battery low";
    case attention::kBatteryCritical: return "battery critical";
    case attention::kPersonAsked: return "a person asked for the walter";
    case attention::kObsRingNearlyFull: return "the observation ring is nearly full";
    case attention::kFault: return "a fault the node wants reported";
    default: return "unknown";
  }
}

const char* eventName(std::uint8_t value) {
  switch (value) {
    case type::kEvBoot: return "boot";
    case type::kEvMeshReply: return "mesh reply";
    case type::kEvAttention: return "attention";
    default: return "unknown";
  }
}

// §5.9's table, in MeshCore's own words for the four request types the node answers over the
// link. Anything else is the ERR_RANGE the radio gives it - including 0x03 telemetry, whose local
// form is LPP_GET (§5.8), and 0x02 keep-alive, which belongs to a login session this link has not.
const char* localRequestName(std::uint8_t req_type) {
  switch (req_type) {
    case local_req::kStatus: return "status";
    case local_req::kAccessList: return "access list";
    case local_req::kNeighbours: return "neighbours";
    case local_req::kOwnerInfo: return "owner info";
    default: return "unknown";
  }
}

std::size_t capabilityWords(std::uint32_t caps, char* out, std::size_t size) {
  return bitWords(kCapabilities, sizeof kCapabilities / sizeof kCapabilities[0], caps, out, size);
}

std::size_t statusFlagWords(std::uint16_t flags, char* out, std::size_t size) {
  return bitWords(kStatusFlags, sizeof kStatusFlags / sizeof kStatusFlags[0], flags, out, size);
}

}  // namespace client
}  // namespace link
}  // namespace core
