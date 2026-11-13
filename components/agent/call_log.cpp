#include "walter_agent/call_log.h"

#include <cstring>

namespace agent {
namespace {

std::uint16_t saturate(std::uint16_t n) { return n == 0xFFFFu ? n : static_cast<std::uint16_t>(n + 1); }

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void put16(std::uint8_t* at, std::uint16_t v) {
  at[0] = static_cast<std::uint8_t>(v & 0xFFu);
  at[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}
std::uint16_t get16(const std::uint8_t* at) {
  return static_cast<std::uint16_t>(at[0] | (static_cast<std::uint16_t>(at[1]) << 8));
}
void put32(std::uint8_t* at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) at[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
}
std::uint32_t get32(const std::uint8_t* at) {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | at[i];
  return v;
}

}  // namespace

const char* callKindName(CallKind what) {
  switch (what) {
    case CallKind::kUnknown: return "a call";
    case CallKind::kConfig: return "config";
    case CallKind::kHeartbeat: return "heartbeat";
    case CallKind::kObservations: return "observations";
    case CallKind::kClaim: return "claim";
    case CallKind::kResult: return "a result";
    case CallKind::kLease: return "a lease";
    case CallKind::kProgress: return "progress";
    case CallKind::kDownload: return "a download range";
    case CallKind::kCount: break;
  }
  return "a call";
}

CallLog::CallLog(interfaces::KeyValue* store) : store_(store) {}

void CallLog::begin() {
  begun_ = true;
  for (std::size_t i = 0; i < sizeof items_; ++i) items_[i] = 0;
  count_ = 0;
  survived_ = false;
  dirty_ = false;
  if (store_ == nullptr || !store_->ready()) return;
  // The hex is a local and not a member: it is 147 bytes, it is wanted twice a boot at most, and
  // this object is one of main/'s namespace-scope objects whose every byte is .bss that the main
  // task's stack has to be found around afterwards (docs/ARCHITECTURE.md §14, rail R22).
  char hex[kCallsHexBytes] = {};
  if (!store_->read(kCallsKey, hex, sizeof hex)) return;
  decode(hex);
}

void CallLog::pack(std::uint8_t* at, const FailedCall& call) {
  at[0] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(call.what) & 0x0Fu) |
                                    ((static_cast<std::uint8_t>(call.outcome) & 0x0Fu) << 4));
  put16(at + 1, call.status);
  put16(at + 3, call.wake);
  put32(at + 5, call.at_epoch);
}

FailedCall CallLog::unpack(const std::uint8_t* at) {
  FailedCall call;
  const std::uint8_t kind = static_cast<std::uint8_t>(at[0] & 0x0Fu);
  const std::uint8_t outcome = static_cast<std::uint8_t>((at[0] >> 4) & 0x0Fu);
  call.what = kind < static_cast<std::uint8_t>(CallKind::kCount) ? static_cast<CallKind>(kind)
                                                                 : CallKind::kUnknown;
  call.status = get16(at + 1);
  call.outcome = outcome <= static_cast<std::uint8_t>(interfaces::HttpOutcome::kSocketFailed)
                     ? static_cast<interfaces::HttpOutcome>(outcome)
                     : interfaces::HttpOutcome::kOk;
  call.wake = get16(at + 3);
  call.at_epoch = get32(at + 5);
  return call;
}

void CallLog::decode(const char* hex) {
  if (std::strlen(hex) != kCallsHexBytes - 1) {
    counters_.refused = saturate(counters_.refused);
    return;
  }
  std::uint8_t bytes[kCallsBytes] = {};
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    const int hi = hexValue(hex[2 * i]);
    const int lo = hexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      counters_.refused = saturate(counters_.refused);
      return;
    }
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  if (bytes[0] != kCallsVersion) {
    counters_.refused = saturate(counters_.refused);
    return;
  }
  // The entries are stored newest first and a kind of `kUnknown` ends the list: a value written
  // with fewer than eight is zero-padded behind them.
  std::memcpy(items_, bytes + 1, sizeof items_);
  count_ = 0;
  for (std::size_t i = 0; i < kCallsKept; ++i) {
    const std::uint8_t kind = static_cast<std::uint8_t>(items_[i * kCallEntryBytes] & 0x0Fu);
    if (kind == 0 || kind >= static_cast<std::uint8_t>(CallKind::kCount)) break;
    ++count_;
  }
  survived_ = true;
}

void CallLog::atWake(std::uint32_t wakes) {
  wake_ = wakes > kWakeAtMost ? static_cast<std::uint16_t>(kWakeAtMost)
                              : static_cast<std::uint16_t>(wakes);
}

void CallLog::record(CallKind what, std::uint16_t status, interfaces::HttpOutcome outcome,
                     std::uint32_t at_epoch) {
  if (what == CallKind::kUnknown || what >= CallKind::kCount) return;
  if (count_ == kCallsKept) {
    counters_.pushed_out = saturate(counters_.pushed_out);
  } else {
    ++count_;
  }
  // The oldest of the eight falls off the end of the move, which is what pushes it out.
  std::memmove(items_ + kCallEntryBytes, items_, sizeof items_ - kCallEntryBytes);
  FailedCall call;
  call.what = what;
  call.status = status;
  call.outcome = outcome;
  call.wake = wake_;
  call.at_epoch = at_epoch;
  pack(items_, call);
  // Whatever is past the list is zeroed: a stale entry the move pushed into an unused slot would
  // be read back from the store as a ninth call, because a kind of 0 is what ends the list.
  for (std::size_t i = count_ * kCallEntryBytes; i < sizeof items_; ++i) items_[i] = 0;
  counters_.recorded = saturate(counters_.recorded);
  dirty_ = true;
}

bool CallLog::save() {
  // **Nothing moved, nothing is written.** A wake in which no call failed spends no flash here,
  // which is the task's fourth deliverable and the only reason this is a flag and not a compare.
  if (!dirty_) return true;
  if (store_ == nullptr || !store_->ready()) return false;
  std::uint8_t bytes[kCallsBytes] = {};
  bytes[0] = kCallsVersion;
  std::memcpy(bytes + 1, items_, sizeof items_);
  // The hex is a local and not a member, for the reason `begin()` gives.
  char hex[kCallsHexBytes] = {};
  const char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    hex[2 * i] = digits[bytes[i] >> 4];
    hex[2 * i + 1] = digits[bytes[i] & 0x0Fu];
  }
  hex[2 * sizeof bytes] = 0;
  if (!store_->write(kCallsKey, hex)) return false;
  counters_.writes = saturate(counters_.writes);
  dirty_ = false;
  return true;
}

FailedCall CallLog::at(std::size_t index) const {
  return index < count_ ? unpack(items_ + index * kCallEntryBytes) : FailedCall();
}

void CallLog::forget() {
  for (std::size_t i = 0; i < sizeof items_; ++i) items_[i] = 0;
  count_ = 0;
  dirty_ = false;
  survived_ = false;
  if (store_ != nullptr && store_->ready()) (void)store_->erase(kCallsKey);
}

}  // namespace agent
