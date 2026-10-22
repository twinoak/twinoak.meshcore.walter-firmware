// A Walter's mesh client on a bench of fakes (task p4-02): the fake pins with their rules, the
// port and the link client of task p1-02, a simulated RAK (sim/sim_rak.h) with a repeater behind
// it, and the agent's LinkMesh on top - the whole road from `mm::IMesh` to the repeater's answer,
// through the UART framing. Every test of components/agent/ starts from one of these, and the
// library's own runner scenarios run through one (host_test/agent-link/shim/support/FakeMesh.h).
//
// The clock is the caller's, because the repeater behind the RAK keeps its own (the library's
// FakePlatform) and the two have to move together: the harness ties them (the shim's
// FakePlatform), and this world only asks for the one the link runs on.
#pragma once

#include <cstdint>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/sim_rak.h"
#include "walter_agent/link_mesh.h"

namespace sim {

struct MeshWorld {
  fakes::FakeClock& clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  core::board::RadioPins board{pins, clock, retained};
  SimRak rak;
  fakes::FakeWallClock wall;
  // A single-radio unit (task p1-06): slot 0, selecting it moves no pin.
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{board, rak};
  core::link::client::Client client{port, clock, wall, slots};
  agent::LinkMesh mesh{client, clock};

  // `hello` false is a radio nobody has spoken to yet: no session, so the mesh client answers
  // supports() false for everything and every send is refused by the client without a byte.
  MeshWorld(fakes::FakeClock& the_clock, mm::IMesh& repeater, bool hello = true)
      : clock(the_clock), rak(the_clock, repeater) {
    // The rail is "unknown" for the settle time after boot (ARCHITECTURE §2 rule 3): let it pass,
    // as the firmware does by the time anything speaks on the link.
    clock.advance(core::board::kKillTiming.settle_ms);
    board.safe();
    if (hello) client.hello();
  }

  // How many SEND_* requests the RAK executed and answered with a packet sent (§6.2: `send` 1 or
  // 2) - retries not counted (§3), refusals not counted: what client_tx must equal.
  std::uint32_t sendsExecuted() const {
    std::uint32_t n = 0;
    for (const ScriptedRadio::Seen& one : rak.radio.requests) {
      if (!one.executed) continue;
      if (one.type < core::link::type::kSendLogin || one.type > core::link::type::kSendTrace) continue;
      if (one.response.size() < 2 || one.response[0] != core::link::status::kOk) continue;
      if (one.response[1] != core::link::send_result::kNothingSent) ++n;
    }
    return n;
  }

  // How many requests of this type the RAK saw, retries included.
  std::uint32_t requestsOf(std::uint8_t type) const {
    std::uint32_t n = 0;
    for (const ScriptedRadio::Seen& one : rak.radio.requests) n += one.type == type;
    return n;
  }
};

}  // namespace sim
