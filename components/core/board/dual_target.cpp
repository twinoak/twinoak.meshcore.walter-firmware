#include "core/board/dual_target.h"

namespace core {
namespace board {

// Compiled by the firmware build as well as by the host tests - and linked out of the image,
// because nothing constructs it (main/main.cpp builds a SingleTarget). That is deliberate: a
// change that breaks the dual adapter's selector breaks the build of the unit that will never
// use it, which is the cheapest possible guard on a seam nobody exercises yet.
SelectResult DualTarget::select(Slot slot) {
  if (!isSlot(slot.index)) {
    refusals_++;
    return SelectResult::kNoSuchSlot;
  }
  // false = module A, which is the flip-flops' RC-clear state and where a single adapter's one
  // radio sits (DUAL-RAK4630-FEASIBILITY.md §3). Both bits together: see the header.
  const bool b = slot == kSlot1;
  last_outcome_ = pins_.selectorProgram(b, b);
  if (last_outcome_ != Outcome::kOk) {
    refusals_++;
    return SelectResult::kRefused;
  }
  selected_ = slot;
  known_ = true;
  selects_++;
  return SelectResult::kOk;
}

}  // namespace board
}  // namespace core
