// The backend that does nothing (task p3-05), and **this firmware's default**.
//
// `interfaces/backend.h` is the seam the wake cycle's step 8 talks to. This is the only
// implementation in this repository: it answers *nothing to do*, every wake, for ever. It is not
// a stub that will be filled in here - it is the honest shape of a unit whose server side is not
// decided (spec/SYSTEM.md §8, spec/proposals/). A unit running this image wakes, looks after its
// radio, keeps its readings and sleeps, and asks nobody anything.
//
// What it costs to leave it in: nothing. What it buys: the whole of the wake cycle - the order,
// the budgets, the refusals, the record - is built, tested and running before anybody writes a
// line of protocol, and the agent's work in phase 4 is one class behind one interface.
#pragma once

#include <cstdint>

#include "interfaces/backend.h"

namespace core {
namespace cycle {

class NullBackend final : public interfaces::Backend {
 public:
  // How many times it was asked. Not a counter anybody needs; it is what the console's `cycle`
  // prints so that a person can see the step ran and answered, rather than wondering whether it
  // was skipped.
  std::uint32_t exchanges() const { return exchanges_; }

  interfaces::Exchanged exchange(const interfaces::BackendKnowledge& known,
                                 std::uint64_t deadline_ms,
                                 interfaces::BackendAnswer* out) override {
    (void)known;
    (void)deadline_ms;
    ++exchanges_;
    // *Nothing to do*, and it says why nobody was reached (task p10-02): there is no server to
    // reach. A build running this one must never read *the backend was reached* in its journal,
    // because nothing ever was - and `Reached::kUnsaid` would have left the question open rather
    // than answering it.
    if (out != nullptr) out->reached = interfaces::Reached::kNotConfigured;
    return interfaces::Exchanged::kNothing;
  }

 private:
  std::uint32_t exchanges_ = 0;
};

}  // namespace cycle
}  // namespace core
