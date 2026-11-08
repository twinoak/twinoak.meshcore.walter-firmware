// The RAK's side of spec/LINK-PROTOCOL.md §6 as a simulator (task p4-02): a radio on the other end
// of the Walter's UART that lends its identity to the mesh client - one peer, one exchange, the
// five sends, the reply record and its event, the counters - with a *repeater* behind it that
// answers what the peer answers. The repeater is any `mm::IMesh`; in the tests it is the agent
// library's own FakeMesh (host_test/agent-core/test/support/FakeMesh.h), the simulated stock
// repeater, so that what the runner's scenarios see through the framing is what they saw in the
// same process.
//
// What it is built on: sim/scripted_radio.h, the radio of task p1-02 - the frames (phase 1's
// codec), §3's one remembered response (a retry of a SEND_* transmits nothing), §5's HELLO, PING,
// STATUS and the clock out of the fixtures, §8's attention, §9's scanner, and every knob of it
// (silence, a restart with a new boot_id, a capability taken away). This class answers the
// requests of §6, §5.7's CLI and §5.8's LPP_GET through that radio's `on_request` hook and sends
// §6.3's event through `sendEventPayload`; everything else is the scripted radio's.
//
// The rules of §6 it keeps, because the Walter's behaviour depends on them:
//
//   - one peer at a time, one exchange at a time: a send without a job is ERR_STATE (a trace
//     needs none), a send while a reply is awaited is ERR_BUSY;
//   - one packet per send: `sent` has one entry per packet the repeater was asked for, and
//     `client_tx()` is its size - what MESH_COUNTERS answers and what the runner's transaction
//     count is held against. A retry of the same request is §3's remembered response;
//   - the reply: awaited from the send until the repeater answers or `timeout_ms` is up, reported
//     exactly once as EV_MESH_REPLY (unless a test loses it), kept in the record until the next
//     send, JOB_BEGIN or JOB_END, and read with REPLY_GET;
//   - a returned path is learned: PATH_GET reads the repeater's path, which a flooded reply
//     changes;
//   - the job ends itself after 120 s of link silence;
//   - a password is in RAM for the length of the send: the one buffer here is wiped as soon as
//     the repeater has been handed the login, and a sensitive CLI line the same.
//
// §5.9's LOCAL_REQUEST is modelled from link 1.2 (task p9-06): the repeater's own request handler
// behind `local_door`, answering status, the access list, neighbours and owner info with the bytes
// a mesh admin would have been answered - nothing on the air, no job needed, `client_tx` unmoved.
//
// §7's OBS_PULL is modelled too (task p4-04): a ring of adverts a test pushes, handed out oldest
// first, at most five to a payload and at most `max`, removed from the ring as they go, with
// `remaining` and the per-pull `dropped` the specification asks for. The fixtures' own two-advert
// case answers a radio that is not this one.
//
// What it does not model: airtime and two-byte flood hashes (the RAK's own packet building, which
// the link does not show).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <agent/IMesh.h>

#include "core/link/codec/messages.h"
#include "core/link/codec/packet.h"
#include "core/link/codec/wire.h"
#include "fakes/fake_clock.h"
#include "interfaces/byte_stream.h"
#include "sim/scripted_radio.h"

namespace sim {

class SimRak final : public interfaces::ByteStream {
 private:
  // A radio of this object's own, which `radio` below is unless a bench lends one.
  ScriptedRadio own_radio_;

 public:
  SimRak(fakes::FakeClock& clock, mm::IMesh& repeater);
  // Over a radio a bench already has (task p4-05: the wake bench's, whose knobs every test of a
  // wake turns): the same RAK, answering §6 through that radio's `on_request`.
  SimRak(ScriptedRadio& existing, fakes::FakeClock& clock, mm::IMesh& repeater);

  // The link's end, with every knob of task p1-02's radio: `radio.silence`, `radio.deaf`,
  // `radio.caps`, `radio.boot_id`, `radio.requests`, `radio.executions`, ...
  ScriptedRadio& radio;

