// The console's `agent id`, `agent heartbeat` and `agent obs`, behind core/'s seam (task p4-04):
// `core::console::AgentProbe` over this task's identity, observations and platform. The console
// prints what it is given; this is what gives it.
//
// It is the one thing in this task that **reads the radio from the console**, and it reads the
// least it can: `identity()` refreshes, which asks §5.6 once a boot and §5.7's `get radio` once
// after that (and nothing at all when both are known and the radio has not restarted) - and
// hands the key it found to the local door and the §7 drain, as `AgentBackend` does before every
// wake (task p8-04), so that the job types this line prints are the ones the mesh can carry.
// `heartbeat()` and `observations()` ask the radio nothing; `pull()` is §7 and is the bench's
// way to put records in the ring before task p4-05's wake drives the drain itself.
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/IJobExtension.h>

#include "core/console/agent_probe.h"
#include "core/cycle/duty_cycle.h"
#include "core/cycle/wake_counters.h"
#include "walter_agent/identity.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/observations.h"
#include "walter_agent/walter_platform.h"

namespace agent {

// The most a heartbeat body may be here, as `ModemUplink` sizes its own (task p4-03): the
// contract fixture is 544 bytes and a unit with thirteen job types, a board, the two optional
// fields and six error counters fits inside this with room.
constexpr std::size_t kHeartbeatBodyMax = 1536;

class IdentityProbe final : public core::console::AgentProbe {
 public:
  // `duty` is where the band and the interval come from (the intent the heartbeat promises);
  // `extension` is task p4-05's four `twinoak.*` job types, which do not exist yet. Both may be
  // null, and then the intent is the `no-reading` band's and the list is the thirteen alone.
  IdentityProbe(Identity& identity, LinkMesh& mesh, Observations& observations,
                WalterPlatform& platform, core::cycle::DutyCycle* duty = nullptr,
                const mm::IJobExtension* extension = nullptr);

  bool identity(core::console::AgentIdentityView* out) override;
  const char* heartbeat(std::size_t* length) override;
  void observations(core::console::AgentObservationsView* out) override;
  std::size_t pull() override;

  // What the console does not print but a wake does: the intent as it stands.
  mm::WakeIntent intent() const;
  // The heartbeat's extras as this build can fill them (walter_agent/identity.h): the mesh's
  // `overheard`, the ring's drops and the platform's reboot count. Task p4-05 fills the loop's
  // own three from the loop.
  HeartbeatExtras extras() const;

  // What woke this unit, so that `agent heartbeat` prints the same four counters the wake's own
  // heartbeat posts (task p13-02, core/cycle/wake_counters.h). Null leaves them out, which is a
  // build with no settings store to keep them in.
  void countWakesFrom(const core::cycle::WakeTotals* wakes) { wakes_ = wakes; }

 private:
  Identity& identity_;
  LinkMesh& mesh_;
  Observations& observations_;
  WalterPlatform& platform_;
  core::cycle::DutyCycle* duty_ = nullptr;
  const mm::IJobExtension* extension_ = nullptr;
  const core::cycle::WakeTotals* wakes_ = nullptr;
  // The body is built here and not on the caller's stack: the main task's stack is 3584 bytes
  // (task p4-03's note on the same buffer).
  char body_[kHeartbeatBodyMax] = {};
};

}  // namespace agent
