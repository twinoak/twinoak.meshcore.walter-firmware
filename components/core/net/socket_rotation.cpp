#include "core/net/socket_rotation.h"

namespace core {
namespace net {

void SocketRotation::useIds(int first, int second) {
  id_[0] = first;
  id_[1] = (second == first) ? 0 : second;
  slot_ = -1;
  live_ = false;
  for (int i = 0; i < kRotationIds; ++i) {
    closed_[i] = false;
    ring_[i] = false;
    owed_[i] = false;
  }
}

int SocketRotation::slotOf(int id) const {
  if (id == 0) return -1;
  for (int i = 0; i < kRotationIds; ++i) {
    if (id_[i] == id) return i;
  }
  return -1;
}

int SocketRotation::nextId() const {
  if (id_[1] == 0) return id_[0];
  // The one the last connection was not on. Before the first connection, the first id.
  return slot_ == 0 ? id_[1] : id_[0];
}

int SocketRotation::currentId() const { return slot_ < 0 ? 0 : id_[slot_]; }

bool SocketRotation::owes(int id) const {
  const int slot = slotOf(id);
  return slot >= 0 && owed_[slot];
}

int SocketRotation::begin(int id, std::uint64_t deadline_ms) {
  const int slot = slotOf(id);
  if (slot < 0) return 0;

  // A close this id still owes: wait for it, briefly, in blocking steps. When it arrives,
  // onClose() pays the debt and sets no flag, so this connection starts with a clean slate;
  // when it does not, the debt stays and the first close seen here pays it instead.
  if (owed_[slot]) {
    ++owed_waits_;
    const std::uint64_t now = clock_.nowMs();
    std::uint64_t until = now + kOwedCloseWaitMs;
    if (until > deadline_ms) until = deadline_ms;
    while (owed_[slot] && clock_.nowMs() < until) clock_.delayMs(kOwedCloseStepMs);
    if (owed_[slot]) ++owed_timeouts_;
  }

  ring_[slot] = false;
  closed_[slot] = false;
  slot_ = slot;
  live_ = true;
  ++connections_;
  return id;
}

void SocketRotation::end() {
  if (!live_) return;
  live_ = false;
  const int slot = slot_;
  // Was the close seen? If not, it is still on its way and this id owes one. The flag is read
  // again afterwards because the event task may have set it between the two lines, in which case
  // nothing is owed - the close has already been accounted for.
  const bool seen = closed_[slot];
  owed_[slot] = !seen;
  if (!seen && closed_[slot]) owed_[slot] = false;
}

void SocketRotation::cancel() { live_ = false; }

void SocketRotation::onRing(int id) {
  const int slot = slotOf(id);
  if (slot < 0) {
    ++other_id_events_;
    return;
  }
  ring_[slot] = true;
}

void SocketRotation::onClose(int id) {
  const int slot = slotOf(id);
  if (slot < 0) {
    ++other_id_events_;
    return;
  }
  if (owed_[slot]) {
    // The close of an earlier connection on this id, however late. It pays the debt and tells
    // the connection in progress nothing.
    owed_[slot] = false;
    ++discounted_closes_;
    return;
  }
  closed_[slot] = true;
}

bool SocketRotation::closedSeen() const { return slot_ >= 0 && closed_[slot_]; }

bool SocketRotation::ringSeen() const { return slot_ >= 0 && ring_[slot_]; }

void SocketRotation::clearRing() {
  if (slot_ >= 0) ring_[slot_] = false;
}

}  // namespace net
}  // namespace core
