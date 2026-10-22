// The console's one seam into the mesh client (task p4-02): `rak mesh status <key>` - begin a job
// with a neighbour, send one status request, wait for the reply, print it - for the bench
// (docs/bench.md, *The agent*). The mesh client itself is components/agent/'s LinkMesh over the
// link, and core/ cannot name it (agent/ is built on core/, not the other way round), so the
// console takes this interface and main/ hands it the implementation
// (components/agent/mesh_probe.h). A build without one answers in one line.
//
// What crosses it is numbers: the outcome, the link's own fields of the reply record (§6.3), the
// repeater's stats as the library's codec read them, and `client_tx` before and after - the one
// figure the bench item is about (one packet on the air, one reply, `client_tx` up by one).
// Nothing the mesh said as text crosses it (S-12).
#pragma once

#include <cstdint>

#include "core/link/client/client.h"

namespace core {
namespace console {

struct MeshProbeResult {
  enum class Outcome : std::uint8_t {
    kBadKey,      // not 64 hex characters: nothing was sent
    kNotBegun,    // JOB_BEGIN was not answered OK: `fault` and `status` say why
    kNotSent,     // the send was refused or nothing went on the air: `fault` and `status`
    kNoReply,     // sent, and nothing came back inside the wait: `state` is the record's
    kUnreadable,  // a reply came that is not a status reply this firmware reads
    kOk,
  };
  Outcome outcome = Outcome::kBadKey;
  link::client::Fault fault = link::client::Fault::kNone;
  std::uint8_t status = link::status::kOk;

  // The send and the reply record (§6.2, §6.3).
  bool flooded = false;
  std::uint32_t tag = 0;
  std::uint8_t state = link::reply_state::kNone;
  bool via_path = false;
  std::int8_t snr_q4 = 0;
  std::int16_t rssi = link::kRssiUnknown;
  std::uint32_t elapsed_ms = 0;
  std::uint8_t reply_len = 0;  // what came, in bytes (kUnreadable says how much)

  // §6.4's client_tx before the job and after it, when MESH_COUNTERS answered both times.
  bool counters_known = false;
  std::uint32_t client_tx_before = 0;
  std::uint32_t client_tx_after = 0;

  // The repeater's stats (RepeaterStats of examples/simple_repeater/MyMesh.h at d9296435, as the
  // library reads them), when kOk. Whole numbers; the SNR in quarter-decibels as the air carries it.
  int battery_mv = 0;
  std::uint32_t uptime_s = 0;
  int noise_floor_dbm = 0;
  int last_rssi = 0;
  int last_snr_q4 = 0;
  std::uint32_t packets_recv = 0;
  std::uint32_t packets_sent = 0;
  std::uint32_t airtime_s = 0;
  int tx_queue_len = 0;
};

class MeshProbe {
 public:
  virtual ~MeshProbe() = default;
  // One status request to `keyHex` (64 hex characters), the reply awaited `timeout_ms`, the job
  // ended whatever happened. Returns true when `out->outcome` is kOk.
  virtual bool status(const char* keyHex, std::uint32_t timeout_ms, MeshProbeResult* out) = 0;
};

}  // namespace console
}  // namespace core
