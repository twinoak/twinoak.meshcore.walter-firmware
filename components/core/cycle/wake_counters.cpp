#include "core/cycle/wake_counters.h"

#include <cstring>

namespace core {
namespace cycle {
namespace {

std::uint32_t saturate(std::uint32_t n) { return n == 0xFFFFFFFFu ? n : n + 1; }
std::uint16_t saturate16(std::uint16_t n) {
  return n == 0xFFFFu ? n : static_cast<std::uint16_t>(n + 1);
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
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

WakeCounters::WakeCounters(interfaces::KeyValue* store) : store_(store) {}

void WakeCounters::begin() {
  begun_ = true;
  totals_ = WakeTotals();
  survived_ = false;
  dirty_ = false;
  wake_counted_ = false;
  sent_counted_ = false;
  if (store_ == nullptr || !store_->ready()) return;
  // A local and not a member: 35 bytes, wanted once a boot, and every byte of this object is
  // .bss the main task's stack has to be found around afterwards (docs/ARCHITECTURE.md §14).
  char hex[kWakeCountersHexBytes] = {};
  if (!store_->read(kWakeCountersKey, hex, sizeof hex)) return;
  decode(hex);
}

void WakeCounters::decode(const char* hex) {
  if (std::strlen(hex) != kWakeCountersHexBytes - 1) {
    writes_.refused = saturate16(writes_.refused);
    return;
  }
  std::uint8_t bytes[kWakeCountersBytes] = {};
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    const int hi = hexValue(hex[2 * i]);
    const int lo = hexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      writes_.refused = saturate16(writes_.refused);
      return;
    }
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  if (bytes[0] != kWakeCountersVersion) {
    writes_.refused = saturate16(writes_.refused);
    return;
  }
  totals_.timer = get32(bytes + 1);
  totals_.radio = get32(bytes + 5);
  totals_.spurious = get32(bytes + 9);
  totals_.silent = get32(bytes + 13);
  survived_ = true;
}

void WakeCounters::countWake(Wake wake) {
  if (wake_counted_) return;
  wake_counted_ = true;
  switch (wake) {
    case Wake::kTimer: totals_.timer = saturate(totals_.timer); break;
    case Wake::kAttention: totals_.radio = saturate(totals_.radio); break;
    case Wake::kSpurious: totals_.spurious = saturate(totals_.spurious); break;
    default:
      // A person's cable, a power-on, a reset, a crash, a watchdog: not one of the three the
      // server cannot see, and nothing is counted. `wake_counted_` is still set, so a second
      // call does not count one of them by mistake.
      return;
  }
  dirty_ = true;
}

void WakeCounters::countSent(interfaces::Reached reached) {
  if (sent_counted_) return;
  sent_counted_ = true;
  // **Silent is *nothing was sent*, and not *nothing came back*.** The same line the console's
  // `nothing sent: ` clause is drawn at (core/cycle/wake_cycle.cpp's `recordLine`): a wake whose
  // heartbeat went out and did not come back is `kUnsaid` - the exchange failed - and it is not
  // silent, because the unit did try. One vocabulary, one line, two readers.
  if (reached == interfaces::Reached::kAnswered || reached == interfaces::Reached::kUnsaid) return;
  totals_.silent = saturate(totals_.silent);
  dirty_ = true;
}

bool WakeCounters::save() {
  // Nothing moved, nothing is written: a wake that is none of the four spends no flash here.
  if (!dirty_) return true;
  if (store_ == nullptr || !store_->ready()) return false;
  std::uint8_t bytes[kWakeCountersBytes] = {};
  bytes[0] = kWakeCountersVersion;
  put32(bytes + 1, totals_.timer);
  put32(bytes + 5, totals_.radio);
  put32(bytes + 9, totals_.spurious);
  put32(bytes + 13, totals_.silent);
  char hex[kWakeCountersHexBytes] = {};
  const char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    hex[2 * i] = digits[bytes[i] >> 4];
    hex[2 * i + 1] = digits[bytes[i] & 0x0Fu];
  }
  hex[2 * sizeof bytes] = 0;
  if (!store_->write(kWakeCountersKey, hex)) return false;
  writes_.writes = saturate16(writes_.writes);
  dirty_ = false;
  return true;
}

void WakeCounters::forget() {
  totals_ = WakeTotals();
  survived_ = false;
  dirty_ = false;
  wake_counted_ = false;
  sent_counted_ = false;
  if (store_ != nullptr && store_->ready()) (void)store_->erase(kWakeCountersKey);
}

}  // namespace cycle
}  // namespace core
