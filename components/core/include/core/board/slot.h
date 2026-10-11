// The slot seam (task p1-06): "which radio" as a parameter, everywhere it will need to be one.
//
// spec/SYSTEM.md S-11: "The Walter addresses radios by *slot*; there is one slot today." A unit
// has one RAK4630 on the single adapter and that is slot 0. A dual adapter carrying two is
// designed and not built (the hardware repository's DUAL-RAK4630-FEASIBILITY.md): the same seven
// wires reach both, and a selector on the adapter - two flip-flops, no firmware - decides which.
// This header is the seam, so that the second radio is an addition and not a rewrite:
//
//   Slot          a radio's position: 0 or 1. Only 0 exists on a unit today.
//   TargetSelect  which radio the shared wires reach. SingleTarget accepts slot 0 and does
//                 nothing; DualTarget (board/dual_target.h) programs the adapter's selector and
//                 is compiled, tested and *not constructed by this firmware*.
//   SlotLock      one selection at a time: an operation on a slot selects it first and holds it,
//                 so that two operations on different slots cannot interleave - the link client
//                 is never mid-request when the UART is steered elsewhere.
//
// Logic: no ESP-IDF include, no heap, no pin of its own (DualTarget reaches the pins through
// RadioPins, which is the only object that touches them).
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace board {

// A radio's position on the adapter (S-11; spec/LINK-PROTOCOL.md §5.1 `slot`: "0 on a
// single-radio unit; the radio's position on a dual adapter"). A type and not a bare integer so
// that a slot cannot be passed where a count or an address is wanted.
struct Slot {
  std::uint8_t index = 0;

  constexpr Slot() = default;
  constexpr explicit Slot(std::uint8_t i) : index(i) {}
};

constexpr bool operator==(Slot a, Slot b) { return a.index == b.index; }
constexpr bool operator!=(Slot a, Slot b) { return a.index != b.index; }

// How wide the seam is: the dual adapter's two positions. Not how many a unit *has* - that is
// TargetSelect::slots(), which is 1 today. An index at or above this is not a slot at all and is
// refused by the parser, before anything is selected.
constexpr std::size_t kSlotCount = 2;

constexpr Slot kSlot0{0};  // the only radio of a unit today; the dual adapter's module A
constexpr Slot kSlot1{1};  // the dual adapter's module B

constexpr bool isSlot(unsigned index) { return index < kSlotCount; }

// Why a selection did not happen. kOk is the only one that means the wires now reach that slot.
enum class SelectResult : std::uint8_t {
  kOk,
  kNoSuchSlot,  // this adapter has no such position (slot 1 on a single-radio unit)
  kBusy,        // another slot's operation holds the selection; nothing was steered
  kRefused,     // the board refused to program the selector (the rail is off, SWD is held, ...)
};

const char* describe(SelectResult result);

// Which radio the seven shared wires reach. One implementation is constructed by the firmware
// (SingleTarget); the other (DualTarget) exists so that the board that needs it is a board and
// not a rewrite.
class TargetSelect {
 public:
  virtual ~TargetSelect() = default;

  // How many positions this adapter has: 1 or 2. A slot at or above it does not exist here.
  virtual std::size_t slots() const = 0;

  // Steers the debug lines, the reset and the UART to that radio. Idempotent, and asked before
  // every radio-facing operation - DUAL-RAK4630-FEASIBILITY.md §4 rule 2: "Program the select
  // register explicitly before every SWD or UART operation; assume nothing about its current
  // state."
  virtual SelectResult select(Slot slot) = 0;

  // Which slot the last successful select() steered to. Slot 0 before any.
  virtual Slot selected() const = 0;
};

// The single adapter (the only board today, S-11): slot 0 is the one radio, the wires reach it
// always, and selecting it does nothing at all - no pin moves, so a single-radio unit can never
// emit the selector's sequence. Any other slot is refused.
class SingleTarget final : public TargetSelect {
 public:
  std::size_t slots() const override { return 1; }
  SelectResult select(Slot slot) override {
    if (slot != kSlot0) {
      refusals_++;
      return SelectResult::kNoSuchSlot;
    }
    selects_++;
    return SelectResult::kOk;
  }
  Slot selected() const override { return kSlot0; }

  std::uint32_t selects() const { return selects_; }
  std::uint32_t refusals() const { return refusals_; }

 private:
  std::uint32_t selects_ = 0;
  std::uint32_t refusals_ = 0;
};

// One selection at a time. Every radio-facing operation begins by taking the slot it is for: the
// take selects it, and while it is held another slot is refused rather than steered under a
// request that is already on the wire. Takes of the *same* slot nest and are counted, because an
// operation calls others - the flasher identifies the chip inside its own hold.
//
// Not a mutex: this firmware's core runs in one task (docs/ARCHITECTURE.md §1). It is a guard
// against interleaving, which is what the hardware cannot survive.
class SlotLock {
 public:
  explicit SlotLock(TargetSelect& target) : target_(target) {}

  TargetSelect& target() { return target_; }
  std::size_t slots() const { return target_.slots(); }
  bool exists(Slot slot) const { return slot.index < target_.slots(); }

  // Selects `slot` and holds it until the matching release(). kBusy when another slot is held.
  SelectResult take(Slot slot);
  // Gives back one take's worth. Harmless when nothing is held.
  void release();

  bool held() const { return depth_ > 0; }
  Slot slot() const { return slot_; }
  std::uint32_t depth() const { return depth_; }
  std::uint32_t takes() const { return takes_; }
  std::uint32_t refusals() const { return refusals_; }

 private:
  TargetSelect& target_;
  Slot slot_{};
  std::uint32_t depth_ = 0;
  std::uint32_t takes_ = 0;
  std::uint32_t refusals_ = 0;
};

// take() and release() as one object, for the scope of an operation. Copying one would release
// the hold twice, so it cannot be copied or moved.
class SlotHold {
 public:
  SlotHold(SlotLock& lock, Slot slot) : lock_(lock), result_(lock.take(slot)) {}
  ~SlotHold() {
    if (result_ == SelectResult::kOk) lock_.release();
  }
  SlotHold(const SlotHold&) = delete;
  SlotHold& operator=(const SlotHold&) = delete;

  bool ok() const { return result_ == SelectResult::kOk; }
  SelectResult result() const { return result_; }

 private:
  SlotLock& lock_;
  const SelectResult result_;
};

}  // namespace board
}  // namespace core
