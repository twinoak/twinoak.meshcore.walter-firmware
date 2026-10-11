#include "core/flasher/board_profile.h"

namespace core {
namespace flasher {

namespace {

bool same(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  while (*a != 0 && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// One slot, one board (S-11). The table is indexed by slot so that a second radio is a row.
const BoardProfile* const kBySlot[kSlotsPresent] = {&kRak4630Single};

}  // namespace

const BoardProfile* profileForSlot(board::Slot slot) {
  if (slot.index >= kSlotsPresent) return nullptr;
  return kBySlot[slot.index];
}

const BoardProfile* findProfile(board::Slot slot, const char* board_name) {
  const BoardProfile* p = profileForSlot(slot);
  if (p == nullptr) return nullptr;
  return same(p->name, board_name) ? p : nullptr;
}

}  // namespace flasher
}  // namespace core
