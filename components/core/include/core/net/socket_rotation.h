// Which socket a connection uses, and whose events it believes (task p8-03, bounded by p9-02).
// One request is one connection (core/net/http_over_tls.h) and a request ends when its
// Content-Length has been read, which is *before* the server's close reaches us. On the board that
// close is a `+SQNSH: <id>` the modem's library turns into a queued event and dispatches a moment
// later, and until v0.1.1 every connection was socket 1 and every open cleared the one close flag:
// the late event landed on the next connection, which then stopped sending (`the request did not
// go out`) or ended its body early (`the connection closed before the body was complete`). Three
// wakes in a row on the first unit, 6 November 2026, and a claimed job (21026) leased by
// meshmaster.dk and never seen.
//
// This is the rule, with no modem in it, so that a host test can make the close arrive late:
//
//   **One connection, one socket.** Consecutive connections take different ids - two of them,
//   turn and turn about - so a close belonging to the connection before this one names an id
//   this one is not using, and the accounting is per id: a ring or a close for the other id
//   changes nothing here.
//
//   **A close can be owed.** When a connection ends without its close having been seen, that id
//   *owes* one: the server's `+SQNSH` is still on its way. The debt is at most one per id,
//   because a socket's life produces at most one close.
//
//   **A connection on an id that owes a close waits for it, briefly, and then discounts the
//   first close it gets.** The wait is kOwedCloseWaitMs, in blocking steps so the task that
//   delivers the event can run; if the close has still not come, the connection goes ahead and
//   the first close seen on that id pays the debt instead of ending the connection. That is the
//   "two connections late" case - with two ids, a close two connections late arrives on the id
//   coming round again - and it is deterministic: the debt is paid exactly once, by exactly one
//   close event, whenever it turns up.
//
//   **A debt lives for one connection.** A debt that is still unpaid when the connection that
//   waited for it has finished its exchange is *written off*, and that id is then marked as one
//   whose closes do not come: nothing is owed on it and no connection on it waits for or
//   discounts a close, until a close actually arrives - one does, and the id goes back to the
//   rule above. This is task p9-02, the review's S2: before it, a close that was never delivered
//   left that id one close behind **for the modem's life** - every connection on it waited
//   kOwedCloseWaitMs in vain and then had its own close discounted, so a dropped body read as a
//   stall. Now the whole cost of a lost `+SQNSH` is one wait and one write-off, per id, once.
//
// **A lost close is not hypothetical.** The library queues socket events into a queue of
// WALTER_MODEM_EVENT_QUEUE_MAX_ITEMS = 8 with `xQueueSend(..., 0)` - a full queue drops the event
// and says nothing - and its event task takes **one event per loop**, each loop a
// `vTaskDelay(10)` plus another 10 ms before the dispatch (WalterModem.cpp,
// `_eventProcessingTask`). So a response that rings more than eight times inside ~160 ms loses
// events, and one of them can be the `+SQNSH`. The same figures are what kOwedCloseWaitMs is set
// from, below.
//
// **The accounting of an id is one value, and every step is one atomic transition** (task p9-02,
// the review's S3). It is written by the task that delivers the modem's events as well as by the
// task making the request, and v0.1.1 kept it in two `volatile bool`s read and set one after the
// other: one interleaving of end() and onClose() left *both* set, which was the permanent debt
// above. There is nothing left to interleave - CloseState below is the whole accounting of an id,
// `after()` is every transition of it, and each of begin(), end(), cancel() and onClose() applies
// exactly one of them with a compare-exchange. Whatever the two tasks do, the result is one of
// the two orders, and `after()` is written so that both are consistent.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "interfaces/clock.h"

