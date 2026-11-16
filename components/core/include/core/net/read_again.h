// One more ask before a call is given up (task p15-03).
//
// At nearly every wake of the unit in service the observations call fails. The request goes out
// whole, meshmaster.dk stores the batch and answers `200` in under a second, and this firmware
// never reads that answer: one `AT+SQNSRECV` goes unanswered for the modem library's whole
// 30-second command timeout, and the call ends *nothing answered* (`core/net/call_record.h` is
// the receiving side of it, written down since task p15-02).
//
// When that command fails, the connection's close has often already been handed over - the
// server sends its answer under `Connection: close` and the `+SQNSH` follows it - and until this
// task the socket layer read the pair as *the other end hung up*, answered `TlsRecv::kClosed`,
// and the call was over. **The modem had never been asked a second time.** Whether it still
// holds those bytes when the failed command gives up, nobody has looked; if it does, one more
// ask is the whole cure for the data - the call ends well, the batch comes off the ring and
// nothing is posted twice - and it is still thirty seconds late.
//
// So this is the rule, with no modem in it, so that a host test can drive it
// (`core/net/socket_rotation.h` is written the same way and for the same reason):
//
//   **A receive that failed with the close seen gets one more ask.** The modem is asked once
//   more for what it holds. Bytes are the answer and the response is read on from there as if
//   the first ask had returned them. Nothing, or a second failure: *closed*, as before this
//   rule.
//
//   **Once, not a loop.** There is one extra ask per call, spent whatever it finds, so a call
//   can cost at most one command more than it did. There is no third ask and no second one on
//   the next failure of the same call.
//
//   **A receive that failed with no close seen does not change at all.** That is the socket
//   itself failing (`interfaces::HttpOutcome::kSocketFailed`, task p9-02), the connection may
//   still be alive, and the one thing known about it is that the modem would not answer - so
//   asking it again straight away says nothing. A close seen with nothing failed is the end of
//   the response, as before.
//
// **The deadline does not refuse it**, and that is a deviation from the task's own words worth
// reading twice (`tasks/phase-15/reports/03.md`). The command that failed is the command that
// overran the deadline: on the board it took 30 000 ms against a piece deadline of 15 000
// (`core/net/http_over_tls.h`, kDefaultPieceMs), so the wall is always past the deadline by the
// time this rule is asked and an ask gated on it would never be made at all - the rule would be
// dead code on the one unit it exists for. What bounds it instead is that there is exactly one
// of it: one AT command, which the library answers or gives up on in its own time, so the worst
// a failing call can now cost is two command timeouts instead of one. The wake's own uplink
// deadline still bounds the wake (`walter_agent/modem_uplink.h` begins no call without
// kLeastCallMs left of it).
//
// -- what is counted, and where it is kept -----------------------------------------------------
//
// Two running totals: how often the extra ask was made, and how often an answer came back that
// way. They are what says whether this ever does anything on a unit nobody has a cable to, so
// they are kept as `httpFailures` is - across a deep sleep, a restart and a power cut - in one
// value of the settings store under `kReadAgainKey`, the shape and the reasons of
// `core/cycle/wake_counters.h` and `walter_agent/flood_counters.h`. `agent calls` and the
// report's `calls` section print the pair.
//
// **Written only when a total moved**, and a total moves only when a receive failed with the
// close seen - so a unit whose calls behave spends no flash on this key at all, and the 79-byte
// record beside it is written in the same wake anyway when the call then failed.
//
// -- why it lives inside the record ------------------------------------------------------------
//
// The one instance in the firmware is a member of `core::net::CallRecord` (`again()`), and that
// is a decision about bytes and not about concerns. The rule is reached by exactly the two
// objects the record is - the socket layer below the transport, which makes the ask, and
// `walter_agent/call_log.h`, which keeps the totals and prints them - and both already hold a
// pointer to the record, which is taken from the heap in `app_main` (task p15-02). A pointer of
// its own in each would be four bytes of `.bss` apiece, and the margin the main task's stack is
// found in is counted in tens of bytes (`docs/ARCHITECTURE.md` §14, rail R22). The class, its
// file and its tests are its own: there is nothing of the record in the rule, and a host test
// drives the rule alone.
//
// **A build with no record makes no second ask**, because the rule is reached through the record
// and through nothing else. `main/main.cpp` always has one; a test that wires no record gets the
// behaviour this firmware had before this task, which is the honest fallback for a build that
// writes nothing down either.
#pragma once

#include <cstddef>
#include <cstdint>

#include "interfaces/key_value.h"
#include "interfaces/text_out.h"

