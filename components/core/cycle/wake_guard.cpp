#include "core/cycle/wake_guard.h"

namespace core {
namespace cycle {

const char* describe(Guard state) {
  switch (state) {
    case Guard::kDisarmed: return "no wake is being timed";
    case Guard::kArmed: return "a wake is being timed";
    case Guard::kUsbHost: return "a USB host is on the port: awake while a person is here";
    case Guard::kStaying: return "`stay`: awake on purpose, for the bench";
    case Guard::kOverrun: return "the wake overran its bound and a restart was asked for";
  }
  return "?";
}

WakeGuard::WakeGuard(interfaces::Clock& clock, interfaces::Restart& restart,
                     health::Health& health)
    : clock_(clock), restart_(restart), health_(health) {
  refresh();
}

void WakeGuard::arm(std::uint32_t bound_ms) {
  armed_ = true;
  overrun_ = false;
  armed_at_ms_ = clock_.nowMs();
  bound_ms_ = bound_ms != 0 && bound_ms <= kRaisedBoundMax ? bound_ms : kWakeBoundMs;
  refresh();
}

void WakeGuard::allow(std::uint32_t bound_ms) {
  if (!armed_) return;
  const std::uint32_t want = bound_ms > kRaisedBoundMax ? kRaisedBoundMax : bound_ms;
  if (want > bound_ms_) bound_ms_ = want;
  refresh();
}

void WakeGuard::disarm() {
  armed_ = false;
  overrun_ = false;
  bound_ms_ = kWakeBoundMs;
  refresh();
}

void WakeGuard::usbHostIs(bool present) {
  if (present) {
    usb_host_ = true;
    usb_gone_known_ = false;
  } else if (usb_host_) {
    // It was there a moment ago and is not now: the grace period starts here. A host that was
    // never there starts no grace - a Walter that booted on a mast is not inside one.
    usb_host_ = false;
    usb_gone_known_ = true;
    usb_gone_at_ms_ = clock_.nowMs();
  }
  refresh();
}

void WakeGuard::stayAwake(bool on) {
  staying_ = on;
  staying_since_ms_ = on ? clock_.nowMs() : 0;
  refresh();
}

void WakeGuard::refresh() {
  const std::uint64_t now = clock_.nowMs();
  status_.usb_host = usb_host_;
  status_.staying = staying_;
  status_.awake_ms = armed_ ? static_cast<std::uint32_t>(now - armed_at_ms_) : 0;
  status_.usb_gone_for_ms =
      usb_gone_known_ ? static_cast<std::uint32_t>(now - usb_gone_at_ms_) : 0;

  // `stay` runs out on its own: a person who forgets `stay off` loses the rest of the day and
  // not the unit.
  if (staying_) {
    const std::uint64_t staying_for = now - staying_since_ms_;
    if (staying_for >= kStayBoundMs) {
      staying_ = false;
      status_.staying = false;
      status_.stay_left_ms = 0;
    } else {
      status_.stay_left_ms = static_cast<std::uint32_t>(kStayBoundMs - staying_for);
    }
  } else {
    status_.stay_left_ms = 0;
  }

  const bool person_here =
      usb_host_ || (usb_gone_known_ && now - usb_gone_at_ms_ < kUsbGraceMs);
  if (overrun_) {
    status_.state = Guard::kOverrun;
  } else if (person_here) {
    status_.state = Guard::kUsbHost;
  } else if (staying_) {
    status_.state = Guard::kStaying;
  } else if (armed_) {
    status_.state = Guard::kArmed;
  } else {
    status_.state = Guard::kDisarmed;
  }
  status_.bound_ms = bound_ms_;
  status_.left_ms = status_.state == Guard::kArmed && status_.awake_ms < bound_ms_
                        ? bound_ms_ - status_.awake_ms
                        : 0;
}

bool WakeGuard::tick() {
  refresh();
  if (status_.state != Guard::kArmed) return false;
  if (status_.awake_ms < bound_ms_) return false;
  // The bound has passed with nothing holding it off. Record who asked before asking, so that
  // the next boot can say it even if the restart is instant - which on a board it is.
  overrun_ = true;
  ++status_.overruns;
  health_.noteRestart(health::Restart::kWakeOverrun);
  refresh();
  restart_.restart();
  return true;
}

}  // namespace cycle
}  // namespace core
