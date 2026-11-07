// The rule itself (task p8-03, core/net/socket_rotation.h): which id a connection takes, which
// events it believes, what an owed close is and what happens when one never comes. The fault it
// exists for is end to end in test_late_close.cpp; this is every branch of the rule, including
// the bounded wait - a close that arrives *during* it, which only a clock with a hook can
// arrange.
#include <doctest.h>

#include <cstddef>
#include <cstdint>

#include "core/net/socket_rotation.h"
#include "fakes/fake_clock.h"

namespace {

namespace net = core::net;

constexpr int kA = 1;
constexpr int kB = 6;  // the id the library's reserve actually hands out (walter_tls.h)

// One connection, start to finish: the id it took.
int oneConnection(net::SocketRotation& rotation, std::uint64_t deadline_ms, bool closeIsSeen) {
  const int id = rotation.begin(rotation.nextId(), deadline_ms);
  if (closeIsSeen) rotation.onClose(id);
  rotation.end();
  return id;
}

}  // namespace

TEST_CASE("consecutive connections take different ids, turn and turn about") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);

  CHECK(rotation.idCount() == 2);
  CHECK(rotation.currentId() == 0);
  CHECK(rotation.nextId() == kA);

  int ids[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 6; ++i) ids[i] = oneConnection(rotation, 100000, true);
  CHECK(ids[0] == kA);
  CHECK(ids[1] == kB);
  CHECK(ids[2] == kA);
  CHECK(ids[3] == kB);
  CHECK(ids[4] == kA);
  CHECK(ids[5] == kB);
  CHECK(rotation.connections() == 6);
  // Every close was seen before the connection ended, so nothing was ever owed and nothing waited.
  CHECK(rotation.owedWaits() == 0);
  CHECK(rotation.discountedCloses() == 0);
}

TEST_CASE("one id is allowed: the debt rule on its own") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, 0);
  CHECK(rotation.idCount() == 1);
  CHECK(rotation.nextId() == kA);
  CHECK(oneConnection(rotation, 100000, true) == kA);
  CHECK(rotation.nextId() == kA);
  CHECK(oneConnection(rotation, 100000, true) == kA);

  // The same when the second id repeats the first.
  net::SocketRotation same(clock);
  same.useIds(kA, kA);
  CHECK(same.idCount() == 1);
  CHECK(same.secondId() == 0);
}

TEST_CASE("an event for the id not in use changes nothing about the one in use") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);
  rotation.begin(kA, 100000);

  rotation.onClose(kB);
  rotation.onRing(kB);
  CHECK_FALSE(rotation.closedSeen());
  CHECK_FALSE(rotation.ringSeen());

  rotation.onRing(kA);
  CHECK(rotation.ringSeen());
  rotation.clearRing();
  CHECK_FALSE(rotation.ringSeen());
  rotation.onClose(kA);
  CHECK(rotation.closedSeen());

  // And an id this rotation does not hold at all is counted and ignored.
  const std::uint32_t before = rotation.otherIdEvents();
  rotation.onClose(4);
  rotation.onRing(4);
  CHECK(rotation.otherIdEvents() == before + 2);
}

TEST_CASE("a close one connection late lands on the other id and is discounted") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);

  // The first connection ends on its Content-Length, before its close arrived.
  CHECK(rotation.begin(rotation.nextId(), 100000) == kA);
  rotation.end();
  CHECK(rotation.owes(kA));

  // The second takes the other id and clears its flags; then the first one's close turns up.
  CHECK(rotation.begin(rotation.nextId(), 100000) == kB);
  rotation.onClose(kA);
  CHECK(rotation.discountedCloses() == 1);
  CHECK_FALSE(rotation.owes(kA));
  CHECK_FALSE(rotation.closedSeen());  // nothing about *this* connection
  CHECK(rotation.owedWaits() == 0);    // kB owed nothing, so nothing was waited for
}

