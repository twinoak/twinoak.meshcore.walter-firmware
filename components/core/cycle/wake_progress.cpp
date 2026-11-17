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

WakeProgress::WakeProgress(interfaces::Retained& retained, interfaces::Clock& clock)
    : retained_(retained), clock_(clock) {}

void WakeProgress::begin() {
  // Idempotent: main/ begins the duty cycle once a boot and the bench's constructor may have
  // done it already (core/cycle/duty_cycle.h), and a second read would take this wake's own
  // record for the wake before's.
  if (begun_) return;
  before_ = WakeWhere{};
  std::uint8_t bytes[kProgressBytes] = {};
  if (retained_.load(interfaces::RetainedSlot::kWakeProgress, bytes, sizeof bytes)) {
    const std::uint8_t flags = bytes[kFlagsAt];
    before_.said = true;
    before_.reached_sleep = (flags & kProgressReachedSleep) != 0;
    before_.in_job = (flags & kProgressInJob) != 0;
    before_.on_modem = (flags & kProgressOnModem) != 0;
    before_.on_link = (flags & kProgressOnLink) != 0;
    before_.step = bytes[kStepAt];
    before_.doing = bytes[kDoingAt] < static_cast<std::uint8_t>(Doing::kCount)
                        ? static_cast<Doing>(bytes[kDoingAt])
                        : Doing::kNothing;
    before_.requests = bytes[kRequestsAt];
    before_.since_s = get16(bytes + kSinceAt);
    before_.seen_s = get16(bytes + kSeenAt);
    std::memcpy(before_.what, bytes + kWhatAt, kProgressWhatBytes);
    before_.what[kProgressWhatBytes] = 0;
  }
  // And this wake's own, from nothing: the record is one line and the wake before has just been
  // read out of it.
  now_ = WakeWhere{};
  now_.said = true;
  began_ms_ = clock_.nowMs();
  begun_ = true;
  writeOut();
}

std::uint16_t WakeProgress::second() const {
  const std::uint64_t now = clock_.nowMs();
  const std::uint64_t s = now > began_ms_ ? (now - began_ms_) / 1000u : 0u;
  return static_cast<std::uint16_t>(s > kProgressAtMostS ? kProgressAtMostS : s);
}

void WakeProgress::cropName(const char* name, char* out) {
  for (std::size_t i = 0; i <= kProgressWhatBytes; ++i) out[i] = 0;
  if (name == nullptr) return;
  for (std::size_t i = 0; i < kProgressWhatBytes && name[i] != 0; ++i) out[i] = name[i];
}

bool WakeProgress::sameName(const char* name) const {
  char want[kProgressWhatBytes + 1];
  cropName(name, want);
  return std::memcmp(want, now_.what, sizeof want) == 0;
}

bool WakeProgress::nameIs(const char* name) {
  char want[kProgressWhatBytes + 1];
  cropName(name, want);
  if (std::memcmp(want, now_.what, sizeof want) == 0) return false;
  std::memcpy(now_.what, want, sizeof want);
  return true;
}

void WakeProgress::atStep(std::uint8_t step) {
  if (!begun_) return;
  // A step that is beginning is where this wake is; what the step before was doing is over.
  const bool moved = now_.step != step || now_.doing != Doing::kStep || now_.in_job ||
                     now_.on_modem || now_.on_link;
  const bool named = nameIs(nullptr);
  if (!moved && !named) {
    stillAwake();
    return;
  }
  now_.step = step;
  now_.doing = Doing::kStep;
  now_.in_job = false;
  now_.on_modem = false;
  now_.on_link = false;
  now_.requests = 0;
  now_.since_s = second();
  writeOut();
}

void WakeProgress::callIsOut(const char* name) {
  if (!begun_) return;
  const bool named = nameIs(name);
  if (!named && now_.doing == Doing::kCall && now_.on_modem) {
    stillAwake();
    return;
  }
  now_.doing = Doing::kCall;
  now_.on_modem = true;
  now_.since_s = second();
  writeOut();
}