  // ---------------------------------------------------------------- the stream, the Walter's end
  bool open() override { return radio.open(); }
  void close() override { radio.close(); }
  std::size_t read(std::uint8_t* out, std::size_t max) override;
  std::size_t write(const std::uint8_t* data, std::size_t size) override;
  bool flush(std::uint32_t timeout_ms) override { return radio.flush(timeout_ms); }
  bool idleFor(std::uint32_t ms) override { return radio.idleFor(ms); }

  // ---------------------------------------------------------------- the knobs
  std::uint32_t job_idle_end_ms = core::link::kJobIdleEndMs;  // §6.1: 120 s
  int lose_events = 0;           // the next n EV_MESH_REPLY are not sent: the Walter must REPLY_GET
  bool lose_all_events = false;
  std::uint32_t event_delay_ms = 0;  // how long after the outcome the event is on the wire
  // §6.4: the radio's own `tx` counts forwards, so it is client_tx plus this - a Walter that
  // reported `tx` as its own transactions would be caught by any assertion on the difference.
  std::uint32_t forwards = 1000;
  std::uint32_t obs_dropped = 0;
  std::int16_t noise_floor = -112;
  std::int16_t last_rssi = -97;
  std::int8_t last_snr_q4 = 25;
  // What the record says a reply was heard at (§6.3): one figure each, the repeater has none.
  std::int8_t reply_snr_q4 = 25;
  std::int16_t reply_rssi = -97;

  // §5.7, the unit's own console, for the mesh client's local door: a line in, a reply out.
  // Returning false is a line that restarts the node and never answers (`reboot`): no response
  // at all, which the Walter reads as the expected silence. Unset, every line is
  // `Unknown command` (CommonCLI::handleCommand at d9296435).
  std::function<bool(const std::string& line, std::string* reply)> console;
  // §7, one advert this radio heard: what a test pushes into its ring and what OBS_PULL hands
  // out. `age_s` is measured by the radio, so it is right whatever either clock said.
  struct Advert {
    std::vector<std::uint8_t> pubkey;  // 32 bytes
    std::uint32_t adv_ts = 0;
    std::uint32_t age_s = 0;
    std::int8_t snr_q4 = 0;
    std::int16_t rssi = core::link::kRssiUnknown;
    std::uint8_t path_len = 0;  // bits 7-6 the hash size minus one, bits 5-0 the hop count
    std::vector<std::uint8_t> path;
    std::vector<std::uint8_t> app_data;  // MeshCore's advert payload, 0 to 32 bytes
  };
  // The radio's own ring, oldest first. A test pushes; a pull takes from the front.
  std::deque<Advert> observations;
  // §7's `dropped`: lost since the last OBS_PULL. Cleared by a pull, as the specification says.
  std::uint16_t obs_dropped_since_pull = 0;

  // §5.8, LPP_GET's answer: the fixtures' eight bytes (link-v1.json, `lpp response`) unless a
  // test sets the companion's telemetry bytes.
  std::vector<std::uint8_t> lpp = {0x01, 0x74, 0x01, 0x90, 0x01, 0x67, 0x00, 0x28};

  // §5.9 (link 1.2, task p9-06), LOCAL_REQUEST: the node's **own request handler**, reached over
  // this link with nothing on the air. `req_type` and `data` are where the datagram's plaintext
  // holds them, and the answer is what the handler would have replied after its four tag bytes.
  // The mesh client is not involved at all: a job in progress is untouched, no packet is built
  // and `client_tx()` does not move, which is the whole point of the request.
  //
  // Unset is a radio whose role has no handler - ERR_UNSUPPORTED, which is also what a radio
  // without the capability bit answers (§5.9's last line). Returning **false** is a request the
  // handler gives no answer to; §5.9 makes that ERR_RANGE, where over the mesh it would have
  // been silence.
  std::function<bool(std::uint8_t req_type, const std::uint8_t* data, std::size_t len,
                     std::vector<std::uint8_t>* answer)>
      local_door;
  struct LocalAsked {
    std::uint8_t req_type = 0;
    std::vector<std::uint8_t> data;
  };
  std::vector<LocalAsked> local_asked;  // every LOCAL_REQUEST executed, in order (§3: no retries)
  std::uint32_t local_refused = 0;      // of those, answered with a status other than OK