TEST_CASE("a close two connections late: the id waits, then discounts the first close it gets") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);

  rotation.begin(kA, 100000);
  rotation.end();           // kA owes a close
  rotation.begin(kB, 100000);
  rotation.onClose(kB);
  rotation.end();           // kB owes nothing
  CHECK(rotation.owes(kA));

  // kA comes round again with its close still in the air. It waits the bounded time, which is
  // spent and no more, and goes ahead.
  const std::uint64_t at = clock.now_ms;
  CHECK(rotation.begin(kA, 100000) == kA);
  CHECK(clock.now_ms - at == net::kOwedCloseWaitMs);
  CHECK(rotation.owedWaits() == 1);
  CHECK(rotation.owedTimeouts() == 1);
  CHECK(rotation.owes(kA));             // still owed: the debt is paid by the next close
  CHECK_FALSE(rotation.closedSeen());

  // And the late close, whenever it turns up, pays the debt and says nothing about this one.
  rotation.onClose(kA);
  CHECK(rotation.discountedCloses() == 1);
  CHECK_FALSE(rotation.closedSeen());

  // This connection's own close is believed.
  rotation.onClose(kA);
  CHECK(rotation.closedSeen());
}

TEST_CASE("the wait ends the moment the owed close arrives") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);
  rotation.begin(kA, 100000);
  rotation.end();  // kA owes a close

  // The event task delivers it 30 ms into the wait: the clock's hook is the other task here.
  const std::uint64_t deliver_at = clock.now_ms + 30;
  clock.on_delay = [&](std::uint32_t) {
    if (clock.now_ms >= deliver_at) rotation.onClose(kA);
  };

  const std::uint64_t at = clock.now_ms;
  rotation.begin(kB, 100000);  // the other id first: it owes nothing, so no wait
  CHECK(clock.now_ms == at);
  rotation.end();

  CHECK(rotation.begin(kA, 100000) == kA);
  clock.on_delay = nullptr;
  // It waited, and stopped as soon as the close came: far short of the whole wait.
  CHECK(rotation.owedWaits() == 1);
  CHECK(rotation.owedTimeouts() == 0);
  CHECK(rotation.discountedCloses() == 1);
  CHECK(clock.now_ms - at <= 30 + net::kOwedCloseStepMs);
  CHECK_FALSE(rotation.owes(kA));
  CHECK_FALSE(rotation.closedSeen());
}

TEST_CASE("the wait never passes the caller's deadline") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, 0);
  rotation.begin(kA, 100000);
  rotation.end();  // kA owes a close, and it never comes

  const std::uint64_t deadline = clock.now_ms + 40;
  rotation.begin(kA, deadline);
  CHECK(clock.now_ms <= deadline);
  CHECK(clock.now_ms - (deadline - 40) <= 40);
  CHECK(rotation.owedTimeouts() == 1);

  // A deadline already past is no wait at all. On a rotation of its own, because the one above
  // has just written its debt off (*A debt lives for one connection*) and owes nothing now.
  net::SocketRotation pressed(clock);
  pressed.useIds(kA, 0);
  pressed.begin(kA, 100000);
  pressed.end();
  REQUIRE(pressed.owes(kA));
  const std::uint64_t at = clock.now_ms;
  pressed.begin(kA, at);
  CHECK(clock.now_ms == at);
  CHECK(pressed.owedWaits() == 1);
  CHECK(pressed.owedTimeouts() == 1);
}

TEST_CASE("a close seen while the connection is being settled owes nothing") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);
  rotation.begin(kA, 100000);
  rotation.onClose(kA);  // the server's close arrived before the request ended
  rotation.end();
  CHECK_FALSE(rotation.owes(kA));

  // So the id coming round again waits for nothing.
  rotation.begin(kB, 100000);
  rotation.end();
  const std::uint64_t at = clock.now_ms;
  rotation.begin(kA, 100000);
  CHECK(clock.now_ms == at);
  CHECK(rotation.owedWaits() == 0);
}