void WakeProgress::callCameBack() {
  if (!begun_ || now_.doing != Doing::kCall) return;
  // **A call made inside a claimed job leaves the job reading as the job.** The flag was never
  // cleared, and the type and the second the job began at are put back from the copy this object
  // keeps outside the record - so a `status` job that posts a result halfway through still says
  // `status`, and says how long it has been running and not how long ago its result came back.
  now_.on_modem = false;
  if (now_.in_job) {
    now_.doing = Doing::kJob;
    (void)nameIs(job_what_);
    now_.since_s = job_since_s_;
  } else {
    now_.doing = Doing::kStep;
    (void)nameIs(nullptr);
    now_.since_s = second();
  }
  writeOut();
}

void WakeProgress::jobIsRunning(const char* type, std::uint32_t requests) {
  if (!begun_) return;
  const std::uint8_t made = requests > 0xFFu ? 0xFFu : static_cast<std::uint8_t>(requests);
  // The job itself is what began; a request counted inside it does not move the moment it began
  // at, and neither does a call that went out and came back (see above).
  const bool began = !now_.in_job || now_.doing != Doing::kJob || !sameName(type);
  const bool named = nameIs(type);
  if (!began && !named && now_.requests == made) {
    stillAwake();
    return;
  }
  now_.in_job = true;
  now_.doing = Doing::kJob;
  now_.on_modem = false;
  now_.requests = made;
  if (began) now_.since_s = second();
  std::memcpy(job_what_, now_.what, sizeof job_what_);
  job_since_s_ = now_.since_s;
  writeOut();
}

void WakeProgress::jobEnded() {
  if (!begun_ || !now_.in_job) return;
  now_.in_job = false;
  now_.doing = Doing::kStep;
  now_.on_link = false;
  now_.requests = 0;
  (void)nameIs(nullptr);
  job_what_[0] = 0;
  now_.since_s = second();
  writeOut();
}

void WakeProgress::waitingOnLink(bool waiting) {
  if (!begun_ || now_.on_link == waiting) return;
  now_.on_link = waiting;
  writeOut();
}

void WakeProgress::stillAwake() {
  if (!begun_) return;
  const std::uint16_t s = second();
  if (s == now_.seen_s) return;  // the second has not moved: nothing is written
  writeOut();
}

void WakeProgress::reachedSleep() {
  if (!begun_ || now_.reached_sleep) return;
  now_.reached_sleep = true;
  writeOut();
}

void WakeProgress::sleepRefused() {
  if (!begun_ || !now_.reached_sleep) return;
  now_.reached_sleep = false;
  writeOut();
}

void WakeProgress::writeOut() {
  now_.seen_s = second();
  std::uint8_t bytes[kProgressBytes] = {};
  std::uint8_t flags = 0;
  if (now_.reached_sleep) flags = static_cast<std::uint8_t>(flags | kProgressReachedSleep);
  if (now_.in_job) flags = static_cast<std::uint8_t>(flags | kProgressInJob);
  if (now_.on_modem) flags = static_cast<std::uint8_t>(flags | kProgressOnModem);
  if (now_.on_link) flags = static_cast<std::uint8_t>(flags | kProgressOnLink);
  bytes[kFlagsAt] = flags;
  bytes[kStepAt] = now_.step;
  bytes[kDoingAt] = static_cast<std::uint8_t>(now_.doing);
  bytes[kRequestsAt] = now_.requests;
  put16(bytes + kSinceAt, now_.since_s);
  put16(bytes + kSeenAt, now_.seen_s);
  std::memcpy(bytes + kWhatAt, now_.what, kProgressWhatBytes);
  (void)retained_.store(interfaces::RetainedSlot::kWakeProgress, bytes, sizeof bytes);
}

void WakeProgress::forget() {
  before_ = WakeWhere{};
  now_ = WakeWhere{};
  begun_ = false;
  began_ms_ = clock_.nowMs();
  job_what_[0] = 0;
  job_since_s_ = 0;
  std::uint8_t bytes[kProgressBytes] = {};
  // A record of all zeroes reads as *written down, reached no sleep, nothing recorded*, which
  // would be a word about a wake nobody remembers. So the slot is left saying *it reached sleep*:
  // forgetting is not a wake that was ended.
  bytes[kFlagsAt] = kProgressReachedSleep;
  (void)retained_.store(interfaces::RetainedSlot::kWakeProgress, bytes, sizeof bytes);
}

}  // namespace cycle
}  // namespace core
