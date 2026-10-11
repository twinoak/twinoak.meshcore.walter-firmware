// A Walter and a *dual* adapter on a bench of fakes (task p1-06): the fake pins that check the
// wire rules, the simulated selector between them and the Walter, two simulated nRF52840s behind
// two wire-level debug ports, two scripted radios behind the UART's mux, and the firmware's own
// DualTarget steering all of it. The board this is for does not exist
// (DUAL-RAK4630-FEASIBILITY.md: "feasible", not "built"), and neither does any unit that
// constructs DualTarget - this bench is the only place it is ever constructed.
//
// What it is for: showing that the slot is a parameter and not a wish. A test can flash slot 1
// and read slot 0's flash log, say hello to A and see that B heard nothing, and watch a reset
// pulse reach one module and not the other.
#pragma once

#include <doctest.h>

#include "core/board/dual_target.h"
#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/flasher/flasher.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/nrf52_chip.h"
#include "sim/scripted_radio.h"
#include "sim/selector.h"
#include "sim/swd_target.h"

namespace sim {

struct DualBench {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  Selector selector{pins, clock};
  SwdTarget target_a{pins};
  SwdTarget target_b{pins};
  Nrf52Chip chip_a{clock};
  Nrf52Chip chip_b{clock};
  fakes::FakeRetained retained;
  core::board::RadioPins board{selector, clock, retained};
  core::board::DualTarget dual{board};
  core::board::SlotLock slots{dual};

  ScriptedRadio radio_a{clock};
  ScriptedRadio radio_b{clock};
  UartMux uart{selector, radio_a, radio_b};
  fakes::FakeWallClock wall;
  core::link::client::LinkPort port{board, uart};
  core::link::client::Client client{port, clock, wall, slots};
  core::flasher::Flasher flasher{board, clock, slots};

  DualBench() {
    selector.attach(core::board::kSlot0, &target_a);
    selector.attach(core::board::kSlot1, &target_b);
    chip_a.attachTo(target_a);
    chip_b.attachTo(target_b);
    chip_a.installSoftDevice();
    chip_b.installSoftDevice();
    // The two radios are told apart by their boot_id, so a test can say which one answered.
    radio_a.boot_id = 0x0A0A0A0A;
    radio_b.boot_id = 0x0B0B0B0B;
    pins.record_log = false;  // a flash is millions of transitions (task p1-05)
    // The rail is "unknown" for the settle time after boot (ARCHITECTURE §2 rule 3): let it pass.
    clock.advance(core::board::kKillTiming.settle_ms);
    board.safe();
  }

  ~DualBench() { CHECK(pins.violations.empty()); }

  Nrf52Chip& chip(core::board::Slot slot) { return slot == core::board::kSlot0 ? chip_a : chip_b; }
  ScriptedRadio& radio(core::board::Slot slot) {
    return slot == core::board::kSlot0 ? radio_a : radio_b;
  }
};

}  // namespace sim
