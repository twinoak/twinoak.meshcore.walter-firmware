#include "core/cycle/agent_attention.h"

namespace core {
namespace cycle {

namespace {

void put16(std::uint8_t* at, std::uint16_t value) {
  at[0] = static_cast<std::uint8_t>(value & 0xFF);
  at[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

std::uint16_t get16(const std::uint8_t* at) {
  return static_cast<std::uint16_t>(at[0] | (static_cast<std::uint16_t>(at[1]) << 8));
}

// `why` into a fixed field, truncated at the field and always terminated. The library's own
// sentence is 41 characters and two counts (agent/AgentLoop.cpp), so 43 is enough for it and the
// truncation is for whatever a later release writes.
void copyWhy(char* out, std::size_t cap, const char* why) {
  if (cap == 0) return;
  out[0] = 0;
  if (why == nullptr) return;
  std::size_t i = 0;
  for (; i + 1 < cap && why[i] != 0; ++i) out[i] = why[i];
  out[i] = 0;
}

}  // namespace

const char* describe(AgentAttentionKind kind) {
  switch (kind) {
    case AgentAttentionKind::kNone: return "nothing";
    case AgentAttentionKind::kTokenRefused:
      return "the server refused this unit's token (http 401): set server.token";
    case AgentAttentionKind::kRebootWanted:
      return "the agent asked for a restart for its health and did not get one";
    case AgentAttentionKind::kCount: break;
  }
  return "?";
}

// Neither is a letter a step's outcome uses (docs/cycle.md's two tables): one line carries both
// fields, and a letter that meant two things in one line would be a letter nobody could read.
// `t` is taken - *no time* - so the token's is `u`, for unauthorised.
char letter(AgentAttentionKind kind) {
  switch (kind) {
    case AgentAttentionKind::kTokenRefused: return 'u';
    case AgentAttentionKind::kRebootWanted: return 'h';
    case AgentAttentionKind::kNone:
    case AgentAttentionKind::kCount: break;
  }
  return '?';
}

void agentAttentionLetters(std::uint8_t bits, char out[3]) {
  std::size_t n = 0;
  if ((bits & agent_attention::kTokenRefused) != 0) {
    out[n++] = letter(AgentAttentionKind::kTokenRefused);
  }
  if ((bits & agent_attention::kRebootWanted) != 0) {
    out[n++] = letter(AgentAttentionKind::kRebootWanted);
  }
  out[n] = 0;
}

const char* describeAgentAttention(std::uint8_t bits) {
  const bool token = (bits & agent_attention::kTokenRefused) != 0;
  const bool reboot = (bits & agent_attention::kRebootWanted) != 0;
  if (token && reboot) {
    return "the token was refused (http 401), and a restart was asked for and not taken";
  }
  if (token) return describe(AgentAttentionKind::kTokenRefused);
  if (reboot) return describe(AgentAttentionKind::kRebootWanted);
  return "";
}

void encode(const AgentPayload& payload, std::uint8_t out[kAgentPayloadBytes]) {
  for (std::size_t i = 0; i < kAgentPayloadBytes; ++i) out[i] = 0;
  out[0] = static_cast<std::uint8_t>(payload.what);
  put16(out + 2, payload.count);
  for (std::size_t i = 0; i + 1 < kAgentWhyBytes && payload.why[i] != 0; ++i) {
    out[4 + i] = static_cast<std::uint8_t>(payload.why[i]);
  }
}

bool decode(const update::Entry& entry, AgentPayload* out) {
  if (entry.kind != update::kind::kAgentAttention || entry.size < kAgentPayloadBytes ||
      out == nullptr) {
    return false;
  }
  if (entry.payload[0] >= static_cast<std::uint8_t>(AgentAttentionKind::kCount)) return false;
  *out = AgentPayload{};
  out->what = static_cast<AgentAttentionKind>(entry.payload[0]);
  out->count = get16(entry.payload + 2);
  for (std::size_t i = 0; i + 1 < kAgentWhyBytes; ++i) {
    const char c = static_cast<char>(entry.payload[4 + i]);
    // Printable ASCII only: the field is read back into a console line, and a byte a flash
    // wrote badly must not become a control character on somebody's terminal.
    out->why[i] = (c >= 0x20 && c < 0x7F) ? c : 0;
    if (out->why[i] == 0) break;
  }
  return true;
}

// -- the facade --------------------------------------------------------------------------------

void AgentAttention::raise(AgentAttentionKind kind, const char* why) {
  const std::uint8_t bit = bitOf(kind);
  bits_ = static_cast<std::uint8_t>(bits_ | bit);
  if (wake_ != nullptr) wake_->raiseAgentAttention(bit);
  if (kind == AgentAttentionKind::kTokenRefused && token_refusals_ < 0xFFFFFFFFu) {
    ++token_refusals_;
  }
  if (kind == AgentAttentionKind::kRebootWanted) {
    if (reboots_wanted_ < 0xFFFFFFFFu) ++reboots_wanted_;
    copyWhy(why_, sizeof why_, why);
  }
}

void AgentAttention::raiseTokenRefused() { raise(AgentAttentionKind::kTokenRefused, nullptr); }

void AgentAttention::raiseRebootWanted(const char* why) {
  raise(AgentAttentionKind::kRebootWanted, why);
  if (journal_ == nullptr) return;
  AgentPayload payload;
  payload.what = AgentAttentionKind::kRebootWanted;
  payload.count = reboots_wanted_ > 0xFFFFu ? 0xFFFFu : static_cast<std::uint16_t>(reboots_wanted_);
  copyWhy(payload.why, sizeof payload.why, why);
  std::uint8_t bytes[kAgentPayloadBytes];
  encode(payload, bytes);
  (void)journal_->append(update::kind::kAgentAttention, bytes, sizeof bytes);
}

std::uint8_t AgentAttention::raised() const {
  if (wake_ != nullptr) return wake_->agentAttention();
  return bits_;
}

void AgentAttention::taken() {
  bits_ = agent_attention::kNone;
  if (wake_ != nullptr) wake_->clearAgentAttention();
}

}  // namespace cycle
}  // namespace core
