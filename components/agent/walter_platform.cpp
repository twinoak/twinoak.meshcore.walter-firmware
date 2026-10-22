#include "walter_agent/walter_platform.h"

#include "core/link/codec/wire.h"

namespace agent {

namespace {

// The one platform whose sink `mm::logSetSink` was given. There is one on a board and one in a
// test at a time; a platform constructed later takes it, which is what a test wants.
WalterPlatform* g_log_owner = nullptr;

}  // namespace

WalterPlatform* logOwner() { return g_log_owner; }

// -- the clock ---------------------------------------------------------------------------------

bool WalterClock::hasTime() const { return wall_.known(); }

std::uint32_t WalterClock::now() const {
  interfaces::WallTime time;
  // `wall_` is a reference member, so a const method of this object may still call a non-const
  // method of the object it refers to - and NetworkClock::now() is non-const because it reads
  // the millisecond clock.
  if (!wall_.now(&time)) return 0;
  return time.epoch;
}

bool WalterClock::fromNetwork() const { return wall_.known(); }

const char* WalterClock::sourceName() const {
  if (!wall_.known()) return "none";
  return wall_.source() == core::link::time_source::kServer ? "server" : "network";
}

bool WalterClock::takeServerTime(std::uint32_t serverEpoch, std::int32_t& difference) {
  ++offered_;
  // Server minus local, **before** any change, for the library's log line.
  const std::uint32_t local = now();
  difference = local == 0 ? 0
                          : static_cast<std::int32_t>(static_cast<std::int64_t>(serverEpoch) -
                                                      static_cast<std::int64_t>(local));
  if (!wall_.giveFromServer(serverEpoch)) return false;
  ++taken_;
  return true;
}

// -- the platform ------------------------------------------------------------------------------

WalterPlatform::WalterPlatform(interfaces::Clock& clock, core::cycle::NetworkClock& wall,
                               interfaces::Entropy& entropy, interfaces::Heap& heap,
                               core::cycle::AgentAttention* attention)
    : clock_(clock), entropy_(entropy), heap_(heap), attention_(attention), clock_out_(wall) {}

WalterPlatform::~WalterPlatform() {
  if (g_log_owner != this) return;
  mm::logSetSink(nullptr);
  mm::logBegin(nullptr, nullptr);
  g_log_owner = nullptr;
}

void WalterPlatform::beginLog(void (*sink)(const char* line), mm::LogLevel level) {
  sink_ = sink;
  g_log_owner = this;
  // The two the library stamps a line with: UNIX seconds once this unit knows them, and the
  // uptime before that ("[+123s]"). Both are free functions because that is what logBegin takes.
  mm::logBegin(&logEpoch, &logUptimeMs);
  mm::logSetLevel(level);
  mm::logSetSink(sink == nullptr ? nullptr : &WalterPlatform::logSink);
}

std::uint32_t WalterPlatform::logEpoch() {
  WalterPlatform* owner = g_log_owner;
  return owner == nullptr ? 0 : owner->clock_out_.now();
}

std::uint32_t WalterPlatform::logUptimeMs() {
  WalterPlatform* owner = g_log_owner;
  return owner == nullptr ? 0 : owner->millis();
}

void WalterPlatform::logSink(const char* line, std::size_t len) {
  (void)len;  // the line is NUL-terminated as well (agent/Log.cpp's snprintf)
  WalterPlatform* owner = g_log_owner;
  if (owner == nullptr || owner->sink_ == nullptr || line == nullptr) return;
  ++owner->log_lines_;
  owner->sink_(line);
}

// The library's millisecond clock wraps every 49.7 days and every comparison it makes is a
// signed difference, which is right across the wrap (agent/IPlatform.h). interfaces::Clock
// counts in 64 bits and restarts at zero after a deep sleep; the low 32 bits of it are exactly
// what the library asks for.
std::uint32_t WalterPlatform::millis() {
  return static_cast<std::uint32_t>(clock_.nowMs() & 0xFFFFFFFFu);
}

// A yield and never a busy spin: the library calls this with 1 in the idle loop inside a job's
// reply wait. The pump is what must not stop while the agent waits - the link client's receive
// side and the console's byte - and it runs **before** the delay, so a wait of one millisecond
// still pumps once.
void WalterPlatform::delayMs(std::uint32_t ms) {
  ++delays_;
  if (pump_ != nullptr) {
    ++pumps_;
    pump_();
  }
  clock_.delayMs(ms);
}

std::uint32_t WalterPlatform::random() {
  ++randoms_;
  return entropy_.bits32();
}

// The one drastic thing the library may ask a board to do to itself, and this firmware does not
// do it: the header says why. Journalled, raised for the record, and then the board is left to
// the bound on the wake, which is the thing that owns restarts.
void WalterPlatform::rebootForHealth(const char* why) {
  ++restarts_refused_;
  if (attention_ != nullptr) attention_->raiseRebootWanted(why);
  mm::logf(mm::LogLevel::Error,
           "agent: a restart was asked for and this firmware does not restart itself for the "
           "server's sake (%s); the bound on this wake ends it",
           why == nullptr ? "no reason given" : why);
  // Not a spin and not a reboot: the wake guard's bound is what ends a wake that cannot finish
  // (core/cycle/wake_guard.h, armed by main/ at every boot), and a person on the console has
  // `reboot`. The method is [[noreturn]] and this is the one honest way to honour that without
  // taking a decision that is not this object's to take.
  for (;;) {
    clock_.delayMs(kRefusedRestartYieldMs);
  }
}

}  // namespace agent
