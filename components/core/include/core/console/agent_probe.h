// The console's seam into the agent's identity (task p4-04): `agent id` - who this exit node
// says it is - `agent heartbeat` - the body it would post, so that a person on the bench can
// compare it with meshmaster.dk's contract fixture by eye - and `agent obs` - the observation
// ring. The identity itself is components/agent/'s, over the box link and the copied library,
// and core/ cannot name it (agent/ is built on core/, not the other way round), so the console
// takes this interface and main/ hands it the implementation
// (components/agent/include/walter_agent/agent_probe.h). A build without one answers in one line.
//
// What crosses it: the identity's fields as the heartbeat would carry them, the heartbeat's own
// bytes, and numbers. **No token and no password crosses it in any field** - the token is not
// the identity's to hold and `agent id` says *set* or *unset* from the settings, as
// `get server.token` does - and the observation rows carry no advert name, because a name is
// something a stranger chose and neither end logs it (spec/LINK-PROTOCOL.md §7,
// spec/SYSTEM.md S-12).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/radio_identity.h"

namespace core {
namespace console {

struct AgentIdentityView {
  // Whether the radio has an identity at all. False is a unit registered before its repeater:
  // it still checks in, and the heartbeat then carries no key, no radio block and no job types.
  bool known = false;
  bool session = false;  // a HELLO has been answered in this boot
  // **Which of the three states** (task p10-03, core/link/radio_identity.h): `known` is
  // `kKnown`, and the other two are what `known == false` covered alone - a radio awaiting its
  // first install, and a radio that **refuses** §5.6 because it has lost its identity. The two
  // need opposite things, so `agent id` says which, with its evidence.
  link::IdentityState identity = link::IdentityState::kNotMadeYet;
  bool caps_identity_lost = false;  // the session advertises the lost mode's `caps 0x21`

  char public_key[65] = {};   // 64 lowercase hex, or empty
  char companion_name[49] = {};  // §5.6's advert name - the unit's **own** repeater's
  char companion_firmware[49] = {};
  char companion_board[49] = {};
  char companion_upstream[49] = {};
  char agent_version[33] = {};
  char board[16] = {};
  char transport[16] = {};
  std::uint32_t boot_id = 0;

  bool radio_known = false;
  double freq_mhz = 0;
  double bw_khz = 0;
  int sf = 0;
  int cr = 0;

  // The job types the heartbeat would advertise, as one line of comma-separated words: the
  // library's thirteen less what the mesh cannot carry, then any extension's. Empty is an empty
  // list, which is what a unit with no radio says.
  char job_types[320] = {};
  int job_type_count = 0;

  // What this wake promises the server (`mm::WakeIntent`), and the band it came from.
  std::uint32_t next_check_in_s = 0;
  int max_transactions_this_wake = 0;
  const char* band = "";

  // The clock the library would stamp a heartbeat with, and where it came from.
  bool time_known = false;
  std::uint32_t epoch = 0;
  const char* time_source = "";

  // The heap, as the heartbeat reports it.
  std::uint32_t free_heap = 0;
  std::uint32_t min_free_heap = 0;

  // What the identity cost: requests made, and the ways it could not be read.
  std::uint32_t identity_reads = 0;
  std::uint32_t radio_reads = 0;
  std::uint32_t boot_changes = 0;
  std::uint32_t unreadable = 0;
  std::uint32_t no_identity = 0;
  std::uint32_t identity_lost = 0;  // of those, the ones that were ERR_STATE: a refusal

  // **The local door** (task p9-04): a job begun for the unit's own repeater is answered by this
  // end and nothing goes on the air (`walter_agent/link_mesh.h`). Until this line the four
  // counters were printed nowhere, and on 7 November a telemetry job that went on the air
  // because meshmaster.dk held the radio's old key could only be told from the server's audit
  // log (tasks/phase-8/FOLLOWUPS.md). `door_open` is `known` seen from the door's side: the key
  // the door tells its own repeater by.
  bool door_open = false;
  std::uint32_t local_jobs = 0;      // jobs begun for the unit's own repeater
  std::uint32_t local_requests = 0;  // CLI, LPP_GET and LOCAL_REQUEST requests made for one
  std::uint32_t local_silent = 0;    // local sends answered with silence on purpose
  std::uint32_t local_unserved = 0;  // local sends the link has no request for
  // Whether the session carries `LOCAL_REQUEST` (spec/LINK-PROTOCOL.md §5.9, link 1.2, task
  // p9-06): with it the door answers a status, an access list, a neighbour list and the owner
  // info out of the radio's own handler, and without it those four are what they were before
  // 1.2 - three *Failed* and one silence. It is read off the session's capability bits, which is
  // the same question the door asks at every send, so the line cannot drift from the behaviour.
  bool door_local_request = false;
};

// One row of `agent obs`: the fields of an advert the radio heard, **without its name**.
struct AgentObservationRow {
  char public_key[65] = {};
  std::uint32_t heard_at_epoch = 0;  // 0 when this unit has no time: `ago_s` then says it
  std::uint32_t ago_s = 0;
  std::uint8_t hops = 0;
  bool snr_known = false;
  int snr_q4 = 0;
  bool rssi_known = false;
  int rssi = 0;
  const char* role = "";  // "companion" | "repeater" | "roomserver" | "sensor" | "" (not said)
  bool has_position = false;
};

struct AgentObservationsView {
  bool ring = false;  // a ring exists in this build
  std::size_t count = 0;
  std::size_t capacity = 0;
  std::uint32_t heard = 0;
  std::uint32_t dropped = 0;       // the library's figure: the one the heartbeat carries
  std::uint32_t radio_dropped = 0; // §6.4's `obs_dropped`, as last read
  std::uint8_t remaining = 0;      // what the RAK said was still in its own ring
  std::uint32_t pulls = 0;
  std::uint32_t own_key = 0;
  std::uint32_t unreadable = 0;

  // The last five, newest first.
  static constexpr std::size_t kRows = 5;
  std::size_t rows = 0;
  AgentObservationRow row[kRows];
};

class AgentProbe {
 public:
  virtual ~AgentProbe() = default;

  // Who this unit says it is, reading the radio where it has to (one §5.6 and one §5.7 request
  // at most, and none when nothing has changed). Returns `out->known`.
  virtual bool identity(AgentIdentityView* out) = 0;

  // The JSON body of `POST /agent/v1/heartbeat` as it would be sent now: a pointer into the
  // implementation's own buffer - about a kilobyte and a half, which the console does not put on
  // its stack - with its length, or nullptr when it did not fit. **Makes no call**: it builds the
  // body and nothing else.
  virtual const char* heartbeat(std::size_t* length) = 0;

  // The ring, and the last five of it.
  virtual void observations(AgentObservationsView* out) = 0;

  // One §7 drain, now, for the bench: how many records went into the ring.
  virtual std::size_t pull() = 0;
};

}  // namespace console
}  // namespace core
