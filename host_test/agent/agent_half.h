// The agent's half of a Walter on the wake bench (task p4-05): everything main/main.cpp
// constructs for the agent, over the bench's fakes, built per life of the Walter - a wake from
// deep sleep is the Walter destroyed and built again (host_test/core/cycle/wake_bench.h), and
// the loop and the runner go with it, as their RAM does on a board. What outlives a life is the
// world's: the loop's state in the key-value store, the kept image in its flash, the scripted
// meshmaster.dk, the radio and the repeater behind it.
//
// The objects are main.cpp's, in main.cpp's order, so that the bench is the board: the mesh over
// the link client, the uplink over the HTTPS client, the platform over the bench's clock and
// fakes, the identity, the §7 drain, the firmware jobs over the store and the two machines, the
// kept image's store over a flash window, the backend over all of it. `pump` is the library's
// one plain function pointer, which on a board is the console's; here it polls the link.
#pragma once

#include <cstdint>

#include <agent/AgentLoop.h>
#include <agent/JobRunner.h>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/cycle/agent_attention.h"
#include "core/cycle/duty_cycle.h"
#include "core/cycle/network_clock.h"
#include "core/link/client/client.h"
#include "core/net/client.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/update/fetch.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "fakes/fake_entropy.h"
#include "fakes/fake_heap.h"
#include "interfaces/clock.h"
#include "interfaces/flash_region.h"
#include "interfaces/key_value.h"
#include "walter_agent/agent_backend.h"
#include "walter_agent/agent_probe.h"
#include "walter_agent/agent_uplink_check.h"
#include "walter_agent/firmware_jobs.h"
#include "walter_agent/identity.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/mesh_probe.h"
#include "walter_agent/modem_uplink.h"
#include "walter_agent/observations.h"
#include "walter_agent/region_store.h"
#include "walter_agent/uplink_probe.h"
#include "walter_agent/walter_platform.h"

namespace bench {

// What a life of the Walter hands the half: the bench's objects it is built over.
struct AgentHalfParts {
  core::link::client::Client* link = nullptr;
  core::settings::Settings* settings = nullptr;
  core::net::Client* http = nullptr;
  interfaces::Clock* clock = nullptr;
  core::cycle::NetworkClock* wall = nullptr;
  core::cycle::DutyCycle* duty = nullptr;
  core::update::Journal* journal = nullptr;
  core::image::Store* store = nullptr;
  core::update::RadioUpdate* update = nullptr;
  core::update::SelfUpdate* self = nullptr;
  core::update::Fetch* fetch = nullptr;
  core::supervisor::Supervisor* supervisor = nullptr;
  core::board::RadioPins* pins = nullptr;
  core::board::SlotLock* slots = nullptr;
  interfaces::KeyValue* state_store = nullptr;   // the world's: NVS
  interfaces::FlashRegion* kept_flash = nullptr; // the world's: the rakfw partition's tail
  fakes::FakeEntropy* entropy = nullptr;         // the world's: so two lives draw different numbers
  fakes::FakeHeap* heap = nullptr;
  agent::AgentUplinkCheck* trial = nullptr;      // the life's: the self-update was built over it
  const char* version = "0.0.0-dev";
};

struct AgentHalf {
  AgentHalfParts p;
  core::cycle::AgentAttention attention;
  agent::LinkMesh mesh;
  agent::LinkMeshProbe mesh_probe{mesh};
  agent::ModemUplink server;
  agent::ModemUplinkProbe uplink_probe{server};
  agent::WalterPlatform platform;
  agent::Identity identity;
  agent::Observations observations;
  agent::FirmwareJobs firmware;
  agent::IdentityProbe identity_probe;
  agent::RegionStore kept;
  mm::JobRunner jobs;
  mm::AgentLoop loop;
  std::uint8_t kept_scratch[agent::kKeptScratchBytes];
  agent::AgentBackend backend;
  int pumps = 0;

  explicit AgentHalf(const AgentHalfParts& parts)
      : p(parts),
        attention(&parts.duty->reason(), parts.journal),
        mesh(*parts.link, *parts.clock),
        server(*parts.http, *parts.settings, *parts.clock, *parts.wall, parts.version),
        platform(*parts.clock, *parts.wall, *parts.entropy, *parts.heap, &attention),
        identity(*parts.link, parts.version),
        observations(*parts.link, mesh),
        firmware(jobParts(parts), *parts.pins, *parts.slots, *parts.link, server, *parts.settings, *parts.clock,
                 *parts.wall),
        identity_probe(identity, mesh, observations, platform, parts.duty, &firmware),
        kept(*parts.kept_flash, 0, parts.kept_flash->size()),
        backend(backendParts(), *parts.clock, *parts.wall) {
    server.raiseAttentionThrough(&attention);
    current_ = this;
    backend.begin(&pumpAll);
  }
  ~AgentHalf() {
    if (current_ == this) current_ = nullptr;
  }

  // What the library's waits run: the link polled, as the console's pump does on a board.
  static void pumpAll() {
    if (current_ != nullptr) {
      ++current_->pumps;
      current_->p.link->poll();
    }
  }

 private:
  static agent::FirmwareJobs::Parts jobParts(const AgentHalfParts& parts) {
    agent::FirmwareJobs::Parts j;
    j.store = parts.store;
    j.update = parts.update;
    j.self = parts.self;
    j.fetch = parts.fetch;
    j.journal = parts.journal;
    j.supervisor = parts.supervisor;
    j.guard = &parts.duty->guard();
    return j;
  }
  agent::AgentBackend::Parts backendParts() {
    agent::AgentBackend::Parts b;
    b.mesh = &mesh;
    b.server = &server;
    b.platform = &platform;
    b.identity = &identity;
    b.observations = &observations;
    b.jobs = &jobs;
    b.loop = &loop;
    b.firmware = &firmware;
    b.trial = p.trial;
    b.state_store = p.state_store;
    b.kept = kept.usable() ? &kept : nullptr;
    b.kept_scratch = kept_scratch;
    b.kept_scratch_len = sizeof kept_scratch;
    b.duty = p.duty;
    b.self = p.self;
    b.update = p.update;
    return b;
  }

  static AgentHalf* current_;
};

inline AgentHalf* AgentHalf::current_ = nullptr;

}  // namespace bench
