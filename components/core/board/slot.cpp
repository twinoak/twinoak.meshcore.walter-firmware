#include "core/board/slot.h"

namespace core {
namespace board {

const char* describe(SelectResult result) {
  switch (result) {
    case SelectResult::kOk: return "ok";
    case SelectResult::kNoSuchSlot: return "no such slot on this adapter";
    case SelectResult::kBusy: return "refused: another slot's operation holds the selection";
    case SelectResult::kRefused: return "refused: the selector could not be programmed";
  }
  return "?";
}

// The order matters: a slot that does not exist is refused before anything is steered, and a
// nested take of the slot already held does not re-program the selector - it is the same
// selection, and the operation that holds it is in the middle of something.
SelectResult SlotLock::take(Slot slot) {
  if (!exists(slot)) {
    refusals_++;
    return SelectResult::kNoSuchSlot;
  }
  if (depth_ > 0) {
    if (slot != slot_) {
      refusals_++;
      return SelectResult::kBusy;
    }
    depth_++;
    takes_++;
    return SelectResult::kOk;
  }
  const SelectResult selected = target_.select(slot);
  if (selected != SelectResult::kOk) {
    refusals_++;
    return selected;
  }
  slot_ = slot;
  depth_ = 1;
  takes_++;
  return SelectResult::kOk;
}

void SlotLock::release() {
  if (depth_ > 0) depth_--;
}

}  // namespace board
}  // namespace core