namespace core {
namespace net {

// The ids a rotation holds: this connection's and the one before it. A third would only move the
// "two connections late" case to "three connections late"; the debt covers both the same way.
constexpr int kRotationIds = 2;

// How long a connection waits for a close its id still owes before going ahead without it.
// Read from the library (the paragraph above): an event is dispatched a `vTaskDelay(10)` and a
// second 10 ms after it was queued, one event per loop, so the `+SQNSH` of a connection whose
// response rang a few times is tens of milliseconds behind the response - and by the time the
// next connection on that id is being opened, a whole connection later, it has long arrived.
// 250 ms is a dozen of those dispatches: it is spent only when something has gone wrong, and
// since task p9-02 it is spent at most once per id until a close arrives. TO BE SET ON THE BENCH
// (docs/bench.md §13).
constexpr std::uint32_t kOwedCloseWaitMs = 250;

// The step of that wait. Ten FreeRTOS ticks at CONFIG_FREERTOS_HZ = 1000 (task p9-01), and the
// library needs 20 ms to dispatch an event at all, so a finer step would only poll harder for
// something that cannot be there yet. Every step is a vTaskDelay and not a busy wait: the event
// being waited for comes from another task, and task p8-01's lesson is that a wait which never
// blocks lets nothing else run.
constexpr std::uint32_t kOwedCloseStepMs = 10;

// How many connections' ids are kept for the console to print (`http get`, `lte`): enough for a
// wake's four requests and the pair of `http get`s before them.
constexpr std::size_t kRecentConnections = 8;

// The whole close accounting of one id, in one value.
enum class CloseState : std::uint32_t {
  // Nothing owed, no close seen: what an id is before its first connection and between two
  // well-behaved ones.
  kClear = 0,
  // A close has arrived and the connection in progress (or the one just finished) is the one it
  // belongs to. This is what closedSeen() answers.
  kSeen = 1,
  // An earlier connection on this id ended without its close: one is on its way.
  kOwed = 2,
  // Owed, and the connection now running is the one that waited for it: its last chance. If it
  // is still unpaid when that connection's exchange is over, the debt is written off.
  kLastChance = 3,
  // This id's closes do not come (a debt was written off and no close has arrived since): owe
  // nothing, wait for nothing, discount nothing. The first close that does arrive ends it.
  kQuiet = 4,
};

// The four things that happen to an id, each one atomic transition.
enum class CloseStep : std::uint8_t {
  kBegin,       // a connection on this id is about to be dialled
  kCloseEvent,  // the task that delivers the modem's events brought a close for this id
  kEnd,         // the connection is over and the other end answered it
  kCancel,      // the connection is over and nothing came of it
};

// Every transition of the rule, in one place, as one expression per step - so that a host test
// can read the whole table and so that there is nothing between a read and a write to interleave.
constexpr CloseState after(CloseStep step, CloseState state) {
  switch (step) {
    case CloseStep::kBegin:
      // A debt that survived the wait goes into its last chance; a close left over from the
      // connection before this one says nothing about this one and is cleared.
      return state == CloseState::kOwed  ? CloseState::kLastChance
             : state == CloseState::kSeen ? CloseState::kClear
                                          : state;
    case CloseStep::kCloseEvent:
      // A close owed is a close paid, however late, and it tells this connection nothing.
      // Anything else is this connection's own close, and ends any quiet.
      return (state == CloseState::kOwed || state == CloseState::kLastChance)
                 ? CloseState::kClear
                 : CloseState::kSeen;
    case CloseStep::kEnd:
      // The close was seen, so nothing is owed; a debt in its last chance is written off and the
      // id goes quiet; a quiet id owes nothing; otherwise the close is still on its way.
      return state == CloseState::kSeen         ? CloseState::kClear
             : state == CloseState::kLastChance ? CloseState::kQuiet
             : state == CloseState::kQuiet      ? CloseState::kQuiet
                                                : CloseState::kOwed;
    case CloseStep::kCancel:
      // Nothing came of this connection, so it changes no accounting: a debt in its last chance
      // has had no chance at all and goes back to being owed.
      return state == CloseState::kLastChance ? CloseState::kOwed : state;
  }
  return state;
}

class SocketRotation {
 public:
  explicit SocketRotation(interfaces::Clock& clock) : clock_(clock) {}