  // ---------------------------------------------------------------- what it did
  struct Sent {
    std::uint8_t type = 0;  // the request type (type::kSendLogin ...)
    bool flood = false;
    std::uint32_t timeout_ms = 0;
    std::uint32_t tag = 0;
    std::uint64_t at_ms = 0;
  };
  std::vector<Sent> sent;  // one per packet put on the air: §6.4's client_tx
  std::uint32_t client_tx() const { return static_cast<std::uint32_t>(sent.size()); }
  std::vector<std::string> cli_lines;      // §5.7 lines, in order (never a password: the test's own)
  std::vector<std::uint8_t> lpp_masks;     // §5.8's perm_mask, per request
  std::uint32_t jobs_begun = 0;
  std::uint32_t jobs_ended = 0;
  std::uint32_t jobs_timed_out = 0;  // §6.1's 120 s
  std::uint32_t events_sent = 0;
  std::uint32_t events_lost = 0;
  std::uint32_t refused_busy = 0;    // ERR_BUSY
  std::uint32_t refused_state = 0;   // ERR_STATE
  std::uint32_t refused_other = 0;   // ERR_RANGE, ERR_MALFORMED, ERR_FAILED
  std::uint32_t reply_gets = 0;
  std::uint32_t counter_reads = 0;
  std::uint32_t path_reads = 0;
  std::uint32_t obs_pulls = 0;
  std::uint32_t obs_handed_out = 0;

  bool jobBegun() const { return job_; }
  bool awaiting() const { return await_.kind != Await::kNone; }
  // True while this radio is driving the repeater - answering a request, or polling it for the
  // awaited reply. A harness that stands between a runner and the same repeater object (the
  // shim of host_test/agent-link/) reads it to tell the repeater's own calls from the runner's.
  bool busy() const { return busy_; }
  std::uint8_t recordState() const { return record_.state; }
  // The buffers a password and a sensitive line pass through here, for a test that reads them
  // back: both all zeros after the send.
  const std::uint8_t* passwordBuffer() const { return password_; }
  std::size_t passwordBufferSize() const { return sizeof password_; }
  const char* cliBuffer() const { return cli_; }
  std::size_t cliBufferSize() const { return sizeof cli_; }

  // §5.1: a new boot_id, and everything of §6 forgotten - the job, the path, the awaited reply.
  void restart(bool announce = true);

 private:
  enum class Await : std::uint8_t { kNone, kResponse, kText, kTrace };

  struct Awaiting {
    Await kind = Await::kNone;
    std::uint32_t tag = 0;
    std::uint32_t timeout_ms = 0;
    std::uint64_t sent_at_ms = 0;
    bool flood = false;
  };

  // §6.3's record, kept as the fields and encoded when asked for.
  struct Record {
    std::uint8_t state = core::link::reply_state::kNone;
    std::uint32_t tag = 0;
    bool via_path = false;
    std::uint32_t elapsed_ms = 0;
    bool flood = false;
    mm::MeshReply reply;  // kind None for states 0-2
  };

  bool onRequest(const core::link::Packet& packet, std::vector<std::uint8_t>* payload);
  void tick();  // before every access: the job's idle end, the awaited reply
  void finishWait(bool answered);
  std::vector<std::uint8_t> encodeRecord() const;
  std::vector<std::uint8_t> encodePath() const;
  bool decodePath(const std::uint8_t* at, std::size_t size, std::uint8_t* mode, std::uint8_t* hash_size,
                  std::uint8_t* hop_count, std::vector<std::uint8_t>* hashes) const;
  std::vector<std::uint8_t> send(const core::link::Packet& packet);
  void forgetJob();
  std::uint32_t uniqueTime();

  fakes::FakeClock& clock_;
  mm::IMesh& repeater_;

  bool job_ = false;
  std::string peer_hex_;
  std::uint64_t last_request_ms_ = 0;
  Awaiting await_;
  Record record_;
  std::uint32_t last_unique_ = 0;

  std::uint8_t password_[core::link::kMaxPassword + 1] = {};
  char cli_[core::link::kMaxCliLine + 1] = {};
  bool busy_ = false;
};

}  // namespace sim
