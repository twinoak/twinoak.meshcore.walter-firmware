// What the modem said in one call, on its receiving side (task p15-02).
//
// At nearly every wake of the unit in service the observations call fails: *nothing answered, the
// connection closed before the body was complete*. It had failed 1089 times by 16 November 2026,
// and on that day all eight entries of `agent calls` were that one call. What is known is that the
// request goes out whole - meshmaster.dk stores the batch and its log has a `200` in under a second
// for every post this firmware called lost - and that the call then lasts 30 seconds, which is the
// modem library's command timeout. What is **not** known is what the modem did in those thirty
// seconds: whether it announced the answer, which command went unanswered, when the close came.
// The library's own trace would say and does not fit a release image (`docs/bench.md` §13), and a
// deep sleep takes the console with it.
//
// So this is written down instead, small enough for a release image: for each thing that happens
// after the request has gone out, how long after and what.
//
//   a receive that returned bytes     and how many, and how long the command took - so that when
//                                     it was asked can be read against the rings (task p15-07)
//   a receive that said *not yet*     counted, not listed: the modem answers `+CME ERROR: 4` or
//                                     `+SQNSRECV: <id>,0` to every ask before the first byte is
//                                     there, and there are hundreds of them in a stalled call
//   a receive that failed             the library's own state, the modem's `+CME ERROR` number
//                                     when it answered one, and how long the command took
//   a ring                            with the length the modem announced, as the library hands
//                                     it over (`+SQNSRING: <id>,<n>`)
//   the close                         `+SQNSH: <id>`, when the library hands it over
//   the one more ask                  **read, never written, since task p15-05**: the firmware
//                                     of task p15-03 asked the modem a second time after a
//                                     receive that failed with the close seen and wrote down
//                                     what came of it. The ask is gone (it answered nothing in
//                                     35 tries on the unit) and nothing writes this moment any
//                                     more, but a record kept by that firmware is in the store
//                                     of every unit that ran it, so it is still decoded and
//                                     still printed.
//
// and, once for the call: the socket id, the bytes sent, and how it ended.
//
// **And, since task p15-07, what the answer said of itself** when one came back: the status,
// whether it was framed by a length or by chunks or by the close, how many bytes of body were
// read, and whether it said `Connection: close`. Those four are **not stored**: they are what a
// call that *worked* is read by, and a call that worked is not kept over a sleep (deliverable 1
// of that task: a report is made in the middle of a wake, straight after the calls that worked,
// so their record is still in memory). A record read back out of the store therefore says nothing
// about an answer, which is also what every record kept by a firmware before that task says.
//
// **No byte of a payload, a header, a token or a name is in it** (`spec/SYSTEM.md` S-12). Every
// field is a count, a duration or a code, and `describe()` prints nothing else; a host test reads
// the printed lines for a byte of the body and of the token and finds neither.
//
// -- where the events are taken, and where the record lives ------------------------------------
//
// The events are taken where they happen: in the modem component (`components/modem/walter_tls.cpp`
// - `socketEvent` for the ring and the close, `receive` for the three kinds of ask) and in the
// transport above it (`core/net/http_over_tls.h` - the request going out and how the call ended).
// **What a record holds, how it is packed and how it is printed is here**, with no modem in it, and
// is tested on the host; the host tests' own socket (`host_test/sim/fake_tls_socket.h`) drives the
// same calls, so a scripted call fills a record exactly as the board does.
//
// **Two tasks write it.** The ring and the close come from the task that delivers the modem's
// events, the rest from the task making the request - the same pair `core/net/socket_rotation.h`
// is written for. There is no lock: a moment is appended by taking an index with one
// `fetch_add`, which gives every writer six bytes of its own, and the two counts are atomic.
// The once-per-call fields are written by the requesting task alone.
//
// -- kept over a sleep -------------------------------------------------------------------------
//
// The record of the newest call that did not come back 2xx is written to **one value of the
// settings store**, as hex, under `kCallRecordKey` - the shape `agent_calls` and `agent_state` use
// and for the same reason: RTC memory has no slot left and this has to survive a power cut, which
// RTC memory does not. A store's value is 160 characters, so the whole record is 79 bytes and 158
// characters of hex, and a dozen moments is what fits with a count of the rest.
//
// It is written **at the moment the call failed**, which is also the moment `agent calls`'s list
// moved (`walter_agent/call_log.h` does both), and not at the end of the wake: there is one record
// in RAM, the next call overwrites it, and the alternative was a second 79-byte copy in static
// storage, which a board whose heap margin is counted in hundreds of bytes does not have
// (`docs/ARCHITECTURE.md` §14, rail R22). A call that succeeds writes nothing, so the kept record
// stays the failed one's.
//
// **One value and no more, again, since task p15-05.** Task p15-03 kept two totals of its one
// more ask in a second value beside this one; the ask is gone and so is that value, which
// `walter_agent/call_log.h` erases once from the store of a unit that ran it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "interfaces/http.h"
#include "interfaces/key_value.h"
#include "interfaces/text_out.h"

