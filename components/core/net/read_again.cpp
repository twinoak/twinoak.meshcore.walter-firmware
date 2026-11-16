#include "core/net/read_again.h"

#include <cstdio>
#include <cstring>

namespace core {
namespace net {

namespace {

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

// A total that has said everything it has to say stays where it is rather than wrapping to a
// small number that reads as a unit this has hardly happened on.
std::uint32_t oneMore(std::uint32_t n) { return n == 0xFFFFFFFFu ? n : n + 1; }

std::uint16_t oneMore16(std::uint16_t n) {
  return n == 0xFFFFu ? n : static_cast<std::uint16_t>(n + 1);
}

}  // namespace

// ------------------------------------------------------------------------------- as a call runs

void ReadAgain::newCall() { spent_ = false; }

AfterFailure ReadAgain::onFailedReceive(bool closed_seen) {
  const AfterFailure next = after(closed_seen, spent_);
  if (next == AfterFailure::kAskAgain) {
    // Spent here, before the ask is even made: whatever comes of it, this call has had its one.
    spent_ = true;
    asks_ = oneMore(asks_);
    dirty_ = true;
  }
  return next;
}

void ReadAgain::found(ReadAgainFound what) {
  if (what != ReadAgainFound::kBytes) return;
  answers_ = oneMore(answers_);
  dirty_ = true;
}

// ------------------------------------------------------------------------------------ the totals

void ReadAgain::begin(interfaces::KeyValue* store) {
  asks_ = 0;
  answers_ = 0;
  survived_ = false;
  dirty_ = false;
  if (store == nullptr || !store->ready()) return;
  // The hex is a local and not a member: it is 19 bytes, it is wanted once a boot, and this
  // object sits inside the record (core/net/read_again.h, *why it lives inside the record*).
  char hex[kReadAgainHexBytes] = {};
  if (!store->read(kReadAgainKey, hex, sizeof hex)) return;
  if (!decode(hex)) {
    refused_ = oneMore16(refused_);
    return;
  }
  survived_ = true;
}

bool ReadAgain::decode(const char* hex) {
  if (std::strlen(hex) != kReadAgainHexBytes - 1) return false;
  std::uint8_t bytes[kReadAgainBytes] = {};
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    const int hi = hexValue(hex[2 * i]);
    const int lo = hexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  if (bytes[0] != kReadAgainVersion) return false;
  asks_ = get32(bytes + 1);
  answers_ = get32(bytes + 5);
  return true;
}

bool ReadAgain::save(interfaces::KeyValue* store) {
  // **Nothing moved, nothing is written.** A unit whose calls behave spends no flash here.
  if (!dirty_) return true;
  if (store == nullptr || !store->ready()) return false;
  std::uint8_t bytes[kReadAgainBytes] = {};
  bytes[0] = kReadAgainVersion;
  put32(bytes + 1, asks_);
  put32(bytes + 5, answers_);
  char hex[kReadAgainHexBytes] = {};
  const char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    hex[2 * i] = digits[bytes[i] >> 4];
    hex[2 * i + 1] = digits[bytes[i] & 0x0Fu];
  }
  hex[2 * sizeof bytes] = 0;
  if (!store->write(kReadAgainKey, hex)) return false;
  dirty_ = false;
  return true;
}

void ReadAgain::forget(interfaces::KeyValue* store) {
  asks_ = 0;
  answers_ = 0;
  survived_ = false;
  dirty_ = false;
  if (store != nullptr && store->ready()) (void)store->erase(kReadAgainKey);
}

// ---------------------------------------------------------------------------------------- in words

void ReadAgain::describe(interfaces::TextOut& out) const {
  char line[200];
  int n = 0;
  if (asks_ == 0) {
    n = std::snprintf(line, sizeof line,
                      "  asked again after a failed receive: never yet\n");
  } else {
    n = std::snprintf(line, sizeof line,
                      "  asked again after a failed receive: %lu since this unit was made, "
                      "%lu of them answered\n",
                      static_cast<unsigned long>(asks_), static_cast<unsigned long>(answers_));
  }
  if (n <= 0) return;
  const std::size_t length =
      static_cast<std::size_t>(n) < sizeof line ? static_cast<std::size_t>(n) : sizeof line - 1;
  out.write(line, length);
}

}  // namespace net
}  // namespace core