TEST_CASE("a debt nobody pays is written off, and then that id expects no close at all") {
  // The review's S2 (task p9-02). Before it this test measured the cost of a debt that stayed:
  // every connection on the id waited kOwedCloseWaitMs in vain and then had its own close
  // discounted, for the modem's life. Now the debt lives for one connection.
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, 0);

  // A connection whose close never comes: the modem's event queue is eight deep and drops what
  // does not fit (socket_rotation.h), so this is a `+SQNSH` that was queued and lost.
  rotation.begin(kA, 100000);
  rotation.end();
  CHECK(rotation.owes(kA));

  // The next connection on that id waits for it, once.
  const std::uint64_t waited_at = clock.now_ms;
  rotation.begin(kA, 100000);
  CHECK(clock.now_ms - waited_at == net::kOwedCloseWaitMs);
  CHECK(rotation.owedWaits() == 1);
  CHECK(rotation.owedTimeouts() == 1);
  CHECK(rotation.owes(kA));  // its last chance: a close arriving now would still pay it
  // Nothing comes, and that connection's exchange ends: the debt is written off and the id is
  // marked as one whose closes do not come.
  rotation.end();
  CHECK(rotation.writeOffs() == 1);
  CHECK_FALSE(rotation.owes(kA));
  CHECK(rotation.isQuiet(kA));

  // From here on the id costs nothing: no connection waits, nothing is owed, and nothing would
  // be discounted. Ten connections on a modem that answers no close at all.
  for (int i = 0; i < 10; ++i) {
    const std::uint64_t at = clock.now_ms;
    rotation.begin(kA, 100000);
    CHECK(clock.now_ms == at);
    rotation.end();
    CHECK_FALSE(rotation.owes(kA));
  }
  CHECK(rotation.owedWaits() == 1);
  CHECK(rotation.owedTimeouts() == 1);
  CHECK(rotation.writeOffs() == 1);
  CHECK(rotation.discountedCloses() == 0);
}

TEST_CASE("a quiet id believes the first close it does get, and goes back to the rule") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, 0);
  rotation.begin(kA, 100000);
  rotation.end();           // owes
  rotation.begin(kA, 100000);
  rotation.end();           // the debt is written off
  REQUIRE(rotation.isQuiet(kA));

  // A connection on a quiet id: a close arriving is this connection's own - which is what a
  // dropped body looks like - and is believed, not discounted. That is the half of S2 that
  // mattered: a real early close read as a stall.
  rotation.begin(kA, 100000);
  rotation.onClose(kA);
  CHECK(rotation.closedSeen());
  CHECK(rotation.discountedCloses() == 0);
  rotation.end();
  CHECK_FALSE(rotation.isQuiet(kA));
  CHECK_FALSE(rotation.owes(kA));

  // And the rule is back: the next connection that ends without its close owes one again.
  rotation.begin(kA, 100000);
  rotation.end();
  CHECK(rotation.owes(kA));
}

TEST_CASE("a cancelled connection is no chance at all: the debt goes back to being owed") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, 0);
  rotation.begin(kA, 100000);
  rotation.end();  // owes

  // The connection that waited for it was never answered - a dial the modem refused, which is
  // the third shape of 7 November - so it proves nothing about whether closes arrive.
  rotation.begin(kA, 100000);
  rotation.cancel();
  CHECK(rotation.writeOffs() == 0);
  CHECK(rotation.owes(kA));
  CHECK_FALSE(rotation.isQuiet(kA));

  // The next one that does exchange is the one that settles it.
  rotation.begin(kA, 100000);
  rotation.end();
  CHECK(rotation.writeOffs() == 1);
  CHECK(rotation.isQuiet(kA));
}

TEST_CASE("the way the modem layer avoids the debt: a connection nothing answered owes nothing") {
  fakes::FakeClock clock;
  net::SocketRotation careful(clock);
  careful.useIds(kA, 0);
  careful.begin(kA, 100000);
  careful.cancel();
  CHECK_FALSE(careful.owes(kA));
  const std::uint64_t at = clock.now_ms;
  careful.begin(kA, 100000);
  CHECK(clock.now_ms == at);
  careful.onClose(kA);
  CHECK(careful.closedSeen());
}

