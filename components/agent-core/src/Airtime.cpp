#include <agent/Airtime.h>

#include <cstring>

namespace mm {

namespace {

constexpr uint32_t kMinuteMs = 60000;

// millis() wraps every 49 days; every comparison here is a signed difference, which is right
// across the wrap.
bool atOrAfter(uint32_t now, uint32_t then) { return static_cast<int32_t>(now - then) >= 0; }

}  // namespace

int neighbourPages(int neighbours) {
  if (neighbours <= 0) return 1;
  return (neighbours + kNeighboursPerPage - 1) / kNeighboursPerPage;
}

int transactionsFor(const char* type, CredentialClass credentialClass, int items) {
  if (type == nullptr) return 1;
  if (std::strcmp(type, "status") == 0) {
    // The firmware version comes from a second request, and only this class needs it.
    return credentialClass == CredentialClass::Acl ? 2 : 1;
  }
  if (std::strcmp(type, "neighbours") == 0) return neighbourPages(items);
  if (std::strcmp(type, "config.read") == 0) {
    // n + 2 + r for a list of n keys (ConfigReadPayload.Transactions): `items` is the list's
    // length when the claim named one, and the default list of sixteen - `regions` on it - when
    // it did not. A named list is priced as if it held `regions`, which is the ceiling.
    return items > 0 ? items + kConfigReadExtraCount + 1 : configReadTransactions(nullptr, 0);
  }
  if (std::strcmp(type, "config.apply") == 0) {
    // n + m + 1 (TransactionCosts.For, task p4-53): the plan's n commands, the re-read of the m
    // keys that were sent - n when every command went out - and `ver`. `items` is the plan's
    // command count; unknown is priced as one command.
    const int n = items > 0 ? items : 1;
    return 2 * n + 1;
  }
  // discover is one at best and three at worst; acl.set, telemetry and a trace are one - and so
  // are the writes (tasks p4-54, p4-55): one command, one reply, sent once.
  return 1;
}

size_t AirtimeGate::inWindow(uint32_t nowMs) const {
  size_t n = 0;
  for (size_t i = 0; i < count_; i++) {
    const uint32_t start = starts_[(first_ + i) % kMaxTracked];
    if (nowMs - start < kMinuteMs) n++;
  }
  return n;
}

uint32_t AirtimeGate::waitMs(uint32_t nowMs) const {
  uint32_t wait = 0;
  if (hasLast_) {
    const uint32_t gapMs = static_cast<uint32_t>(budget_.minGapSeconds > 0 ? budget_.minGapSeconds : 0) * 1000u;
    const uint32_t due = last_ + gapMs;
    if (!atOrAfter(nowMs, due)) wait = due - nowMs;
  }

  const int ceiling = budget_.maxTransactionsPerMinute > 0 ? budget_.maxTransactionsPerMinute : 1;
  // The oldest start still inside the minute; everything older has already left the window.
  size_t live = 0;
  uint32_t oldest = 0;
  for (size_t i = 0; i < count_; i++) {
    const uint32_t start = starts_[(first_ + i) % kMaxTracked];
    if (nowMs - start < kMinuteMs) {
      if (live == 0) oldest = start;
      live++;
    }
  }
  if (live >= static_cast<size_t>(ceiling)) {
    const uint32_t until = oldest + kMinuteMs;
    if (!atOrAfter(nowMs, until)) {
      const uint32_t window = until - nowMs;
      if (window > wait) wait = window;
    }
  }
  return wait;
}

void AirtimeGate::took(uint32_t nowMs) {
  // Drop whatever has left the minute, so the ring never holds more than a minute of starts.
  while (count_ > 0 && nowMs - starts_[first_] >= kMinuteMs) {
    first_ = (first_ + 1) % kMaxTracked;
    count_--;
  }
  if (count_ == kMaxTracked) {
    first_ = (first_ + 1) % kMaxTracked;
    count_--;
  }
  starts_[(first_ + count_) % kMaxTracked] = nowMs;
  count_++;
  last_ = nowMs;
  hasLast_ = true;
  transmissions_++;
}

}  // namespace mm
