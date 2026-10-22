// The Walter's **own** attention (task p4-04): the two things the agent can learn that a person
// has to know about and that no radio can tell us.
//
//   the token was refused   a 401 from meshmaster.dk (components/agent/modem_uplink.h). The
//                           library halts the loop on it and nothing more is sent until a person
//                           types `set server.token <token>`; without this the only trace would
//                           be one line in a log ring that the next wake's deep sleep forgets.
//   a reboot was wanted     the agent library asked the platform to restart the board for its
//                           health (`mm::IPlatform::rebootForHealth`). This firmware does not:
//                           the supervisor and the wake cycle own restarts
//                           (components/agent/walter_platform.h). That the loop asked is worth
//                           recording precisely because nothing happened.
//
// §8's EV_ATTENTION is the *radio* asking for the Walter's attention and is the supervisor's
// (core/cycle/attention_source.h). This is the other direction and has nothing to do with the
// link: **nothing here wakes the radio, sends a frame or climbs a ladder.** It is a byte in the
// duty cycle's retained record, a letter in the wake record's line, a line in `status`, and -
// for a reboot that was asked for and refused - one journal entry.
//
// Where the byte lives: the spare byte of `core::cycle::WakeReason`'s record
// (`interfaces::RetainedSlot::kDutyCycle`), because RTC memory has no slot left
// (`interfaces/retained.h`, kDutyCycle's comment) and because the duty cycle's record is the one
// that already crosses every sleep. Raising it survives the sleep; the wake record that carries
// it to the log and to the journal takes it (`taken()`), so a raise is reported once and a
// condition that is still there raises it again at the next wake.
//
// Logic: no ESP-IDF include, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/cycle/wake_reason.h"
#include "core/update/journal.h"

namespace core {
namespace cycle {

// -- the journal entry: kind::kAgentAttention, 48 bytes ----------------------------------------
//
//    0  what     u8    an AgentAttentionKind
//    1  spare    u8
//    2  count    u16   how many of this kind since the retained record began, saturating
//    4  why      44 B  the library's own sentence, **truncated to what a journal entry holds**
//                      and never a secret (the loop builds it from two counts: "N server calls
//                      unanswered in M min, wifi up", which is 45 characters at two digits each
//                      and so loses its last word here). `AgentAttention::lastWhy()` keeps the
//                      whole of it in RAM for `status`; a journal entry is 64 bytes and 48 of
//                      them are the payload (core/update/journal.h), so this is the field there
//                      is and not a field somebody chose.
constexpr std::size_t kAgentPayloadBytes = 48;
static_assert(kAgentPayloadBytes <= update::kPayloadMax,
              "the agent's payload has to fit a journal entry");
constexpr std::size_t kAgentWhyBytes = kAgentPayloadBytes - 4;

// The whole sentence, as the library hands it over: `kMaxRebootWhyLen` of its own `Health.h` is
// 63 and `AgentLoop.cpp` formats into a `char[64]`.
constexpr std::size_t kWhyMax = 64;

enum class AgentAttentionKind : std::uint8_t {
  kNone = 0,
  kTokenRefused = 1,  // a 401: the token is revoked or wrong
  kRebootWanted = 2,  // rebootForHealth() was called and this firmware did not restart
  kCount,
};

const char* describe(AgentAttentionKind kind);
// The letter the wake record's line carries for it: `t` and `h`. Documented in docs/cycle.md.
char letter(AgentAttentionKind kind);

struct AgentPayload {
  AgentAttentionKind what = AgentAttentionKind::kNone;
  std::uint16_t count = 0;
  char why[kAgentWhyBytes] = {};
};

void encode(const AgentPayload& payload, std::uint8_t out[kAgentPayloadBytes]);
bool decode(const update::Entry& entry, AgentPayload* out);  // false unless it is an agent entry

// -- the bits, as the retained byte and the record's line carry them ---------------------------

namespace agent_attention {
constexpr std::uint8_t kNone = 0;
constexpr std::uint8_t kTokenRefused = 0x01;
constexpr std::uint8_t kRebootWanted = 0x02;
constexpr std::uint8_t kAll = kTokenRefused | kRebootWanted;
}  // namespace agent_attention

constexpr std::uint8_t bitOf(AgentAttentionKind kind) {
  return kind == AgentAttentionKind::kTokenRefused
             ? agent_attention::kTokenRefused
             : (kind == AgentAttentionKind::kRebootWanted ? agent_attention::kRebootWanted
                                                          : agent_attention::kNone);
}

// The letters of the bits raised, in order, into `out` (three bytes: two letters and a NUL).
// Empty when nothing is raised.
void agentAttentionLetters(std::uint8_t bits, char out[3]);
// One phrase for `status` and the record's line, or "" when nothing is raised. Static text.
const char* describeAgentAttention(std::uint8_t bits);

// -- what the agent raises it through ----------------------------------------------------------
//
// A class and not two free functions, because the thing that raises it (components/agent/) is
// built on core/ and must not know where the byte lives or whether there is a journal. Both
// pointers may be null - a build with no duty cycle and no `journal` partition is what a factory
// image is - and then raising it is counted here and nothing else, which is honest: there is
// nowhere to put it.
class AgentAttention {
 public:
  AgentAttention(WakeReason* wake = nullptr, update::Journal* journal = nullptr)
      : wake_(wake), journal_(journal) {}

  // A 401. Nothing is journalled: the token being refused is not news about an update, it is a
  // state a person fixes, and the wake record's letter plus `status` are where it belongs.
  void raiseTokenRefused();

  // `rebootForHealth(why)`: the bit, the count, and one journal entry with the library's own
  // sentence. `why` may be null.
  void raiseRebootWanted(const char* why);

  // The bits raised and not yet carried into a wake record.
  std::uint8_t raised() const;
  // A wake record has carried them: forgotten, so that one raise is reported once.
  void taken();

  // How many of each have been raised since the retained record began (this object's own count,
  // for the console; the retained byte carries the bits and not the counts).
  std::uint32_t tokenRefusals() const { return token_refusals_; }
  std::uint32_t rebootsWanted() const { return reboots_wanted_; }
  // The last reason `rebootForHealth` was given, or "" - what `status` prints beside the letter.
  const char* lastWhy() const { return why_; }

 private:
  void raise(AgentAttentionKind kind, const char* why);

  WakeReason* wake_ = nullptr;
  update::Journal* journal_ = nullptr;
  std::uint32_t token_refusals_ = 0;
  std::uint32_t reboots_wanted_ = 0;
  // Raised in a build with no duty cycle: kept here so that `status` still says it happened.
  std::uint8_t bits_ = agent_attention::kNone;
  char why_[kWhyMax] = {};
};

}  // namespace cycle
}  // namespace core
