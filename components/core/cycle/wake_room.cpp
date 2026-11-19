#include "core/cycle/wake_room.h"

namespace core {
namespace cycle {

std::uint64_t WakeRoom::deadlineFor(std::uint32_t want_ms) const {
  const std::uint64_t now = clock_.nowMs();
  if (!bounded()) return now + want_ms;
  // The most this wake could ever be raised to, counted from the boot as the guard counts: a
  // ceiling that is honest whatever the bound stands at now, because a claim made inside the work
  // may raise it (`tookOn`). A wake already past that ceiling gets nothing, and the call that
  // would be made with it is refused for want of room - which is the next tick's reset either
  // way.
  const std::uint32_t awake = guard_->awakeMs();
  const std::uint32_t ever = awake >= kRaisedBoundMax ? 0 : kRaisedBoundMax - awake;
  return now + (want_ms < ever ? want_ms : ever);
}

std::uint32_t WakeRoom::leftForCallMs(std::uint64_t deadline_ms, bool* bounded_by_the_wake) const {
  if (bounded_by_the_wake != nullptr) *bounded_by_the_wake = false;
  const std::uint64_t now = clock_.nowMs();
  const std::uint64_t to_deadline = deadline_ms > now ? deadline_ms - now : 0;
  if (!bounded()) {
    return deadline_ms == 0 ? kNoLimitMs : static_cast<std::uint32_t>(to_deadline);
  }
  const std::uint32_t to_bound = leftMs();
  if (deadline_ms == 0 || to_bound <= to_deadline) {
    if (bounded_by_the_wake != nullptr) *bounded_by_the_wake = true;
    return to_bound;
  }
  return static_cast<std::uint32_t>(to_deadline);
}

Cover WakeRoom::tookOn(int jobs, std::uint32_t latest_lease_s, std::uint32_t now_epoch) const {
  Cover cover;
  cover.jobs = jobs > 255 ? 255u : (jobs > 0 ? static_cast<std::uint8_t>(jobs) : 0u);
  cover.room_ms = roomForCallsAfter(jobs);
  if (jobs <= 0 || !bounded()) return cover;  // nothing was taken on, or nothing ends this wake
  cover.bound_ms = guard_->bound();
  // **No lease and no clock are the same answer**: there is nothing to cover and nothing to cut
  // against, so the wake keeps the bound it had. A job with no lease then runs under the ordinary
  // bound and the rail is what ends a wake it outlasts, exactly as before this task.
  if (latest_lease_s == 0 || now_epoch == 0) return cover;

  const std::uint32_t lease_in_s = latest_lease_s > now_epoch ? latest_lease_s - now_epoch : 0;
  // The guard counts from the boot and a lease from now, so what the wake has already spent is
  // asked for too (`core/cycle/wake_guard.h`, `awakeMs`).
  const std::uint64_t want = static_cast<std::uint64_t>(guard_->awakeMs()) +
                             static_cast<std::uint64_t>(lease_in_s) * 1000u + cover.room_ms;
  guard_->allow(want > kRaisedBoundMax ? kRaisedBoundMax : static_cast<std::uint32_t>(want));
  cover.bound_ms = guard_->bound();

  const std::uint32_t left = leftMs();
  cover.covered_s = left / 1000u;
  // Where the room for the calls after the jobs begins, which is where a lease has to end. Down
  // to the second, so that a lease cut to it has run out when the library next looks: `now_epoch`
  // itself is a job the library refuses at no transaction, which is the honest answer for a wake
  // with no room left in it.
  const std::uint32_t for_jobs = left > cover.room_ms ? left - cover.room_ms : 0;
  cover.lease_until_s = now_epoch + for_jobs / 1000u;
  cover.cut = latest_lease_s > cover.lease_until_s;
  return cover;
}

}  // namespace cycle
}  // namespace core