namespace core {
namespace net {

// One thing that happened on the receiving side. The numbers are stored, so they are not
// reordered: a value this build has no name for reads back as kNothing and is not printed.
enum class CallMoment : std::uint8_t {
  kNothing = 0,
  kData = 1,           // a receive returned bytes; `value` is how many, and the sixth byte is how
                       // long its command took (task p15-07)
  kReceiveFailed = 2,  // a receive failed; `value` is how long the command took, in ms
  kRing = 3,           // the library handed over a ring; `value` is the length the modem announced
  kClose = 4,          // the library handed over the close
  // The one more ask the firmware of task p15-03 made after a receive that failed with the close
  // seen: `state` is the `ReadAgainFound` it came back with and, when that is kBytes, `value` is
  // how many. **Nothing writes this any more** (task p15-05 took the ask out), and it keeps its
  // number for the records that firmware left in the stores of the units that ran it.
  kReadAgain = 5,
  kCount,
};

// What that one more ask found, as the `state` nibble of a kReadAgain moment holds it. The
// numbers were stored by the firmware of task p15-03, so they are not reordered and not reused:
// this build reads them and never writes one.
enum class ReadAgainFound : std::uint8_t {
  kNothing = 0,  // the modem answered and held nothing
  kBytes = 1,    // it still held the response
  kFailed = 2,   // the ask failed too, with the modem's `+CME ERROR` number when it answered one
  kCount,
};

// How many moments are kept. A dozen, as the task asks: the shape of a stalled call is a ring (or
// none), one failed receive and a close, and a call that is answered is a handful of receives -
// so a dozen holds the whole of either, and the ones past it are counted.
constexpr std::size_t kCallMoments = 12;

// One moment, packed, as it is stored:
//
//   0-1  at_ms  u16  milliseconds after the request went out, capped
//   2    what   u8   low nibble: CallMoment; high nibble: for kReceiveFailed, the modem library's
//                    own `WalterModemState` (0 ok, 1 error, 2 its command timeout, ...), capped
//                    at 15; for kReadAgain, the `ReadAgainFound` the one more ask came back with;
//                    0 for every other kind
//   3-4  value  u16  kData: the bytes; kRing: the length announced; kReceiveFailed: how long the
//                    command took, in ms, capped; kReadAgain: the bytes it came back with
//   5    cme    u8   kReceiveFailed, and kReadAgain that failed, and the modem answered
//                    `+CME ERROR: n`: **n + 1**, capped at 255 - the bias is because
//                    `+CME ERROR: 0` is a real code (equipment failure), so 0 has to mean *the
//                    modem answered no CME error at all*. For kData, since task p15-07, the same
//                    byte with the same bias holds **how long that receive's command took**, in
//                    milliseconds: `took + 1`, 255 meaning kCallTookAtMost or more - so that a
//                    0 there reads as *not written down*, which is what a record kept by a
//                    firmware before that task has in it, and not as *it came back at once*
constexpr std::size_t kCallMomentBytes = 6;

// The once-per-call bytes in front of them:
//
//   0    version u8
//   1    which call it was (low nibble) | how it ended (high nibble)
//   2    the socket id (low nibble) | the moments there was no room for, capped at 15 (high nibble)
//   3-4  the bytes sent, capped                                                              u16
//   5-6  the receives answered *not yet*, capped                                             u16
constexpr std::size_t kCallRecordHeadBytes = 7;

constexpr std::size_t kCallRecordBytes = kCallRecordHeadBytes + kCallMoments * kCallMomentBytes;
constexpr std::size_t kCallRecordHexBytes = 2 * kCallRecordBytes + 1;
constexpr std::uint8_t kCallRecordVersion = 1;

// The key of its own the task asks for, beside `agent_calls` (phase 13's follow-up, *a second key,
// not a longer entry*). Fifteen characters is NVS's limit and this is thirteen.
constexpr const char kCallRecordKey[] = "agent_call_rx";

static_assert(kCallRecordHexBytes - 1 <= interfaces::kKeyValueMax,
              "the record of a call's receiving side has to fit one value of the store");

// The library's own state for a command that got no answer at all: `WALTER_MODEM_STATE_TIMEOUT`
// of `WalterModemState` (`WalterModem.h` at walter-modem 1.5.0, the third of the enum), which is
// what the command task writes when it gives up after CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS - the
// 30 seconds the stalled call lasts. It is the one value of that enum this component names, so
// that the line a person reads says *nothing answered* instead of a bare number.
constexpr std::uint8_t kCallStateNoAnswer = 2;

// The largest each capped field can say. A figure at the cap is printed with a `+` so that nobody
// reads it as exact.
constexpr std::uint16_t kCallMomentAtMost = 0xFFFFu;
constexpr std::uint16_t kCallBytesAtMost = 0xFFFFu;
constexpr std::uint16_t kCallNotYetAtMost = 0xFFFFu;
constexpr std::uint8_t kCallDroppedAtMost = 15;
// How long a receive that returned bytes can say its command took, in milliseconds: one byte with
// the plus-one bias, so 254 and not 255. A successful `AT+SQNSRECV` is a UART round trip and tens
// of milliseconds; the one that takes thirty seconds is the one that fails, and that one's
// duration is a whole u16 (`value`).
constexpr std::uint8_t kCallTookAtMost = 254;

// One moment, read back out of the six bytes it is held in.
struct CallStep {
  CallMoment what = CallMoment::kNothing;
  std::uint16_t at_ms = 0;
  std::uint16_t value = 0;
  std::uint8_t state = 0;  // kReceiveFailed: the library's own state for the command
  std::uint16_t cme = 0;   // kReceiveFailed: the `+CME ERROR` number the modem answered
  bool had_cme = false;    // ... and whether it answered one at all
  // How long this receive's command took (task p15-07), so that **when it was asked** can be read
  // against the rings: `at_ms - took_ms`. kReceiveFailed has always carried it in `value`; kData
  // carries it in the sixth byte, and `took_known` is false for a kData moment of a record kept by
  // a firmware that did not write it. `took_or_more` is the figure sitting on its cap.
  std::uint16_t took_ms = 0;
  bool took_known = false;
  bool took_or_more = false;
};

class CallRecord {
 public:
  // -- as the call runs ----------------------------------------------------------------------