TEST_CASE("every transition of the rule, and both orders of the two tasks' steps") {
  // The review's S3 (task p9-02): end() runs on the task making the request and onClose() on the
  // task that delivers the library's events, and v0.1.1 read and set two flags one after the
  // other. The table below is the whole accounting now, so the only interleavings there are are
  // the two orders - and this says what each of them leaves behind.
  using net::CloseState;
  using net::CloseStep;

  // kBegin: a debt that survived the wait gets its last chance; a stale close is cleared.
  CHECK(net::after(CloseStep::kBegin, CloseState::kClear) == CloseState::kClear);
  CHECK(net::after(CloseStep::kBegin, CloseState::kSeen) == CloseState::kClear);
  CHECK(net::after(CloseStep::kBegin, CloseState::kOwed) == CloseState::kLastChance);
  CHECK(net::after(CloseStep::kBegin, CloseState::kLastChance) == CloseState::kLastChance);
  CHECK(net::after(CloseStep::kBegin, CloseState::kQuiet) == CloseState::kQuiet);

  // kCloseEvent: a debt is paid; anything else is this connection's own close.
  CHECK(net::after(CloseStep::kCloseEvent, CloseState::kClear) == CloseState::kSeen);
  CHECK(net::after(CloseStep::kCloseEvent, CloseState::kSeen) == CloseState::kSeen);
  CHECK(net::after(CloseStep::kCloseEvent, CloseState::kOwed) == CloseState::kClear);
  CHECK(net::after(CloseStep::kCloseEvent, CloseState::kLastChance) == CloseState::kClear);
  CHECK(net::after(CloseStep::kCloseEvent, CloseState::kQuiet) == CloseState::kSeen);

  // kEnd: the close seen is the close accounted for; a last chance is written off.
  CHECK(net::after(CloseStep::kEnd, CloseState::kClear) == CloseState::kOwed);
  CHECK(net::after(CloseStep::kEnd, CloseState::kSeen) == CloseState::kClear);
  CHECK(net::after(CloseStep::kEnd, CloseState::kOwed) == CloseState::kOwed);
  CHECK(net::after(CloseStep::kEnd, CloseState::kLastChance) == CloseState::kQuiet);
  CHECK(net::after(CloseStep::kEnd, CloseState::kQuiet) == CloseState::kQuiet);

  // kCancel: nothing came of the connection, so nothing of the accounting moves on.
  CHECK(net::after(CloseStep::kCancel, CloseState::kClear) == CloseState::kClear);
  CHECK(net::after(CloseStep::kCancel, CloseState::kSeen) == CloseState::kSeen);
  CHECK(net::after(CloseStep::kCancel, CloseState::kOwed) == CloseState::kOwed);
  CHECK(net::after(CloseStep::kCancel, CloseState::kLastChance) == CloseState::kOwed);
  CHECK(net::after(CloseStep::kCancel, CloseState::kQuiet) == CloseState::kQuiet);

  // **The interleaving the review describes.** A connection is in progress with nothing owed,
  // and the server's close and the end of the request happen in the same moment. v0.1.1 read
  // `closed_`, wrote `owed_`, and re-read `closed_`; one order of the event task's read of
  // `owed_` and its write of `closed_` left *both* set, which is a debt no close can ever pay.
  // Here there is one value, so there are two orders and no third:
  //   the close first:  the connection saw it, so nothing is owed.
  CHECK(net::after(CloseStep::kEnd, net::after(CloseStep::kCloseEvent, CloseState::kClear)) ==
        CloseState::kClear);
  //   the end first:    the close pays the debt the moment it arrives.
  CHECK(net::after(CloseStep::kCloseEvent, net::after(CloseStep::kEnd, CloseState::kClear)) ==
        CloseState::kClear);
  // Neither leaves anything owed, which is what v0.1.1 could not promise. And whichever order a
  // step is taken in, the state is one of the five: there is no "both".
  for (int s = 0; s <= 4; ++s) {
    for (int step = 0; step <= 3; ++step) {
      const CloseState from = static_cast<CloseState>(static_cast<std::uint32_t>(s));
      const CloseState to = net::after(static_cast<CloseStep>(static_cast<std::uint8_t>(step)), from);
      const bool known = to == CloseState::kClear || to == CloseState::kSeen ||
                         to == CloseState::kOwed || to == CloseState::kLastChance ||
                         to == CloseState::kQuiet;
      CHECK(known);
    }
  }
}

