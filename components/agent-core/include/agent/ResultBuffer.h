// The offline buffer for job results: what was answered while the server was away.
//
// "Offline is normal" (EXIT-NODES §5): a result the server did not take is kept and posted
// later with the timestamps it was made with, and the server takes a late result and
// de-duplicates on the client id it already carries. The Pi agent keeps that buffer in
// `results/` on a disk; a board that is its own exit node has no disk, so this is a ring in RAM
// - bounded, oldest dropped, every drop counted - which is exactly what EXIT-NODES §5's "What
// it cannot do" says of this shape of exit node, and what `docs/field.md` tells whoever flashes
// one.
//
// The bound is two numbers: the bytes of JSON it will hold and how many results, whichever runs
// out first. Results are variable-length - a `status` result is a few hundred bytes and a
// `config.read` of the default list is several thousand - so the bodies live end to end in one
// arena and the index holds (job id, offset, length). Pushing into a full buffer drops from the
// oldest end until the new one fits; a single result larger than the whole arena is refused and
// counted as a drop, because dropping everything for one result would be worse than losing it.
//
// Host-portable: no Arduino, no board. test/test_buffers holds the bounds, the drop counting and
// the order without one.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

class ResultBuffer {
 public:
  // Twelve kilobytes and eight results. A claim hands over at most two jobs (kClaimMax) and a
  // claim only happens after a heartbeat the server answered, so results pile up only for the
  // jobs that were already running when it went away: two, plus whatever an earlier outage
  // left. Eight is room over; twelve kilobytes holds two of the largest results this firmware
  // writes (a config.read of the default list) or a dozen small ones.
  static constexpr size_t kCapacityBytes = 12288;
  static constexpr size_t kMaxResults = 8;

  // Keep one result. `len` is the body's length without its NUL. Returns false only when the
  // body is longer than the whole arena, which is counted as a drop like any other.
  bool push(long long jobId, const char* body, size_t len);

  size_t count() const { return count_; }
  bool empty() const { return count_ == 0; }
  size_t bytes() const { return used_; }

  // The oldest result, which is the next to be posted. `len` is its length; nullptr when empty.
  const char* oldest(long long& jobId, size_t& len) const;

  // Forget the oldest: what a result the server took costs.
  void popOldest();

  // How many results this buffer had to drop because it was full, and how many it has held in
  // all. `dropped` is what the heartbeat reports as `resultsDropped`.
  uint32_t dropped() const { return dropped_; }
  uint32_t held() const { return held_; }

  void clear();

 private:
  struct Entry {
    long long jobId = 0;
    size_t offset = 0;
    size_t len = 0;
  };

  // The arena is a queue, not a ring: the oldest is always at offset 0 and popping moves what is
  // left down. A result is posted far less often than it is written and the move is a few
  // kilobytes, so the simpler invariant is worth the memmove.
  char arena_[kCapacityBytes] = {0};
  Entry items_[kMaxResults];
  size_t count_ = 0;
  size_t used_ = 0;
  uint32_t dropped_ = 0;
  uint32_t held_ = 0;
};

}  // namespace mm