  // A new call: everything forgotten, and this record is about that call whatever becomes of it -
  // a call that never got its request out holds no moment and still says how it ended, so that
  // the kept record is always the newest failed call's and never an older one's. Nothing is
  // recorded until the request has gone out, which is what wentOut() says.
  void begin();

  // The socket id this connection took (`core/net/socket_rotation.h`). Before the request goes
  // out, and kept whatever happens after.
  void onSocket(int id);

  // The request - head and body - is on the wire. This is the moment every `at_ms` is counted
  // from, and the moment recording begins.
  void wentOut(std::uint64_t now_ms, std::size_t bytes);

  // A receive that returned `bytes` bytes, whose command took `took_ms` (task p15-07: the ask and
  // the answer are both wanted, so that a read can be placed against the rings).
  void gotData(std::uint64_t now_ms, std::size_t bytes, std::uint32_t took_ms);

  // A receive answered *nothing buffered yet*: counted, not listed.
  void notYet();

  // A receive that failed: how long the command took, the library's own state for it, and the
  // `+CME ERROR` number the modem answered - `cme` below zero when it answered none, which is
  // what the library's command timeout looks like.
  void receiveFailed(std::uint64_t now_ms, std::uint32_t took_ms, std::uint8_t state, int cme);

  // The library handed over a ring, announcing `announced` bytes.
  void rang(std::uint64_t now_ms, std::uint32_t announced);

  // The library handed over the close.
  void wasClosed(std::uint64_t now_ms);

  // What the answer said of itself (task p15-07), straight off the response the transport is about
  // to hand back: the status, the framing, the bytes of body and whether it said `Connection:
  // close`. **Not stored** and not printed when `response.status` is 0 - nothing answered, so
  // there is nothing for it to have said. Before ended(), which is where the transport calls it.
  void answerWas(const interfaces::HttpResponse& response);

  // The call is over and this is how it ended. Recording stops here: the close this end asks for
  // afterwards is not the other end's, and is not in the record.
  void ended(interfaces::HttpOutcome outcome);

  // Whether a moment would be recorded now: between wentOut() and ended().
  bool recording() const { return recording_.load(std::memory_order_acquire); }

  // -- what is in it -------------------------------------------------------------------------

  // Whether it holds a call at all: false before the first begin(), and after a load() that
  // found nothing.
  bool held() const { return held_; }

  // Which call it was, in the caller's own numbering (`walter_agent/call_log.h`'s `CallKind`).
  // Opaque here: this component stores the number and never reads it, and `describe()` is given
  // the caller's word for it.
  std::uint8_t callWas() const;
  void callWas(std::uint8_t what);

  int socketId() const;
  std::uint16_t bytesSent() const;
  std::uint16_t notYets() const;
  std::uint8_t dropped() const;
  interfaces::HttpOutcome outcome() const;

  std::size_t steps() const;
  CallStep step(std::size_t index) const;

