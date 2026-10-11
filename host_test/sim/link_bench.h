// A Walter and a radio on a bench of fakes (task p1-02): the fake pins that check the rules of
// docs/ARCHITECTURE.md §2 on every transition, a clock whose delays take no time, the scripted
// radio on the other end of the UART, and the client under test over the port that sequences the
// two. Every test of core/link/client/ starts from one of these.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/scripted_radio.h"

namespace sim {

struct LinkBench {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  core::board::RadioPins board{pins, clock, retained};
  ScriptedRadio radio{clock};
  fakes::FakeWallClock wall;
  // A single-radio unit (task p1-06): slot 0, selecting it moves no pin.
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{board, radio};
  core::link::client::Client client{port, clock, wall, slots};

  LinkBench() {
    // The rail is "unknown" for the settle time after boot (ARCHITECTURE §2 rule 3): let it pass,
    // as the firmware does by the time a console is open, so that a test is not measuring that.
    clock.advance(core::board::kKillTiming.settle_ms);
    board.safe();
  }

  // What the client has put on the wire, as frames: every write it made, with the sender's idle
  // 0x00 stripped (§2: that byte is the line's, not the frame's).
  std::vector<std::vector<std::uint8_t>> frames() const {
    std::vector<std::vector<std::uint8_t>> out;
    for (const std::vector<std::uint8_t>& write : radio.writes) {
      std::vector<std::uint8_t> frame = write;
      while (!frame.empty() && frame.front() == 0) {
        frame.erase(frame.begin());
      }
      if (!frame.empty()) {
        out.push_back(frame);
      }
    }
    return out;
  }

  // A session: HELLO answered, so the capabilities are known and the mesh calls are allowed.
  void sayHello() { client.hello(); }
};

}  // namespace sim
