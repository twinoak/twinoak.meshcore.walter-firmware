#include "core/cycle/wake_progress.h"

#include <cstring>

namespace core {
namespace cycle {

namespace {

constexpr std::size_t kFlagsAt = 0;
constexpr std::size_t kStepAt = 1;
constexpr std::size_t kDoingAt = 2;
constexpr std::size_t kRequestsAt = 3;
constexpr std::size_t kSinceAt = 4;
constexpr std::size_t kSeenAt = 6;
constexpr std::size_t kWhatAt = 8;

void put16(std::uint8_t* at, std::uint16_t v) {
  at[0] = static_cast<std::uint8_t>(v & 0xFFu);
  at[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

std::uint16_t get16(const std::uint8_t* at) {
  return static_cast<std::uint16_t>(at[0] | (static_cast<std::uint16_t>(at[1]) << 8));
}

}  // namespace

const char* describe(Doing doing) {
  switch (doing) {
    case Doing::kNothing: return "nothing was written down";
    case Doing::kStep: return "the step, and nothing finer";
    case Doing::kCall: return "a call to the server was out";
    case Doing::kJob: return "a claimed job was being run";
    case Doing::kCount: break;
  }
  return "nothing was written down";
}

// -- the record's bytes ------------------------------------------------------------------------

void WakeProgress::unpack(const std::uint8_t* bytes, WakeWhere* out) {
  const std::uint8_t flags = bytes[kFlagsAt];
  *out = WakeWhere{};
  out->said = true;
  out->reached_sleep = (flags & kProgressReachedSleep) != 0;
  out->in_job = (flags & kProgressInJob) != 0;
  out->on_modem = (flags & kProgressOnModem) != 0;
  out->on_link = (flags & kProgressOnLink) != 0;
  out->step = bytes[kStepAt];
  out->doing = bytes[kDoingAt] < static_cast<std::uint8_t>(Doing::kCount)
                   ? static_cast<Doing>(bytes[kDoingAt])
                   : Doing::kNothing;
  out->requests = bytes[kRequestsAt];
  out->since_s = get16(bytes + kSinceAt);
  out->seen_s = get16(bytes + kSeenAt);
  std::memcpy(out->what, bytes + kWhatAt, kProgressWhatBytes);
  out->what[kProgressWhatBytes] = 0;
}

void WakeProgress::pack(const WakeWhere& where, std::uint8_t* bytes) {
  for (std::size_t i = 0; i < kProgressBytes; ++i) bytes[i] = 0;
  std::uint8_t flags = 0;
  if (where.reached_sleep) flags = static_cast<std::uint8_t>(flags | kProgressReachedSleep);
  if (where.in_job) flags = static_cast<std::uint8_t>(flags | kProgressInJob);
  if (where.on_modem) flags = static_cast<std::uint8_t>(flags | kProgressOnModem);
  if (where.on_link) flags = static_cast<std::uint8_t>(flags | kProgressOnLink);
  bytes[kFlagsAt] = flags;
  bytes[kStepAt] = where.step;
  bytes[kDoingAt] = static_cast<std::uint8_t>(where.doing);
  bytes[kRequestsAt] = where.requests;
  put16(bytes + kSinceAt, where.since_s);
  put16(bytes + kSeenAt, where.seen_s);
  std::memcpy(bytes + kWhatAt, where.what, kProgressWhatBytes);
}

// -- the journal entry (kind::kWakeMidway) -----------------------------------------------------

void encode(std::uint8_t wake, const WakeWhere& where, std::uint8_t out[kMidwayPayloadBytes]) {
  for (std::size_t i = 0; i < kMidwayPayloadBytes; ++i) out[i] = 0;
  out[0] = wake;
  std::uint8_t flags = 0;
  if (where.reached_sleep) flags = static_cast<std::uint8_t>(flags | kProgressReachedSleep);
  if (where.in_job) flags = static_cast<std::uint8_t>(flags | kProgressInJob);
  if (where.on_modem) flags = static_cast<std::uint8_t>(flags | kProgressOnModem);
  if (where.on_link) flags = static_cast<std::uint8_t>(flags | kProgressOnLink);
  out[1] = flags;
  out[2] = where.step;
  out[3] = static_cast<std::uint8_t>(where.doing);
  out[4] = where.requests;
  put16(out + 6, where.since_s);
  put16(out + 8, where.seen_s);
  std::memcpy(out + 10, where.what, kProgressWhatBytes);
}

bool decode(const update::Entry& entry, MidwayPayload* out) {
  if (out == nullptr || entry.kind != update::kind::kWakeMidway) return false;
  if (entry.size != kMidwayPayloadBytes) return false;
  const std::uint8_t* p = entry.payload;
  *out = MidwayPayload{};
  out->wake = p[0];
  out->where.said = true;
  out->where.reached_sleep = (p[1] & kProgressReachedSleep) != 0;
  out->where.in_job = (p[1] & kProgressInJob) != 0;
  out->where.on_modem = (p[1] & kProgressOnModem) != 0;
  out->where.on_link = (p[1] & kProgressOnLink) != 0;
  out->where.step = p[2];
  out->where.doing =
      p[3] < static_cast<std::uint8_t>(Doing::kCount) ? static_cast<Doing>(p[3]) : Doing::kNothing;
  out->where.requests = p[4];
  out->where.since_s = get16(p + 6);
  out->where.seen_s = get16(p + 8);
  std::memcpy(out->where.what, p + 10, kProgressWhatBytes);
  out->where.what[kProgressWhatBytes] = 0;
  return true;
}

// -- the object --------------------------------------------------------------------------------

WakeProgress::WakeProgress(interfaces::Retained& retained, interfaces::Clock& clock)
    : retained_(retained), clock_(clock) {}

void WakeProgress::begin() {
  // Idempotent: main/ begins the duty cycle once a boot and the bench's constructor may have
  // done it already (core/cycle/duty_cycle.h), and a second read would take this wake's own
  // record for the wake before's.
  if (begun_) return;
  before_said_ = retained_.load(interfaces::RetainedSlot::kWakeProgress, before_, sizeof before_);
  // And this wake's own, from nothing: the record is one line and the wake before has just been
  // read out of it.
  began_ms_ = static_cast<std::uint32_t>(clock_.nowMs() & 0xFFFFFFFFu);
  begun_ = true;
  write(WakeWhere{});
}

WakeWhere WakeProgress::before() const {
  WakeWhere out{};
  if (before_said_) unpack(before_, &out);
  return out;
}

WakeWhere WakeProgress::now() const {
  WakeWhere out{};
  (void)read(&out);
  return out;
}

bool WakeProgress::read(WakeWhere* out) const {
  std::uint8_t bytes[kProgressBytes] = {};
  if (!retained_.load(interfaces::RetainedSlot::kWakeProgress, bytes, sizeof bytes)) return false;
  unpack(bytes, out);
  return true;
}

void WakeProgress::write(const WakeWhere& where) {
  WakeWhere out = where;
  out.said = true;
  out.seen_s = second();
  std::uint8_t bytes[kProgressBytes];
  pack(out, bytes);
  (void)retained_.store(interfaces::RetainedSlot::kWakeProgress, bytes, sizeof bytes);
}

std::uint16_t WakeProgress::second() const {
  const std::uint32_t now = static_cast<std::uint32_t>(clock_.nowMs() & 0xFFFFFFFFu);
  const std::uint32_t s = now > began_ms_ ? (now - began_ms_) / 1000u : 0u;
  return static_cast<std::uint16_t>(s > kProgressAtMostS ? kProgressAtMostS : s);
}

void WakeProgress::cropName(const char* name, char* out) {
  for (std::size_t i = 0; i <= kProgressWhatBytes; ++i) out[i] = 0;
  if (name == nullptr) return;
  for (std::size_t i = 0; i < kProgressWhatBytes && name[i] != 0; ++i) out[i] = name[i];
}

bool WakeProgress::nameIs(const char* name, WakeWhere* where) {
  char want[kProgressWhatBytes + 1];
  cropName(name, want);
  if (std::memcmp(want, where->what, sizeof want) == 0) return false;
  std::memcpy(where->what, want, sizeof want);
  return true;
}

// -- as the wake goes --------------------------------------------------------------------------

void WakeProgress::atStep(std::uint8_t step) {
  WakeWhere where{};
  if (!begun_ || !read(&where)) return;
  // A step that is beginning is where this wake is; what the step before was doing is over.
  const bool moved = where.step != step || where.doing != Doing::kStep || where.in_job ||
                     where.on_modem || where.on_link;
  const bool named = nameIs(nullptr, &where);
  if (!moved && !named) {
    stillAwake();
    return;
  }
  where.step = step;
  where.doing = Doing::kStep;
  where.in_job = false;
  where.on_modem = false;
  where.on_link = false;
  where.requests = 0;
  where.since_s = second();
  write(where);
}

void WakeProgress::callIsOut(const char* name) {
  WakeWhere where{};
  if (!begun_ || !read(&where)) return;
  const bool named = nameIs(name, &where);
  if (!named && where.doing == Doing::kCall && where.on_modem) {
    stillAwake();
    return;
  }
  where.doing = Doing::kCall;
  where.on_modem = true;
  where.since_s = second();
  write(where);
}

void WakeProgress::callCameBack() {
  WakeWhere where{};
  if (!begun_ || !read(&where) || where.doing != Doing::kCall) return;
  // **A call made inside a claimed job leaves the job reading as the job.** The flag was never
  // cleared, and the type and the second the job began at are put back from the copy this object
  // keeps outside the record - so a `status` job that posts a result halfway through still says
  // `status`, and says how long it has been running and not how long ago its result came back.
  where.on_modem = false;
  if (where.in_job) {
    where.doing = Doing::kJob;
    (void)nameIs(job_what_, &where);
    where.since_s = job_since_s_;
  } else {
    where.doing = Doing::kStep;
    (void)nameIs(nullptr, &where);
    where.since_s = second();
  }
  write(where);
}

void WakeProgress::jobIsRunning(const char* type, std::uint32_t requests) {
  WakeWhere where{};
  if (!begun_ || !read(&where)) return;
  const std::uint8_t made = requests > 0xFFu ? 0xFFu : static_cast<std::uint8_t>(requests);
  // The job itself is what began; a request counted inside it does not move the moment it began
  // at, and neither does a call that went out and came back (see above).
  char want[kProgressWhatBytes + 1];
  cropName(type, want);
  const bool began = !where.in_job || where.doing != Doing::kJob ||
                     std::memcmp(want, where.what, sizeof want) != 0;
  const bool named = nameIs(type, &where);
  if (!began && !named && where.requests == made) {
    stillAwake();
    return;
  }
  where.in_job = true;
  where.doing = Doing::kJob;
  where.on_modem = false;
  where.requests = made;
  if (began) where.since_s = second();
  std::memcpy(job_what_, where.what, sizeof job_what_);
  job_since_s_ = where.since_s;
  write(where);
}

void WakeProgress::jobEnded() {
  WakeWhere where{};
  if (!begun_ || !read(&where) || !where.in_job) return;
  where.in_job = false;
  where.doing = Doing::kStep;
  where.on_link = false;
  where.requests = 0;
  (void)nameIs(nullptr, &where);
  job_what_[0] = 0;
  where.since_s = second();
  write(where);
}

void WakeProgress::waitingOnLink(bool waiting) {
  WakeWhere where{};
  if (!begun_ || !read(&where) || where.on_link == waiting) return;
  where.on_link = waiting;
  write(where);
}

void WakeProgress::stillAwake() {
  WakeWhere where{};
  if (!begun_ || !read(&where)) return;
  if (second() == where.seen_s) return;  // the second has not moved: nothing is written
  write(where);
}

void WakeProgress::reachedSleep() {
  WakeWhere where{};
  if (!begun_ || !read(&where) || where.reached_sleep) return;
  where.reached_sleep = true;
  write(where);
}

void WakeProgress::sleepRefused() {
  WakeWhere where{};
  if (!begun_ || !read(&where) || !where.reached_sleep) return;
  where.reached_sleep = false;
  write(where);
}

void WakeProgress::forget() {
  for (std::size_t i = 0; i < sizeof before_; ++i) before_[i] = 0;
  before_said_ = false;
  begun_ = false;
  began_ms_ = static_cast<std::uint32_t>(clock_.nowMs() & 0xFFFFFFFFu);
  job_what_[0] = 0;
  job_since_s_ = 0;
  // A record of all zeroes reads as *written down, reached no sleep, nothing recorded*, which
  // would be a word about a wake nobody remembers. So the slot is left saying *it reached sleep*:
  // forgetting is not a wake that was ended.
  WakeWhere blank{};
  blank.reached_sleep = true;
  std::uint8_t bytes[kProgressBytes];
  pack(blank, bytes);
  (void)retained_.store(interfaces::RetainedSlot::kWakeProgress, bytes, sizeof bytes);
}

}  // namespace cycle
}  // namespace core
