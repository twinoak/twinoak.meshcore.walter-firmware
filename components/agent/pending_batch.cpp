#include "walter_agent/pending_batch.h"

#include <cstring>

#include "walter_agent/call_log.h"

namespace agent {
namespace {

std::uint16_t saturate16(std::uint16_t n) {
  return n == 0xFFFFu ? n : static_cast<std::uint16_t>(n + 1);
}

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

PendingBatch::PendingBatch(interfaces::KeyValue* store) : store_(store) {}

std::uint32_t PendingBatch::fingerprintOf(const char* id) {
  if (id == nullptr || id[0] == 0) return 0;
  // FNV-1a, 32 bits: the offset basis and the prime of Fowler-Noll-Vo, which is four lines and
  // no table. Nothing here depends on it being any particular hash - only on two different ids
  // almost never giving the same answer.
  std::uint32_t hash = 2166136261u;
  for (const char* c = id; *c != 0; ++c) {
    hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(*c));
    hash *= 16777619u;
  }
  return hash == 0 ? 1u : hash;
}

void PendingBatch::begin(const mm::AgentLoop* loop) {
  begun_ = true;
  loop_ = loop;
  stored_tag_ = 0;
  since_wake_ = 0;
  survived_ = false;
  dirty_ = false;
  if (store_ == nullptr || !store_->ready()) return;
  // A local and not a member: fifteen bytes, wanted once a boot, and every byte of this object is
  // .bss the main task's stack has to be found around afterwards (docs/ARCHITECTURE.md §14).
  char hex[kPendingBatchHexBytes] = {};
  if (!store_->read(kPendingBatchKey, hex, sizeof hex)) return;
  decode(hex);
}

void PendingBatch::decode(const char* hex) {
  if (std::strlen(hex) != kPendingBatchHexBytes - 1) {
    writes_.refused = saturate16(writes_.refused);
    return;
  }
  std::uint8_t bytes[kPendingBatchBytes] = {};
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    const int hi = hexValue(hex[2 * i]);
    const int lo = hexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      writes_.refused = saturate16(writes_.refused);
      return;
    }
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  if (bytes[0] != kPendingBatchVersion) {
    writes_.refused = saturate16(writes_.refused);
    return;
  }
  since_wake_ = get16(bytes + 1);
  stored_tag_ = get32(bytes + 3);
  survived_ = stored_tag_ != 0;
}

void PendingBatch::atWake(std::uint32_t wake) {
  // The same clamp the failed calls' list uses, and for the same reason: a number above it is
  // recorded as it rather than wrapping to a small one that reads as recent.
  wake_ = wake > kWakeAtMost ? static_cast<std::uint16_t>(kWakeAtMost) : static_cast<std::uint16_t>(wake);
}

void PendingBatch::note() {
  const std::uint32_t tag = waiting() ? fingerprintOf(id()) : 0;
  // The same batch as the stored wake is about, or no batch where there was none: the answer this
  // object gives has not changed and nothing is written. This is the common case at every wake.
  if (tag == stored_tag_) return;
  stored_tag_ = tag;
  // A batch that has just started waiting has been waiting since **this** wake; one that is gone
  // leaves no wake behind it.
  since_wake_ = tag == 0 ? 0 : wake_;
  dirty_ = true;
}

bool PendingBatch::save() {
  if (!dirty_) return true;
  if (store_ == nullptr || !store_->ready()) return false;
  if (stored_tag_ == 0) {
    // Nothing is waiting: the key goes rather than being written as a zero, so that a unit whose
    // posts are answered carries no value of this at all.
    if (!store_->erase(kPendingBatchKey)) return false;
  } else {
    std::uint8_t bytes[kPendingBatchBytes] = {};
    bytes[0] = kPendingBatchVersion;
    put16(bytes + 1, since_wake_);
    put32(bytes + 3, stored_tag_);
    char hex[kPendingBatchHexBytes] = {};
    const char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < sizeof bytes; ++i) {
      hex[2 * i] = digits[bytes[i] >> 4];
      hex[2 * i + 1] = digits[bytes[i] & 0x0Fu];
    }
    hex[2 * sizeof bytes] = 0;
    if (!store_->write(kPendingBatchKey, hex)) return false;
  }
  writes_.writes = saturate16(writes_.writes);
  dirty_ = false;
  return true;
}

bool PendingBatch::waiting() const { return loop_ != nullptr && loop_->pendingBatchCount() != 0; }

std::size_t PendingBatch::count() const { return loop_ == nullptr ? 0 : loop_->pendingBatchCount(); }

const char* PendingBatch::id() const { return loop_ == nullptr ? "" : loop_->pendingBatchId(); }

bool PendingBatch::sinceKnown() const {
  return waiting() && stored_tag_ != 0 && stored_tag_ == fingerprintOf(id());
}

void PendingBatch::forget() {
  stored_tag_ = 0;
  since_wake_ = 0;
  survived_ = false;
  dirty_ = false;
  if (store_ != nullptr && store_->ready()) (void)store_->erase(kPendingBatchKey);
}

}  // namespace agent
