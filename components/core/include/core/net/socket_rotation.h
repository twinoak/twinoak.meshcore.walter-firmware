// Which socket a connection uses, and whose events it believes (task p8-03). One request is one
// connection (core/net/http_over_tls.h) and a request ends when its Content-Length has been read,
// which is *before* the server's close reaches us. On the board that close is a `+SQNSH: <id>`
// the modem's library turns into a queued event and dispatches a moment later, and until v0.1.1
// every connection was socket 1 and every open cleared the one close flag: the late event landed
// on the next connection, which then stopped sending (`the request did not go out`) or ended its
// body early (`the connection closed before the body was complete`). Three wakes in a row on the
// first unit, 6 November 2026, and a claimed job (21026) leased by meshmaster.dk and never seen.
//
// This is the rule, with no modem in it, so that a host test can make the close arrive late:
//
//   **One connection, one socket.** Consecutive connections take different ids - two of them,
//   turn and turn about - so a close belonging to the connection before this one names an id
//   this one is not using, and the flags are per id: a ring or a close for the other id changes
//   nothing here.
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
// **What a debt nobody pays costs.** A close that is owed and never delivered cannot be told
// apart, from in here, from one that is merely very late: the id's accounting then stays one
// close behind for the rest of the modem's life - every connection on it waits kOwedCloseWaitMs
// in vain and then has its own close discounted. No byte of a request is lost by that, because
// every response this firmware asks for is delimited by a Content-Length or by chunks and the
// parser is what says a response is over (core/net/http_over_tls.h); but the wait is real. So
// the one caller owes a close only for a connection the other end **answered**: the server says
// `Connection: close` and sends its FIN after the last byte of the body, so a connection that
// gave us bytes has a close on its way, and a handshake on its own or a dial that failed has
// none (components/modem/walter_tls.cpp, close(), and end() against cancel() below).
//
// The flags are written by whichever task delivers the modem's events and read by the task that
// is making the request - one `volatile bool` each, nothing to tear, which is what
// components/modem/walter_tls.cpp relied on before and still does.
#pragma once

#include <cstdint>

#include "interfaces/clock.h"

namespace core {
namespace net {

// The ids a rotation holds: this connection's and the one before it. A third would only move the
// "two connections late" case to "three connections late"; the debt covers both the same way.
constexpr int kRotationIds = 2;

// How long a connection waits for a close its id still owes before going ahead without it. The
// close is queued by the URC parser the moment the server's FIN arrives and dispatched some
// milliseconds later, and by the time the next connection is being opened it has long since
// arrived - so this is spent only when something has gone wrong, and a quarter of a second is
// ten times the slack the dispatch needs. TO BE SET ON THE BENCH (docs/bench.md §13).
constexpr std::uint32_t kOwedCloseWaitMs = 250;

// The step of that wait. One FreeRTOS tick at CONFIG_FREERTOS_HZ = 100, so every step is a
// vTaskDelay and not a busy wait: the event being waited for comes from another task, and task
// p8-01's lesson is that a wait which never blocks lets nothing else run.
constexpr std::uint32_t kOwedCloseStepMs = 10;

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
  // `deadline_ms`, for a close this id still owes; clears the id's ring and close flags; makes
  // it the current one. Answers `id`, or 0 when `id` is not one of the two (a caller's mistake).
  int begin(int id, std::uint64_t deadline_ms);

  // The id the connection in progress is on, 0 when there is none.
  int currentId() const;
  bool live() const { return live_; }

  // The connection is over and the other end answered it, so its close is on its way: when it
  // has not been seen yet, this id owes one.
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
  std::uint32_t discountedCloses() const { return discounted_closes_; }
  std::uint32_t otherIdEvents() const { return other_id_events_; }
  bool owes(int id) const;

 private:
  // The place of `id` among the two, or -1.
  int slotOf(int id) const;

  interfaces::Clock& clock_;
  int id_[kRotationIds] = {0, 0};
  int slot_ = -1;   // the slot of the current (or last) connection, -1 before the first
  bool live_ = false;
  // Written by the event task as well as by this one: one byte each, read as a cue and never as
  // a pair (the header's last paragraph).
  volatile bool closed_[kRotationIds] = {false, false};
  volatile bool ring_[kRotationIds] = {false, false};
  volatile bool owed_[kRotationIds] = {false, false};
  std::uint32_t connections_ = 0;
  std::uint32_t owed_waits_ = 0;
  std::uint32_t owed_timeouts_ = 0;
  std::uint32_t discounted_closes_ = 0;
  std::uint32_t other_id_events_ = 0;
};

}  // namespace net
}  // namespace core
