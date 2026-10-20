// A Walter and a simulated nRF52840 on a bench of fakes (task p1-04): task p1-03's SwdBench - the
// fake pins that check the pin rules, the wire-level target, the clock whose delays take no time,
// RadioPins, the wire and the debug port - with the simulated chip plugged into the target's two
// access ports and the nRF52 layer under test over the debug port. Every test of core/nrf52/ and
// of the memory access port starts from one of these.
#pragma once

#include <doctest.h>

#include "core/nrf52/nrf52.h"
#include "core/swd/mem_ap.h"
#include "sim/nrf52_chip.h"
#include "sim/swd_bench.h"

namespace sim {

struct Nrf52Bench : SwdBench {
  Nrf52Chip chip{clock};
  core::nrf52::Nrf52 nrf{dp, clock};

  Nrf52Bench() {
    chip.attachTo(target);
    // When the chip loses power the target behind the lines goes with it: nothing answers.
    chip.on_power_loss = [this] { target.silent = true; };
    // And the reset line reaches the chip (task p5-02): a pulse through RadioPins is a pin reset
    // there, which re-reads the protection as the part's kind says and resets the debug port.
    target.on_pin_reset = [this] { chip.pinReset(); };
  }

  // The usual start of a test: the lines acquired, the session open, the chip identified.
  core::nrf52::Result open() {
    REQUIRE(acquire() == core::board::Outcome::kOk);
    return nrf.open();
  }
  void close() {
    nrf.close();
    release();
  }
  // The power back after the chip lost it (task p1-05): the chip as a power-on reset leaves it,
  // the target answering again. The Walter's side keeps nothing: the flasher opens a new session.
  void powerCycle() {
    chip.powerOn();
    target.silent = false;
  }
};

}  // namespace sim