TEST_CASE("the interleaving the review describes, through the rotation itself") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);

  // The close arrives first and the request then ends: the connection was told, and nothing is
  // owed - so the next connection on this id waits for nothing.
  rotation.begin(kA, 100000);
  rotation.onClose(kA);
  rotation.end();
  CHECK_FALSE(rotation.owes(kA));

  // The request ends first and the close arrives a moment later: the debt is made and paid, and
  // the connection in progress on the *other* id is told nothing.
  rotation.begin(kB, 100000);
  rotation.end();
  CHECK(rotation.owes(kB));
  rotation.begin(kA, 100000);
  rotation.onClose(kB);
  CHECK_FALSE(rotation.owes(kB));
  CHECK_FALSE(rotation.closedSeen());
  CHECK(rotation.discountedCloses() == 1);

  // Either way the id is clear afterwards: no connection on it waits, and none of its own
  // closes are discounted. This is the state v0.1.1's two flags could not reach.
  rotation.end();
  const std::uint64_t at = clock.now_ms;
  rotation.begin(kB, 100000);
  CHECK(clock.now_ms == at);
  rotation.onClose(kB);
  CHECK(rotation.closedSeen());
}

TEST_CASE("the id of each connection is kept for the console to print") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);

  CHECK(rotation.recentCount() == 0);
  CHECK(rotation.recentId(0) == 0);

  for (int i = 0; i < 3; ++i) oneConnection(rotation, 100000, true);
  REQUIRE(rotation.recentCount() == 3);
  CHECK(rotation.recentId(0) == kA);
  CHECK(rotation.recentId(1) == kB);
  CHECK(rotation.recentId(2) == kA);
  CHECK(rotation.recentId(3) == 0);  // past the end

  // More connections than are kept: the last kRecentConnections, oldest first.
  for (int i = 0; i < 20; ++i) oneConnection(rotation, 100000, true);
  REQUIRE(rotation.recentCount() == net::kRecentConnections);
  CHECK(rotation.connections() == 23);
  for (std::size_t i = 0; i < net::kRecentConnections; ++i) {
    // Connection 23 was on kB (even-numbered connections are), so the eight before it alternate
    // back from there.
    const int expected = ((23 - net::kRecentConnections + i) % 2 == 0) ? kA : kB;
    CHECK(rotation.recentId(i) == expected);
  }
}

TEST_CASE("a connection that was never dialled owes nothing, and keeps what was owed") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);

  rotation.begin(kA, 100000);
  rotation.cancel();  // configured, never dialled
  CHECK_FALSE(rotation.owes(kA));
  CHECK_FALSE(rotation.live());

  // What was owed before a cancelled connection is still owed after it.
  rotation.begin(kB, 100000);
  rotation.end();
  CHECK(rotation.owes(kB));
  rotation.begin(kB, 100000);
  CHECK(rotation.owedTimeouts() == 1);
  rotation.cancel();
  CHECK(rotation.owes(kB));

  // end() and cancel() on nothing live are both harmless.
  rotation.end();
  rotation.cancel();
  CHECK_FALSE(rotation.live());
}

TEST_CASE("useIds forgets everything: the modem restarted and its sockets are new") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);
  rotation.begin(kA, 100000);
  rotation.onRing(kA);
  rotation.end();
  CHECK(rotation.owes(kA));

  rotation.useIds(kA, 0);
  CHECK(rotation.idCount() == 1);
  CHECK_FALSE(rotation.owes(kA));
  CHECK(rotation.currentId() == 0);
  CHECK(rotation.nextId() == kA);
  const std::uint64_t at = clock.now_ms;
  rotation.begin(kA, 100000);
  CHECK(clock.now_ms == at);  // no debt survived, so no wait
  CHECK_FALSE(rotation.ringSeen());
}

TEST_CASE("an id the rotation does not hold is refused, and nothing becomes current") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, kB);
  CHECK(rotation.begin(3, 100000) == 0);
  CHECK(rotation.begin(0, 100000) == 0);
  CHECK_FALSE(rotation.live());
  CHECK(rotation.currentId() == 0);
  CHECK(rotation.connections() == 0);
  CHECK_FALSE(rotation.closedSeen());
  CHECK_FALSE(rotation.ringSeen());
  rotation.clearRing();  // harmless before the first connection
}