namespace core {
namespace net {

// What the socket does about a receive that failed.
enum class AfterFailure : std::uint8_t {
  // No close seen: the socket itself failed, which is TlsRecv::kError and
  // interfaces::HttpOutcome::kSocketFailed. Nothing about this moved in task p15-03.
  kFailed = 0,
  // The close was seen and the one extra ask of this call has not been spent: ask the modem once
  // more for what it holds.
  kAskAgain = 1,
  // The close was seen and the extra ask is spent (or there is no record to spend it through):
  // TlsRecv::kClosed, as before this rule.
  kClosed = 2,
};

// The whole rule, in one expression, so that a host test can read the table and the socket layer
// has nothing of its own to get wrong (`core/net/socket_rotation.h`'s `after()` is its model).
constexpr AfterFailure after(bool closed_seen, bool asked_already) {
  if (!closed_seen) return AfterFailure::kFailed;
  return asked_already ? AfterFailure::kClosed : AfterFailure::kAskAgain;
}

// What the one extra ask found. The numbers are stored in the record (`core/net/call_record.h`),
// so they are not reordered.
enum class ReadAgainFound : std::uint8_t {
  kNothing = 0,  // the modem answered and held nothing: *closed*, as before this rule
  kBytes = 1,    // it still held the response: the call goes on from there
  kFailed = 2,   // the ask failed too: *closed*, and no third
  kCount,
};

// The key, and what is kept under it: a version byte and the two totals, little-endian - the
// shape `agent_wakes` and `agent_flood` have, read and written the same way. Fifteen characters
// is NVS's limit (interfaces/key_value.h) and this is twelve.
constexpr const char kReadAgainKey[] = "agent_reread";
constexpr std::uint8_t kReadAgainVersion = 1;
constexpr std::size_t kReadAgainBytes = 1 + 2 * sizeof(std::uint32_t);
constexpr std::size_t kReadAgainHexBytes = 2 * kReadAgainBytes + 1;
static_assert(kReadAgainHexBytes - 1 <= interfaces::kKeyValueMax,
              "the two totals of the one more ask have to fit one value of the store");

class ReadAgain {
 public:
  // -- as a call runs --------------------------------------------------------------------------

  // A new call: the one extra ask is unspent again. Called by `CallRecord::begin()`, which is
  // the one place a call begins, so that the ask cannot be left spent by the call before it.
  // The totals are **not** touched.
  void newCall();

  // The rule above, applied: what the socket does about a receive that just failed. When it
  // answers kAskAgain the ask is spent here and now - so a second failure of the same call is
  // kClosed, whatever the caller then does with the answer - and the total of asks moves.
  AfterFailure onFailedReceive(bool closed_seen);

  // What that ask found. `CallRecord::readAgain()` calls this in the same breath as it writes the
  // moment down, so the count and the record can never disagree.
  void found(ReadAgainFound what);

  // Whether the extra ask of the call now running has been spent.
  bool spent() const { return spent_; }

  // -- the totals ------------------------------------------------------------------------------

  std::uint32_t asks() const { return asks_; }
  std::uint32_t answers() const { return answers_; }
  // Whether stored totals were read back at begin(): false is a first boot, a store that would
  // not open, or a value of another shape.
  bool survived() const { return survived_; }
  // A stored value of another version or shape, counted rather than guessed at.
  std::uint16_t refused() const { return refused_; }
  // Whether a total has moved since the last save(): what the caller counts a write by, so that
  // a save with nothing to write is not counted as one (`walter_agent/call_log.h`).
  bool moved() const { return dirty_; }

  // Once a boot: the totals read back from the store. A null store, or one that is not ready,
  // counts for this boot alone - which is what every total of this firmware does without one.
  void begin(interfaces::KeyValue* store);

  // The totals to the store, **only when one moved**. True when there was nothing to write or
  // the write went through; false when there was something and it did not go in.
  bool save(interfaces::KeyValue* store);

  // The key erased: the bench, and a test. The totals are forgotten with it, so that
  // `agent calls` after a `forget` says the same thing a new unit says.
  void forget(interfaces::KeyValue* store);

  // The one line `agent calls` and the report's `calls` section print, ended with '\n' and well
  // inside the console's 200 bytes (task p9-04).
  void describe(interfaces::TextOut& out) const;

 private:
  bool decode(const char* hex);

  std::uint32_t asks_ = 0;
  std::uint32_t answers_ = 0;
  std::uint16_t refused_ = 0;
  bool spent_ = false;
  bool survived_ = false;
  bool dirty_ = false;
};

}  // namespace net
}  // namespace core
