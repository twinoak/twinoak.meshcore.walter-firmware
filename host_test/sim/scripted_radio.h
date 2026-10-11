// The radio on the other end of the Walter's UART (task p1-02): an interfaces::ByteStream the
// client writes frames into, with a radio behind it that answers out of
// spec/fixtures/link/link-v1.json - the same file the radio firmware's own codec is tested
// against. Later phases reuse it: the supervisor's silent radio (phase 2) and the update's health
// check are this object with other knobs turned.
//
// What it is not: a second implementation of the protocol. It parses what the Walter sent with the
// Walter's own codec and answers with the *fixtures'* bytes, so the thing under test is the
// client's behaviour - one request at a time, retries, events, the session - and not whether two
// codecs agree. That the codec agrees with the specification is task p1-01's test.
//
// The radio's rules it does keep, because the client's behaviour depends on them:
//
//   - it answers a request at once (or after answer_delay_ms, or not at all);
//   - §3's remembered response: a request with the (type, seq) of the last one it executed is not
//     executed again - the remembered response is sent again. `executions` counts the difference;
//   - §8's attention: 0x00 0x00, 300 ms, the event, then every 1000 ms until a request arrives or
//     it has sent it ten times;
//   - §9's other firmware: with `scanner` on it answers text commands with NDJSON lines and
//     ignores frames entirely.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/link/codec/frame.h"
#include "core/link/codec/packet.h"
#include "core/link/codec/wire.h"
#include "fakes/fake_clock.h"
#include "fixtures/json.h"
#include "interfaces/byte_stream.h"

namespace sim {

class ScriptedRadio final : public interfaces::ByteStream {
 public:
  explicit ScriptedRadio(fakes::FakeClock& clock);

  // ---------------------------------------------------------------- the stream, the Walter's end
  bool open() override;
  void close() override;
  std::size_t read(std::uint8_t* out, std::size_t max) override;
  std::size_t write(const std::uint8_t* data, std::size_t size) override;
  bool flush(std::uint32_t timeout_ms) override;
  bool idleFor(std::uint32_t ms) override;

  // ---------------------------------------------------------------- the knobs
  int silence = 0;                    // ignore this many more requests, whole
  bool deaf = false;                  // ignore every request (a radio whose firmware is gone)
  std::uint32_t answer_delay_ms = 0;  // how long after a request its answer appears
  int wrong_seq = 0;                  // this many more answers carry the wrong seq (§3)
  int corrupt = 0;                    // this many more answers have a byte flipped: the CRC fails
  bool scanner = false;               // §9: NoiseScope, lines instead of frames
  bool refuse_open = false;           // the peripheral will not start

  // What HELLO and EV_BOOT say about it (§5.1). The fixture's own body is used and these three
  // fields are patched into it, so a test can take a capability away or make it restart.
  std::uint32_t caps = core::link::cap::kCli | core::link::cap::kPower | core::link::cap::kEnv |
                       core::link::cap::kMeshClient | core::link::cap::kObservations |
                       core::link::cap::kAttention;
  std::uint8_t role = core::link::role::kRepeater;
  std::uint32_t boot_id = 0x1a2b3c4d;
  // What it calls its firmware and its board in the HELLO body, when a test wants something other
  // than the fixture's own strings: a version long enough to be truncated here, and - phase 2's
  // health check - the version an image was supposed to have been flashed with.
  std::string fw_version;
  std::string board;

  // The event seq the next event carries (§3: the radio counts from 0 and wraps). Public so that a
  // test can skip one and make a gap.
  std::uint8_t event_seq = 0;

  // ---------------------------------------------------------------- what it does
  // A new boot_id, the event counter back to 0, nothing remembered - and an EV_BOOT on the wire
  // (§5.1), unless `announce` is false (a radio that was reset while the Walter slept).
  void restart(bool announce = true);

  // §8. 0x00 0x00, then the event 300 ms later, then every 1000 ms, `repeat` counting from 0,
  // until a request arrives or it has been sent `repeats` times.
  void raiseAttention(std::uint8_t reason, std::uint16_t detail, int repeats = 1);

  // Any event case of the fixtures, by name, with this radio's event seq, after a delay.
  void sendEvent(const std::string& case_name, std::uint32_t after_ms = 0);

  // One line of text, as the scanner speaks (§9).
  void sendLine(const std::string& line, std::uint32_t after_ms = 0);

  // The next response is this fixture case (by name), whatever the request was.
  void answerWith(const std::string& case_name);
  // The next response is this status and nothing else (§3).
  void refuseNext(std::uint8_t status);

  // Raw bytes on the wire, after a delay: an attention's leading zeros, noise, half a frame.
  void sendBytes(const std::vector<std::uint8_t>& bytes, std::uint32_t after_ms = 0);

  // ---------------------------------------------------------------- what it saw
  struct Seen {
    std::uint8_t type = 0;
    std::uint8_t seq = 0;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> frame;  // the frame's own bytes, without the sender's idle 0x00
    bool executed = false;            // false: the remembered response was sent again (§3)
  };

  std::vector<std::vector<std::uint8_t>> writes;  // every write() the Walter made, verbatim
  std::vector<Seen> requests;                     // every frame it decoded, retries included
  std::vector<std::string> commands;              // the text commands it was given (§9)
  int executions = 0;
  int opens = 0;
  int closes = 0;
  bool is_open = false;
  std::uint32_t dropped = 0;  // frames its own receiver refused

  // The response payload it last put on the wire, for a test that wants to hold the client's
  // decoding against the bytes.
  std::vector<std::uint8_t> last_response;
  std::string last_response_case;

 private:
  struct Pending {
    std::uint64_t due_ms = 0;
    std::vector<std::uint8_t> bytes;
    bool attention = false;  // dropped when a request arrives (§8)
  };

  void onFrame(const core::link::Packet& packet, const std::vector<std::uint8_t>& frame);
  void onCommand(const std::string& line);
  std::vector<std::uint8_t> responseFor(const core::link::Packet& packet, std::string* case_name);
  std::vector<std::uint8_t> helloBody(const std::vector<std::uint8_t>& from, std::size_t at);
  void queue(const std::vector<std::uint8_t>& bytes, std::uint32_t after_ms, bool attention = false);
  void answer(std::uint8_t type, std::uint8_t seq, const std::vector<std::uint8_t>& payload);
  void ripen();  // move what is due into the readable buffer

  fakes::FakeClock& clock_;
  core::link::FrameReceiver receiver_;
  std::deque<Pending> pending_;
  std::vector<std::uint8_t> readable_;
  std::vector<std::uint8_t> collected_;  // the frame being collected, for `Seen::frame`
  std::string line_;                     // the text command being collected (§9)
  std::uint64_t last_activity_ms_ = 0;

  // §3's one entry: the (type, seq) of the last request executed and the response it got.
  bool remembered_ = false;
  std::uint8_t remembered_type_ = 0;
  std::uint8_t remembered_seq_ = 0;
  std::vector<std::uint8_t> remembered_payload_;

  bool answer_with_ = false;
  std::string answer_case_;
  bool refuse_ = false;
  std::uint8_t refuse_status_ = 0;
};

}  // namespace sim
