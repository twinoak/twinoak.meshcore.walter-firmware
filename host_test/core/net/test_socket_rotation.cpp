// The rule itself (task p8-03, core/net/socket_rotation.h): which id a connection takes, which
// events it believes, what an owed close is and what happens when one never comes. The fault it
// exists for is end to end in test_late_close.cpp; this is every branch of the rule, including
// the bounded wait - a close that arrives *during* it, which only a clock with a hook can
// arrange.
#include <doctest.h>

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
  CHECK(rotation.owedTimeouts() == 1);

  // A deadline already past is no wait at all.
  rotation.end();
  const std::uint64_t at = clock.now_ms;
  rotation.begin(kA, at);
  CHECK(clock.now_ms == at);
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

TEST_CASE("a debt nobody pays stays: the cost of owing a close that never comes") {
  fakes::FakeClock clock;
  net::SocketRotation rotation(clock);
  rotation.useIds(kA, 0);

  // Owing a close for a connection nothing will ever close for - which is what end() would do
  // after a handshake with no request behind it, and why the modem layer calls cancel() there
  // instead (socket_rotation.h, *What a debt nobody pays costs*).
  rotation.begin(kA, 100000);
  rotation.end();
  CHECK(rotation.owes(kA));

  // From here on this id is one close behind, for good: every connection waits in vain and then
  // has its own close discounted. The wait is the cost; no byte of a response is, because the
  // parser and not the close is what ends one.
  for (int i = 0; i < 3; ++i) {
    const std::uint64_t at = clock.now_ms;
    rotation.begin(kA, 100000);
    CHECK(clock.now_ms - at == net::kOwedCloseWaitMs);
    rotation.onClose(kA);
    CHECK_FALSE(rotation.closedSeen());
    rotation.end();
    CHECK(rotation.owes(kA));
  }
  CHECK(rotation.owedTimeouts() == 3);
  CHECK(rotation.discountedCloses() == 3);

  // And the way out is the one the modem layer takes: a connection nothing answered owes
  // nothing, so the debt is never made in the first place.
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
