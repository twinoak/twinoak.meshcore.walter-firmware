#include "walter_agent/call_log.h"

#include <cstdio>
#include <cstring>

namespace agent {
namespace {

std::uint32_t saturate(std::uint32_t n) { return n == 0xFFFFFFFFu ? n : n + 1; }

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

// What `at()` answers for an index this list does not hold.
const FailedCall kNoCall;

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
  for (std::size_t i = 0; i < kCallsKept; ++i) {
    const std::uint8_t* e = bytes + 1 + i * kCallEntryBytes;
    const std::uint8_t kind = static_cast<std::uint8_t>(e[0] & 0x0Fu);
    if (kind == 0 || kind >= static_cast<std::uint8_t>(CallKind::kCount)) break;
    const std::uint8_t outcome = static_cast<std::uint8_t>((e[0] >> 4) & 0x0Fu);
    FailedCall& call = items_[count_++];
    call.what = static_cast<CallKind>(kind);
    call.status = get16(e + 1);
    call.outcome = outcome <= static_cast<std::uint8_t>(interfaces::HttpOutcome::kSocketFailed)
                       ? static_cast<interfaces::HttpOutcome>(outcome)
                       : interfaces::HttpOutcome::kOk;
    call.wake = get16(e + 3);
    call.at_epoch = get32(e + 5);
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
  for (std::size_t i = count_ - 1; i > 0; --i) items_[i] = items_[i - 1];
  FailedCall& call = items_[0];
  call.what = what;
  call.status = status;
  call.outcome = outcome;
  call.wake = wake_;
  call.at_epoch = at_epoch;
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
  for (std::size_t i = 0; i < count_; ++i) {
    const FailedCall& call = items_[i];
    std::uint8_t* e = bytes + 1 + i * kCallEntryBytes;
    e[0] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(call.what) & 0x0Fu) |
                                     ((static_cast<std::uint8_t>(call.outcome) & 0x0Fu) << 4));
    put16(e + 1, call.status);
    put16(e + 3, call.wake);
    put32(e + 5, call.at_epoch);
  }
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

const FailedCall& CallLog::at(std::size_t index) const {
  return index < count_ ? items_[index] : kNoCall;
}

void CallLog::forget() {
  count_ = 0;
  dirty_ = false;
  survived_ = false;
  if (store_ != nullptr && store_->ready()) (void)store_->erase(kCallsKey);
}

}  // namespace agent
