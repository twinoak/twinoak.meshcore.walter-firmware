#include "core/net/socket_rotation.h"

namespace core {
namespace net {

// Two ids, and the header's arrays are written out in full: a third would need its own
// initialiser as well as a thought about the rule.
static_assert(kRotationIds == 2, "the rule and its arrays are written for two ids");

// The whole point of keeping an id's accounting in one value is that one task can change it while
// another is reading it, so it must be a transition the hardware can make in one go rather than
// something with a lock inside it. Xtensa has the compare-exchange; this is here so that a
// toolchain which did not would say so at compile time and not on a bench.
static_assert(std::atomic<CloseState>::is_always_lock_free,
              "CloseState must be changed by one atomic instruction: the event task and the main "
              "task both write it");

void SocketRotation::useIds(int first, int second) {
  id_[0] = first;
  id_[1] = (second == first) ? 0 : second;
  slot_ = -1;
  live_ = false;
  for (int i = 0; i < kRotationIds; ++i) {
    state_[i].store(CloseState::kClear, std::memory_order_release);
    ring_[i].store(false, std::memory_order_relaxed);
  }
}

int SocketRotation::slotOf(int id) const {
  if (id == 0) return -1;
  for (int i = 0; i < kRotationIds; ++i) {
    if (id_[i] == id) return i;
  }
  return -1;
}

CloseState SocketRotation::apply(int slot, CloseStep step) {
  CloseState was = stateOf(slot);
  while (!state_[slot].compare_exchange_weak(was, after(step, was), std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    // `was` now holds what the other task left there: the transition is taken again from it, so
    // the two tasks' steps always come out in one order or the other and never half of each.
  }
  return was;
}

int SocketRotation::nextId() const {
  if (id_[1] == 0) return id_[0];
  // The one the last connection was not on. Before the first connection, the first id.
  return slot_ == 0 ? id_[1] : id_[0];
}

int SocketRotation::currentId() const { return slot_ < 0 ? 0 : id_[slot_]; }

bool SocketRotation::owes(int id) const {
  const int slot = slotOf(id);
  if (slot < 0) return false;
  const CloseState state = stateOf(slot);
  return state == CloseState::kOwed || state == CloseState::kLastChance;
}

bool SocketRotation::isQuiet(int id) const {
  const int slot = slotOf(id);
  return slot >= 0 && stateOf(slot) == CloseState::kQuiet;
}

std::size_t SocketRotation::recentCount() const {
  return connections_ < kRecentConnections ? connections_ : kRecentConnections;
}

int SocketRotation::recentId(std::size_t which) const {
  const std::size_t count = recentCount();
  if (which >= count) return 0;
  return recent_[(connections_ - count + which) % kRecentConnections];
}

int SocketRotation::begin(int id, std::uint64_t deadline_ms) {
  const int slot = slotOf(id);
  if (slot < 0) return 0;

  // A close this id still owes: wait for it, briefly, in blocking steps. When it arrives,
  // onClose() pays the debt and sets no flag, so this connection starts with a clean slate; when
  // it does not, the debt goes into its last chance and the first close seen here pays it
  // instead - and if none is seen by the end of this connection, end() writes it off.
  if (stateOf(slot) == CloseState::kOwed) {
    ++owed_waits_;
    const std::uint64_t now = clock_.nowMs();
    std::uint64_t until = now + kOwedCloseWaitMs;
    if (until > deadline_ms) until = deadline_ms;
    while (stateOf(slot) == CloseState::kOwed && clock_.nowMs() < until) {
      clock_.delayMs(kOwedCloseStepMs);
    }
  }
  if (apply(slot, CloseStep::kBegin) == CloseState::kOwed) ++owed_timeouts_;

  ring_[slot].store(false, std::memory_order_relaxed);
  recent_[connections_ % kRecentConnections] = static_cast<std::uint8_t>(id);
  slot_ = slot;
  live_ = true;
  ++connections_;
  return id;
}

void SocketRotation::end() {
  if (!live_) return;
  live_ = false;
  // A debt that has had a whole connection on its id and is still unpaid is one nobody is going
  // to pay: it is written off and the id goes quiet (socket_rotation.h, *A debt lives for one
  // connection*). Otherwise: the close seen is the close accounted for, and a close not seen is
  // one on its way.
  if (apply(slot_, CloseStep::kEnd) == CloseState::kLastChance) ++write_offs_;
}

void SocketRotation::cancel() {
  if (!live_) return;
  live_ = false;
  apply(slot_, CloseStep::kCancel);
}

void SocketRotation::onRing(int id) {
  const int slot = slotOf(id);
  if (slot < 0) {
    other_id_events_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ring_[slot].store(true, std::memory_order_relaxed);
}

void SocketRotation::onClose(int id) {
  const int slot = slotOf(id);
  if (slot < 0) {
    other_id_events_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const CloseState was = apply(slot, CloseStep::kCloseEvent);
  if (was == CloseState::kOwed || was == CloseState::kLastChance) {
    // The close of an earlier connection on this id, however late: it paid the debt and told the
    // connection in progress nothing.
    discounted_closes_.fetch_add(1, std::memory_order_relaxed);
  }
}

bool SocketRotation::closedSeen() const {
  return slot_ >= 0 && stateOf(slot_) == CloseState::kSeen;
}

bool SocketRotation::ringSeen() const {
  return slot_ >= 0 && ring_[slot_].load(std::memory_order_relaxed);
}

void SocketRotation::clearRing() {
  if (slot_ >= 0) ring_[slot_].store(false, std::memory_order_relaxed);
}

}  // namespace net
}  // namespace core
