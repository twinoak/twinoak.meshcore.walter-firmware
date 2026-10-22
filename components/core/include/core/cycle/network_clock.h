// The time of day, once the network has told us (task p3-05): `interfaces::WallClock` filled
// from the cellular network's clock, and **the only thing in this firmware that gives the radio a
// time of its own accord**.
//
// Until this task `main/` answered "I have no time to give" (`docs/ARCHITECTURE.md` §9: "Reading
// it into interfaces::WallClock - so that the link client has one to give - is the wake cycle's
// step and is task p3-05's"). This is that object: the wake cycle's step 6 reads the network's
// time through `core/uplink/policy.h`, and step 7 is what hands it to the radio.
//
// -- what it will and will not believe ---------------------------------------------------------
//
// A time is taken only when `core/uplink/clock_window.h` says it came from the network - not
// before this firmware's own build date, not twenty years after it - because a modem answers
// `AT+CCLK?` with *something* whatever happens, and spec/LINK-PROTOCOL.md §5.4 is explicit about
// the cost of a wrong one: a MeshCore node whose clock goes backwards makes its own traffic look
// replayed to everyone who heard the later packets. `TimeOrigin::kDefault` (1970, the module's
// build date) is refused here and reported as "no time", which is what the console already says.
//
// A person's `rak time set <epoch>` does **not** come through here: it is one TIME_SET with
// §5.4's source 3, sent by the console, and it does not become this firmware's own idea of the
// time. One place sets this, and it is the network.
//
// -- how it keeps time between answers ---------------------------------------------------------
//
// It holds the epoch it was given and the monotonic millisecond at which it was given, and
// answers `epoch + (now - taken) / 1000`. The monotonic clock is `interfaces::Clock`, which on
// this chip **restarts at zero at every wake from deep sleep** - so a time taken in one wake is
// gone in the next, and `now()` answers false again until the modem has been asked. That is the
// honest behaviour and not a limitation to work around: a Walter has no RTC it can trust across
// a sleep, and a time carried across one by arithmetic over a counter that restarted would be
// exactly the wrong clock §5.4 warns about. The radio is in the same position (the first-batch
// boards have no RTC fitted), which is why §5.4 has the Walter set its clock after every restart.
//
// Logic: no ESP-IDF include, no heap.
#pragma once

#include <cstdint>

#include "core/uplink/clock_window.h"
#include "interfaces/clock.h"
#include "interfaces/uplink.h"
#include "interfaces/wall_clock.h"

namespace core {
namespace cycle {

class NetworkClock final : public interfaces::WallClock {
 public:
  explicit NetworkClock(interfaces::Clock& clock) : clock_(clock) {}

  // A time the uplink read. True when it was believed (`TimeOrigin::kNetwork`); false leaves
  // whatever was known before, because a modem that answers 1970 has not unlearnt the time it
  // gave us a minute ago.
  bool give(const interfaces::NetworkTime& time);

  // The same, for a test and for the bench: an epoch judged by the same window.
  bool give(std::uint32_t epoch);

  // The **server's** own time, which every config reply, heartbeat reply and claim of the agent
  // carries (task p4-04; the library offers it through `mm::IClock::takeServerTime`). Judged by
  // the same window, and refused when the cellular network has already given one in this boot:
  // a time that came off the network wins, which is the rule the library's own clock keeps and a
  // sensible one - the network's is the modem's own clock and the server's has a round trip in
  // it. Taking it matters because §5.4's `TIME_SET` carries where the time came from, and a
  // Walter with a server time and no network time can still set the radio's clock and say
  // truthfully that the source was the server (source 2) rather than the network (source 1).
  bool giveFromServer(std::uint32_t epoch);

  // §5.4's source byte for the time this object holds: 1 the cellular network, 2 the server,
  // 0 when there is none.
  std::uint8_t source() const { return source_; }

  // interfaces::WallClock: the time now, with §5.4's source for where it came from. False
  // until a time has been believed in *this* boot.
  bool now(interfaces::WallTime* out) override;

  bool known() const { return known_; }
  // The epoch as it was given, and how long ago: what `status` and the wake record print.
  std::uint32_t givenEpoch() const { return epoch_; }
  std::uint32_t ageMs() const;

  // The bench and the tests: forgotten, as a wake from deep sleep leaves it.
  void forget();

 private:
  bool take(std::uint32_t epoch, std::uint8_t source);

  interfaces::Clock& clock_;

  std::uint32_t epoch_ = 0;
  std::uint64_t taken_ms_ = 0;
  std::uint8_t source_ = 0;  // link::time_source::*, 0 when nothing is known
  bool known_ = false;
};

}  // namespace cycle
}  // namespace core
