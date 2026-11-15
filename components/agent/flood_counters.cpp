#include "walter_agent/flood_counters.h"

#include <cstring>

namespace agent {
namespace {

std::uint16_t saturate16(std::uint16_t n) {
  return n == 0xFFFFu ? n : static_cast<std::uint16_t>(n + 1);
}

// A total that has reached the top stays there: a counter that wrapped would read on the page as
// a unit whose unscoped try suddenly stopped earning its airtime.
std::uint32_t add(std::uint32_t total, std::uint32_t more) {
  if (more == 0) return total;
  const std::uint32_t room = 0xFFFFFFFFu - total;
  return more >= room ? 0xFFFFFFFFu : total + more;
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

FloodCounters::FloodCounters(interfaces::KeyValue* store) : store_(store) {}

void FloodCounters::begin() {
  begun_ = true;
  totals_ = FloodTotals();
  taken_ = mm::FloodCounts();
  survived_ = false;
  dirty_ = false;
  if (store_ == nullptr || !store_->ready()) return;
  // A local and not a member: 27 bytes, wanted once a boot, and every byte of this object is
  // .bss the main task's stack has to be found around afterwards (docs/ARCHITECTURE.md §14).
  char hex[kFloodTotalsHexBytes] = {};
  if (!store_->read(kFloodTotalsKey, hex, sizeof hex)) return;
  decode(hex);
}

void FloodCounters::decode(const char* hex) {
  if (std::strlen(hex) != kFloodTotalsHexBytes - 1) {
    writes_.refused = saturate16(writes_.refused);
    return;
  }
  std::uint8_t bytes[kFloodTotalsBytes] = {};
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    const int hi = hexValue(hex[2 * i]);
    const int lo = hexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      writes_.refused = saturate16(writes_.refused);
      return;
    }
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  if (bytes[0] != kFloodTotalsVersion) {
    writes_.refused = saturate16(writes_.refused);
    return;
  }
  totals_.answered_scoped = get32(bytes + 1);
  totals_.answered_unscoped = get32(bytes + 5);
  totals_.unanswered = get32(bytes + 9);
  survived_ = true;
}

void FloodCounters::take(const mm::FloodCounts& counts) {
  // The difference, and nothing else: the library's three only ever grow inside a boot, and what
  // this object has already added to the totals is what it remembers. So a wake adds its own
  // counts once, a second call in the same wake adds nothing, and a boot in which the library
  // counted nothing writes nothing.
  const std::uint32_t scoped = counts.answeredScoped > taken_.answeredScoped
                                   ? counts.answeredScoped - taken_.answeredScoped
                                   : 0;
  const std::uint32_t unscoped = counts.answeredUnscoped > taken_.answeredUnscoped
                                     ? counts.answeredUnscoped - taken_.answeredUnscoped
                                     : 0;
  const std::uint32_t none = counts.unanswered > taken_.unanswered
                                 ? counts.unanswered - taken_.unanswered
                                 : 0;
  if (scoped == 0 && unscoped == 0 && none == 0) return;
  totals_.answered_scoped = add(totals_.answered_scoped, scoped);
  totals_.answered_unscoped = add(totals_.answered_unscoped, unscoped);
  totals_.unanswered = add(totals_.unanswered, none);
  taken_ = counts;
  dirty_ = true;
}

bool FloodCounters::save() {
  // Nothing moved, nothing is written: an exit node whose repeaters all have paths never writes
  // this value at all.
  if (!dirty_) return true;
  if (store_ == nullptr || !store_->ready()) return false;
  std::uint8_t bytes[kFloodTotalsBytes] = {};
  bytes[0] = kFloodTotalsVersion;
  put32(bytes + 1, totals_.answered_scoped);
  put32(bytes + 5, totals_.answered_unscoped);
  put32(bytes + 9, totals_.unanswered);
  char hex[kFloodTotalsHexBytes] = {};
  const char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    hex[2 * i] = digits[bytes[i] >> 4];
    hex[2 * i + 1] = digits[bytes[i] & 0x0Fu];
  }
  hex[2 * sizeof bytes] = 0;
  if (!store_->write(kFloodTotalsKey, hex)) return false;
  writes_.writes = saturate16(writes_.writes);
  dirty_ = false;
  return true;
}

void FloodCounters::forget() {
  totals_ = FloodTotals();
  taken_ = mm::FloodCounts();
  survived_ = false;
  dirty_ = false;
  if (store_ != nullptr && store_->ready()) (void)store_->erase(kFloodTotalsKey);
}

}  // namespace agent
