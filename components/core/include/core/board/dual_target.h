// The dual adapter's target selector (task p1-06). Compiled, tested against a simulated selector
// (host_test/sim/selector.h) - and **not constructed by this firmware**: a unit's board profile is
// `rak4630-single`, main/ builds a SingleTarget, and host_test/core/board/test_slot_single.cpp
// fails if a single-radio unit ever puts this sequence on the pins.
//
// It is written from the hardware repository's `hardware/pcb/DUAL-RAK4630-FEASIBILITY.md` at
// commit b798aab043281fa4c9b17802a164fa94be12e206, §2 and §3. What that document says, and why
// this is the shape of it:
//
//   §2.1  The nRF52840's SW-DP has no multi-drop, so two live debug ports cannot share one wire
//         pair - the SWD lines are *switched* to one module at a time (TS5A23157 analog SPDTs).
//   §2.2  nRESET is routed the same way, so flashing module B does not reboot module A.
//   §2.3  MeshCore's serial protocol is point-to-point with no addressing, so the UART is muxed
//         too: "the Walter just decides who it is talking to".
//   §3    The selector is two D flip-flops with an RC clear (SN74LVC1G175) behind a gate that
//         computes `SWDCLK AND NOT nRESET` (74LVC1G97). SWDIO is FF1's D; FF1's Q is DEBUG_SEL
//         (the SWD lines and the reset), FF2's Q is UART_SEL. The Walter programs them by holding
//         nRESET low and clocking two bits in, FF2's first. The RC clear makes the power-on state
//         0,0 - module A - "so a fresh unit behaves exactly like the single-RAK design until told
//         otherwise". The programming reset is filtered out by a ~2-5 ms RC and a Schmitt buffer
//         and never reaches a radio: "Flipping the UART from A to B does not reboot anything."
//   §4    The firmware's whole cost: never toggle SWDCLK while nRESET is asserted except here;
//         program the register before every SWD or UART operation and assume nothing about its
//         current state; real resets ≥ 50 ms, programming windows 100-500 µs.
//
// Everything that touches a pin is RadioPins' - this object's only operation on the world is
// `RadioPins::selectorProgram()`, which is the one place the sanctioned clock-while-reset lives
// and which applies the reset-spacing rule to the reset the window implies.
#pragma once

#include <cstdint>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"

namespace core {
namespace board {

class DualTarget final : public TargetSelect {
 public:
  explicit DualTarget(RadioPins& pins) : pins_(pins) {}

  std::size_t slots() const override { return kSlotCount; }

  // Programs both bits to the same radio: the debug lines, the reset and the UART all reach it.
  // §3's two independent bits ("the Walter can keep the live console on module A *while* it
  // flashes module B") are deliberately not used here - the slot seam is one selection at a time
  // (SlotLock), and splitting it is the dual board's task, when there is a board.
  SelectResult select(Slot slot) override;

  // Before the first select(), the flip-flops' RC clear: module A (§3, "Power-on default =
  // module A"). This firmware never relies on it - every operation re-programs the register -
  // but it is what the hardware answers with, so this is what it says.
  Slot selected() const override { return selected_; }

  bool everSelected() const { return known_; }
  std::uint32_t selects() const { return selects_; }
  std::uint32_t refusals() const { return refusals_; }
  Outcome lastOutcome() const { return last_outcome_; }

 private:
  RadioPins& pins_;
  Slot selected_ = kSlot0;
  bool known_ = false;
  std::uint32_t selects_ = 0;
  std::uint32_t refusals_ = 0;
  Outcome last_outcome_ = Outcome::kOk;
};

}  // namespace board
}  // namespace core