  // The two ids to take turn and turn about. `second` of 0, or equal to `first`, is one id and
  // the debt rule alone - which is what the modem layer falls back to when its library will not
  // give out a second socket (components/modem/walter_tls.h). Forgets everything known about the
  // ids it held: it is called when the modem has restarted and its sockets are new.
  void useIds(int first, int second);

  int firstId() const { return id_[0]; }
  int secondId() const { return id_[1]; }
  int idCount() const { return id_[1] != 0 ? 2 : 1; }

  // Which id the next connection should use. No side effect; the same answer until begin().
  int nextId() const;

  // A connection on `id` is about to be dialled. Waits, bounded by kOwedCloseWaitMs and by
  // `deadline_ms`, for a close this id still owes; clears what the last connection on it was
  // told; makes it the current one. Answers `id`, or 0 when `id` is not one of the two (a
  // caller's mistake).
  int begin(int id, std::uint64_t deadline_ms);

  // The id the connection in progress is on, 0 when there is none.
  int currentId() const;
  bool live() const { return live_; }

  // The connection is over and the other end answered it, so its close is on its way: when it
  // has not been seen yet, this id owes one - unless a debt of its own has just been written off
  // (the header, *A debt lives for one connection*), after which this id is owed nothing until a
  // close arrives.
  void end();

  // The connection is over and nothing came of it - never dialled, or dialled and never
  // answered - so no close is coming for it and nothing new is owed. Whatever the id owed
  // before, it still owes.
  void cancel();

  // -- from the task that delivers the modem's socket events ---------------------------------
  // `id` is whatever the event named; an id this rotation does not hold is counted and ignored.
  void onRing(int id);
  void onClose(int id);

  // -- what the connection in progress has been told -----------------------------------------
  bool closedSeen() const;
  bool ringSeen() const;
  void clearRing();

  // -- counters, for the console and the tests -----------------------------------------------
  std::uint32_t connections() const { return connections_; }
  std::uint32_t owedWaits() const { return owed_waits_; }
  std::uint32_t owedTimeouts() const { return owed_timeouts_; }
  std::uint32_t writeOffs() const { return write_offs_; }
  std::uint32_t discountedCloses() const { return discounted_closes_.load(std::memory_order_relaxed); }
  std::uint32_t otherIdEvents() const { return other_id_events_.load(std::memory_order_relaxed); }
  bool owes(int id) const;
  // Whether this id is one whose closes do not come (CloseState::kQuiet).
  bool isQuiet(int id) const;

  // The ids of the last few connections, oldest first: `http get` prints the ones its own
  // request used and `lte` the ones of the whole boot so far.
  std::size_t recentCount() const;
  int recentId(std::size_t which) const;

 private:
  // The place of `id` among the two, or -1.
  int slotOf(int id) const;
  CloseState stateOf(int slot) const { return state_[slot].load(std::memory_order_acquire); }
  // One transition, whatever the other task is doing in the same moment. Answers the state the
  // id was in before it, which is what the counters are made of.
  CloseState apply(int slot, CloseStep step);

  interfaces::Clock& clock_;
  int id_[kRotationIds] = {0, 0};
  int slot_ = -1;   // the slot of the current (or last) connection, -1 before the first
  bool live_ = false;
  // The accounting, written by the event task as well as by this one: one value per id and one
  // atomic transition per step (the header's last paragraph).
  std::atomic<CloseState> state_[kRotationIds] = {{CloseState::kClear}, {CloseState::kClear}};
  // A ring is a cue and nothing more - it says "ask the modem now" and losing one costs a poll
  // (components/modem/walter_tls.cpp, receive) - so it keeps a flag of its own.
  std::atomic<bool> ring_[kRotationIds] = {{false}, {false}};
  std::uint32_t connections_ = 0;
  std::uint32_t owed_waits_ = 0;
  std::uint32_t owed_timeouts_ = 0;
  std::uint32_t write_offs_ = 0;
  // These two are counted by the event task.
  std::atomic<std::uint32_t> discounted_closes_{0};
  std::atomic<std::uint32_t> other_id_events_{0};
  std::uint8_t recent_[kRecentConnections] = {};
};

}  // namespace net
}  // namespace core
