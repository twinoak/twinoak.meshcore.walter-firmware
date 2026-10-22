#include "walter_agent/mesh_probe.h"

#include <agent/ReplyCodec.h>

namespace agent {

using core::console::MeshProbeResult;

bool LinkMeshProbe::status(const char* keyHex, std::uint32_t timeout_ms, MeshProbeResult* out) {
  if (out == nullptr) return false;
  *out = MeshProbeResult{};

  // §6.4 before, so that the one figure the bench reads is a difference and not a number.
  if (mesh_.refreshCounters()) {
    out->counters_known = true;
    out->client_tx_before = mesh_.counters().tx;
  }

  // No path: the server's own rule for a repeater it has no route to is to flood (IMesh), and a
  // person at the console has no route to type. The reply teaches one, which `rak mesh` prints.
  if (!mesh_.beginJob(keyHex, nullptr, 0, 0)) {
    out->outcome = MeshProbeResult::Outcome::kBadKey;
    return false;
  }
  if (!mesh_.jobBegun()) {
    out->outcome = MeshProbeResult::Outcome::kNotBegun;
    out->fault = mesh_.lastFault();
    out->status = mesh_.lastStatus();
    mesh_.endJob();
    return false;
  }

  std::uint32_t tag = 0;
  const mm::SendResult sent = mesh_.sendRequest(mm::kReqTypeGetStatus, nullptr, 0, tag);
  if (sent == mm::SendResult::Failed) {
    out->outcome = MeshProbeResult::Outcome::kNotSent;
    out->fault = mesh_.lastFault();
    out->status = mesh_.lastStatus();
    mesh_.endJob();
    return false;
  }
  out->flooded = sent == mm::SendResult::Flood;
  out->tag = tag;

  const bool answered = mesh_.awaitReply(tag, timeout_ms, nullptr);
  const LastRecord& record = mesh_.lastRecord();
  out->state = record.state;
  out->via_path = record.via_path;
  out->snr_q4 = record.snr_q4;
  out->rssi = record.rssi;
  out->elapsed_ms = record.elapsed_ms;
  if (!answered) {
    out->outcome = MeshProbeResult::Outcome::kNoReply;
    out->fault = mesh_.lastFault();
    out->status = mesh_.lastStatus();
    mesh_.endJob();
    if (out->counters_known && mesh_.lastFault() == core::link::client::Fault::kNone) {
      out->client_tx_after = mesh_.counters().tx;
    } else {
      out->counters_known = false;
    }
    return false;
  }

  const mm::MeshReply& reply = mesh_.reply();
  out->reply_len = reply.len;
  mm::StatusReading reading;
  if (reply.kind != mm::MeshReply::Kind::Response || !mm::parseRepeaterStats(reply.data, reply.len, reading)) {
    out->outcome = MeshProbeResult::Outcome::kUnreadable;
  } else {
    out->outcome = MeshProbeResult::Outcome::kOk;
    out->battery_mv = reading.batteryMv;
    out->uptime_s = reading.uptimeS;
    out->noise_floor_dbm = reading.noiseFloorDbm;
    out->last_rssi = reading.lastRssi;
    // The firmware stores the SNR x 4 and the library divides; multiplied back, it is the whole
    // number the air carried.
    out->last_snr_q4 = static_cast<int>(reading.lastSnr * 4.0 + (reading.lastSnr < 0 ? -0.5 : 0.5));
    out->packets_recv = reading.packetsRecv;
    out->packets_sent = reading.packetsSent;
    out->airtime_s = reading.airtimeS;
    out->tx_queue_len = reading.txQueueLen;
  }
  // endJob() is JOB_END and then MESH_COUNTERS: the figure after.
  mesh_.endJob();
  if (out->counters_known && mesh_.lastFault() == core::link::client::Fault::kNone) {
    out->client_tx_after = mesh_.counters().tx;
  } else {
    out->counters_known = false;
  }
  return out->outcome == MeshProbeResult::Outcome::kOk;
}

}  // namespace agent