  // What the answer said of itself, when one came back (task p15-07). `answerKnown()` is false
  // for a call nothing answered and for every record read back out of the store.
  bool answerKnown() const { return answer_known_; }
  std::uint16_t answerStatus() const { return answer_status_; }
  bool answerHadLength() const { return answer_has_length_; }
  bool answerWasChunked() const { return answer_chunked_; }
  bool answerSaidClose() const { return answer_close_; }
  std::uint32_t answerBytes() const { return answer_bytes_; }

  // -- the one that worked, kept in RAM and nowhere else (task p15-07) ------------------------

  // This record made a copy of `other`, whatever `other` goes on to do: how **the latest call of
  // this boot that was answered** is kept, so that `agent calls` and the report can lay it beside
  // the failed one a sleep did keep. There is one live record and the next call overwrites it, so
  // a call that worked has to be copied out of it or lost; this copy is never written to the
  // store, which is deliverable 1's *nothing more is kept in flash*. Not recording afterwards -
  // a copy is a finished call - and `other`'s own wiring below is not copied: a copy is not where
  // anything is copied to.
  void copyFrom(const CallRecord& other);

  // Where a call this record held and the server answered is copied to, and the record that is
  // (nullptr for a build that keeps none). `walter_agent/call_log.h` is what asks for the copy
  // and would be the natural place for this pointer; it is **here** because this object is taken
  // from the heap in `app_main` and that one is static storage the main task's stack has to be
  // found around afterwards. Four bytes of `.bss` there cost eight after alignment, and the
  // tightest big allocation of this image has about nine hundred to spare in all
  // (`docs/ARCHITECTURE.md` §14, rail R22; the measurement is in `tasks/phase-15/reports/07.md`).
  // Four bytes of heap here cost nothing anybody is counting.
  void copyAnsweredTo(CallRecord* other) { answered_ = other; }
  CallRecord* answeredCopy() const { return answered_; }

  // Nothing held: this record says *no call*. `begin()`'s own starting point, in public, for the
  // one in RAM that no store ever erases.
  void drop() { clear(); }

  // -- kept over a sleep ---------------------------------------------------------------------

  // This record under kCallRecordKey, as hex. `what` is the caller's number for the call. True
  // when it went in; false when there is no store, or nothing to keep.
  bool keep(std::uint8_t what, interfaces::KeyValue* store);

  // The kept record back out of the store, into this object. False - and this object left
  // holding nothing - when there is none, or it is of another version or shape.
  bool load(interfaces::KeyValue* store);

  // The key erased: the bench, and a test.
  static bool forget(interfaces::KeyValue* store);

  // The record in words, each line ended with '\n', or one line saying there is none. `what` is
  // the caller's word for which call it was; nullptr for *a call*. `lead` is what goes after
  // *what the modem said* on the first line, so that the two blocks `agent calls` prints are told
  // apart by their own first words (task p15-07). Every line is well inside the console's
  // 200 bytes (task p9-04).
  void describe(interfaces::TextOut& out, const char* what, const char* lead = "in it") const;

 private:
  // Everything forgotten and nothing held: what begin() and load() both start from.
  void clear();
  // The index this writer's six bytes are at, or past the end when there was no room.
  std::size_t take();
  void put(std::size_t index, CallMoment what, std::uint64_t now_ms, std::uint16_t value,
           std::uint8_t state, int cme);
  std::uint16_t since(std::uint64_t now_ms) const;
  bool decode(const char* hex);

  // The record in the form it is written in, version byte and all: a keep() is the hex of this
  // and a load() is the hex into it, and nothing is held twice. Every byte of this object is
  // static storage in the firmware, and the main task's stack has to be found around it
  // afterwards (`docs/ARCHITECTURE.md` §14, rail R22).
  std::uint8_t bytes_[kCallRecordBytes] = {};
  // How many moments have been offered, which is how the index of each is taken. Written by the
  // task making the request and by the one delivering the modem's events.
  std::atomic<std::uint16_t> offered_{0};
  std::atomic<std::uint16_t> not_yet_{0};
  std::atomic<bool> recording_{false};
  std::uint64_t origin_ms_ = 0;
  bool held_ = false;
  // What the answer said of itself (task p15-07). **Outside `bytes_`, so outside the stored
  // value**: 158 characters of hex is what one value of the settings store holds, and these are
  // read off a call that worked - which is not kept over a sleep at all. `answer_known_` false is
  // a call nothing answered, and every record a load() brought back.
  std::uint32_t answer_bytes_ = 0;
  std::uint16_t answer_status_ = 0;
  bool answer_known_ = false;
  bool answer_has_length_ = false;
  bool answer_chunked_ = false;
  bool answer_close_ = false;
  // Where a call that was answered is copied to. Wiring, not content: clear(), begin() and
  // copyFrom() all leave it alone, and a copy never carries the pointer of the record it came
  // from - a copy is not a place anything is copied to.
  CallRecord* answered_ = nullptr;
};

}  // namespace net
}  // namespace core
