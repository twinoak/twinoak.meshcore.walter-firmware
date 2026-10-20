#include "core/supervisor/supervisor.h"

#include "core/link/codec/messages.h"
#include "core/link/codec/wire.h"

namespace core {
namespace supervisor {

namespace {

std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::uint8_t cap8(std::uint32_t v) { return v > 255 ? 255 : static_cast<std::uint8_t>(v); }

std::uint32_t low32(std::uint64_t v) { return static_cast<std::uint32_t>(v & 0xFFFFFFFFull); }

// What incidentAt() and attentionAt() answer for an index that is not there: a record that says
// it is not present. Constants, not function statics, so there is no guard variable anywhere in
// core/ (host_test/core/supervisor/test_supervisor_no_allocation.cpp holds that).
const Incident kNoIncident;
const Attention kNoAttention;

}  // namespace

// ------------------------------------------------------------------------------- the words

const char* describe(Rung rung) {
  switch (rung) {
    case Rung::kNone: return "no rung";
    case Rung::kAskAgain: return "rung 1, ask again later";
    case Rung::kReset: return "rung 2, a reset pulse";
    case Rung::kPowerCycle: return "rung 3, a power cycle";
    case Rung::kGoldenImage: return "rung 4, what is on the chip";
    case Rung::kStop: return "rung 5, stop";
  }
  return "?";
}

const char* describe(Silence silence) {
  switch (silence) {
    case Silence::kNone: return "the radio answered";
    case Silence::kScanner: return "the scanner is running: not a fault";
    case Silence::kWalterPoweredUp: return "this Walter has just powered up: the radio is still booting";
    case Silence::kUpdateRunning: return "an update is in progress: not ours to disturb";
    case Silence::kNotAnswering: return "the radio is not answering";
    case Silence::kAwaitingFirstInstall:
      return "the radio awaits its first install after rak provision: silent by design, left alone";
  }
  return "?";
}

const char* describe(Cause cause) {
  switch (cause) {
    case Cause::kNone: return "nothing";
    case Cause::kSilence: return "the radio went quiet";
    case Cause::kBootLoop: return "the radio is restarting itself";
  }
  return "?";
}

const char* describe(RungOutcome outcome) {
  switch (outcome) {
    case RungOutcome::kNotTried: return "not tried";
    case RungOutcome::kDone: return "done; the radio did not come back";
    case RungOutcome::kCured: return "done; the radio came back";
    case RungOutcome::kIntact: return "the chip holds the golden image: nothing to flash";
    case RungOutcome::kRefusedCap: return "refused: today's cap is reached";
    case RungOutcome::kRefusedBattery: return "refused: the battery is too low";
    case RungOutcome::kFailed: return "it could not be done";
    case RungOutcome::kSkipped: return "skipped";
  }
  return "?";
}

const char* describe(Outcome outcome) {
  switch (outcome) {
    case Outcome::kRunning: return "climbing";
    case Outcome::kCured: return "cured";
    case Outcome::kStopped: return "stopped";
    case Outcome::kRefused: return "refused: nothing left that may be tried";
  }
  return "?";
}

const char* describe(StopReason reason) {
  switch (reason) {
    case StopReason::kNone: return "no reason given";
    case StopReason::kEverythingTried: return "the radio is unresponsive after everything that was tried";
    case StopReason::kFirmwareIntact: return "the chip holds the golden image: this is not a firmware fault";
    case StopReason::kChipProtected: return "the chip is protected: opening one is a person's bench procedure";
    case StopReason::kChipUnreadable: return "the chip could not be read at all";
    case StopReason::kFlashFailed: return "the golden image could not be put back";
    case StopReason::kNoGoldenImage: return "there is no golden image to put back";
    case StopReason::kCapReached: return "today's cap is reached for every rung that is left";
    case StopReason::kBatteryTooLow: return "the battery is too low for the rungs that are left";
    case StopReason::kNoStore: return "this build has no image store: the chip cannot be checked";
  }
  return "?";
}

const char* describe(State state) {
  switch (state) {
    case State::kIdle: return "idle";
    case State::kClassify: return "working out what the silence means";
    case State::kClimbing: return "climbing";
    case State::kWaiting: return "waiting after a rung";
    case State::kJudging: return "asking again after a rung";
    case State::kFlashing: return "flashing the golden image";
    case State::kStopped: return "stopped";
  }
  return "?";
}

const char* describe(Health health) {
  switch (health) {
    case Health::kUnknown: return "unknown";
    case Health::kHealthy: return "healthy";
    case Health::kScanner: return "the scanner (NoiseScope) is running";
    case Health::kSilent: return "silent";
    case Health::kAwaitingFirstInstall:
      return "awaiting its first install (provisioned; 'rak update --first-install' is next)";
  }
  return "?";
}

const char* describe(Wrote what) {
  switch (what) {
    case Wrote::kNone: return "nothing";
    case Wrote::kIncident: return "an incident begun";
    case Wrote::kRung: return "a rung";
    case Wrote::kJudged: return "the radio asked again";
    case Wrote::kOutcome: return "the incident ended";
    case Wrote::kStopped: return "stopped";
    case Wrote::kAttention: return "an attention from the radio";
    case Wrote::kCapsReset: return "the day's counts emptied by hand";
    case Wrote::kQuietOver: return "the quiet period passed";
    case Wrote::kAwaitingFirstInstall: return "rak provision: the slot awaits its first install";
    case Wrote::kFirstInstallDone: return "the first install committed: the slot no longer awaits it";
  }
  return "?";
}

// ------------------------------------------------------------------------------- the payload

void encode(const Payload& p, std::uint8_t out[kPayloadBytes]) {
  for (std::size_t i = 0; i < kPayloadBytes; ++i) out[i] = 0;
  out[0] = static_cast<std::uint8_t>(p.what);
  out[1] = static_cast<std::uint8_t>(p.rung);
  out[2] = static_cast<std::uint8_t>(p.cause);
  out[3] = p.outcome;
  out[4] = cap8(p.counts.resets);
  out[5] = cap8(p.counts.power_cycles);
  out[6] = cap8(p.counts.flashes);
  out[7] = p.flags;
  put32(out + 8, p.window_epoch);
  put32(out + 12, p.window_ms);
  put32(out + 16, p.at_epoch);
  put32(out + 20, p.at_ms);
  out[24] = static_cast<std::uint8_t>(p.battery_mv);
  out[25] = static_cast<std::uint8_t>(p.battery_mv >> 8);
  out[26] = p.reset_reason;
  out[27] = p.restarts;
  put32(out + 28, p.uptime_s);
  out[32] = p.rungs_tried;
  out[33] = static_cast<std::uint8_t>(p.silence);
  out[34] = static_cast<std::uint8_t>(p.health);
  put32(out + 36, p.detail);
}

bool decode(const update::Entry& e, Payload* out) {
  if (e.kind != update::kind::kSupervisor || e.size < kPayloadBytes || out == nullptr) return false;
  if (e.payload[0] > static_cast<std::uint8_t>(Wrote::kFirstInstallDone)) return false;
  if (e.payload[1] > static_cast<std::uint8_t>(Rung::kStop)) return false;
  if (e.payload[2] > static_cast<std::uint8_t>(Cause::kBootLoop)) return false;
  out->what = static_cast<Wrote>(e.payload[0]);
  out->rung = static_cast<Rung>(e.payload[1]);
  out->cause = static_cast<Cause>(e.payload[2]);
  out->outcome = e.payload[3];
  out->counts.resets = e.payload[4];
  out->counts.power_cycles = e.payload[5];
  out->counts.flashes = e.payload[6];
  out->flags = e.payload[7];
  out->window_epoch = get32(e.payload + 8);
  out->window_ms = get32(e.payload + 12);
  out->at_epoch = get32(e.payload + 16);
  out->at_ms = get32(e.payload + 20);
  out->battery_mv = static_cast<std::uint16_t>(e.payload[24] | (e.payload[25] << 8));
  out->reset_reason = e.payload[26];
  out->restarts = e.payload[27];
  out->uptime_s = get32(e.payload + 28);
  out->rungs_tried = e.payload[32];
  out->silence = e.payload[33] <= static_cast<std::uint8_t>(Silence::kAwaitingFirstInstall)
                     ? static_cast<Silence>(e.payload[33])
                     : Silence::kNone;
  out->health = e.payload[34] <= static_cast<std::uint8_t>(Health::kAwaitingFirstInstall)
                    ? static_cast<Health>(e.payload[34])
                    : Health::kUnknown;
  out->detail = get32(e.payload + 36);
  return true;
}

// ------------------------------------------------------------------------------- the object

Supervisor::Supervisor(link::client::Client& radio, board::RadioPins& pins, board::SlotLock& slots,
                       health::Health& health, update::BatterySource& battery,
                       interfaces::Clock& clock, interfaces::WallClock& wall,
                       interfaces::Retained& retained, image::Store* store,
                       flasher::Flasher* flasher, update::RadioUpdate* update,
                       update::Journal* journal, board::Slot slot)
    : radio_(radio), pins_(pins), slots_(slots), health_(health), battery_(battery), clock_(clock),
      wall_(wall), retained_(retained), store_(store), flasher_(flasher), update_(update),
      journal_(journal), slot_(slot) {
  radio_.subscribeRestart(this);
  status_.slot = slot;
}

void Supervisor::onRadioRestart(std::uint32_t boot_id, std::uint8_t reset_reason) {
  // Called from inside a client call (an EV_BOOT read, or a HELLO with a boot_id we had not
  // seen: §5.1). It records; nothing here talks on the link.
  restart_seen_ = true;
  boot_id_ = boot_id;
  boot_id_known_ = true;
  if (reset_reason != link::reset_reason::kUnknown) reset_reason_ = reset_reason;
  const std::uint64_t now = clock_.nowMs();
  // The boot-loop window: a sliding count of restarts inside kBootLoopWindowMs. A clock that
  // restarted (a deep sleep) is a new epoch, so the window begins again - and the count with it,
  // because the restarts it held cannot be placed in time any more.
  if (restarts_ == 0 || now < restart_window_ms_ || now - restart_window_ms_ > kBootLoopWindowMs) {
    restart_window_ms_ = now;
    restarts_ = 0;
  }
  ++restarts_;
  ++restarts_total_;
  // The clock is pending again (§5.4): the next question sets it.
}

// ------------------------------------------------------------------- retained storage and flash

void Supervisor::loadRecord() {
  Record r{};
  if (retained_.load(interfaces::RetainedSlot::kSupervisor, &r, sizeof r)) {
    window_epoch_ = r.window_epoch;
    window_ms_ = r.window_ms;
    counts_.resets = r.resets;
    counts_.power_cycles = r.power_cycles;
    counts_.flashes = r.flashes;
    window_open_ = counts_.resets != 0 || counts_.power_cycles != 0 || counts_.flashes != 0;
    stopped_ = r.stopped != 0;
    stop_reason_ = static_cast<StopReason>(r.stop_reason);
    stopped_after_ = r.stopped_after;
    stopped_ms_ = r.stopped_ms;
    restarts_ = r.restarts;
    restarts_total_ = r.restarts_total;
    restart_window_ms_ = r.restart_window_ms;
    awaiting_first_install_ = r.awaiting_first_install != 0;
    awaiting_since_seq_ = r.awaiting_since_seq;
    return;
  }
  // No retained record: a power cut, or the first boot. Flash is what is left (the journal's
  // entries each carry the counts after them), and a cap is never forgiven by a power cut.
  Payload p;
  if (!lastEntry(&p)) return;
  // The mark too (task p5-03): every entry carries it, and the entry that made it has the seq
  // a first install must be newer than.
  if ((p.flags & kFlagAwaitingFirstInstall) != 0) {
    awaiting_first_install_ = true;
    awaiting_since_seq_ = lastMarkSeq();
  }
  counts_.resets = p.counts.resets;
  counts_.power_cycles = p.counts.power_cycles;
  counts_.flashes = p.counts.flashes;
  window_epoch_ = p.window_epoch;
  window_open_ = counts_.resets != 0 || counts_.power_cycles != 0 || counts_.flashes != 0;
  // The monotonic time the window began belongs to a boot that is over: re-anchored at now, and
  // the counts kept. With an epoch in the entry, rollWindow() can still tell a whole day.
  window_ms_ = clock_.nowMs();
  status_.counts_from_journal = window_open_;
  if (p.what == Wrote::kStopped) {
    stopped_ = true;
    stop_reason_ = static_cast<StopReason>(p.outcome);
    stopped_after_ = p.rungs_tried;
    stopped_ms_ = clock_.nowMs();  // the quiet period begins again: the age cannot be measured
  }
}

void Supervisor::storeRecord() {
  Record r{};
  r.window_epoch = window_epoch_;
  r.window_ms = window_ms_;
  r.resets = counts_.resets;
  r.power_cycles = counts_.power_cycles;
  r.flashes = counts_.flashes;
  r.stopped = stopped_ ? 1 : 0;
  r.stop_reason = static_cast<std::uint8_t>(stop_reason_);
  r.stopped_after = stopped_after_;
  r.restarts = cap8(restarts_);
  r.stopped_ms = stopped_ms_;
  r.restart_window_ms = restart_window_ms_;
  r.restarts_total = restarts_total_;
  r.awaiting_first_install = awaiting_first_install_ ? 1 : 0;
  r.awaiting_since_seq = awaiting_since_seq_;
  (void)retained_.store(interfaces::RetainedSlot::kSupervisor, &r, sizeof r);
}

// The newest entry that marked the slot as awaiting its first install, for its seq; 0 when the
// ring no longer holds one (the mark is then held to the newest entry's flag alone, and any
// committed first install the ring still holds is older than that entry and does not clear it,
// which is the conservative reading).
std::uint32_t Supervisor::lastMarkSeq() const {
  if (journal_ == nullptr) return 0;
  update::Cursor cursor;
  update::Entry e;
  bool more = journal_->first(&cursor, &e);
  while (more) {
    Payload p;
    if (decode(e, &p) && p.what == Wrote::kAwaitingFirstInstall) return e.seq;
    more = journal_->previous(&cursor, &e);
  }
  return 0;
}

// `rak update --first-install` committed, after the mark: the journal's newest radio-update entry
// is a first install that ended `done`, and it is newer than the entry that made the mark. Only
// that clears it (and a new `rak provision` sets it again).
void Supervisor::reconcileFirstInstall() {
  if (!awaiting_first_install_ || journal_ == nullptr) return;
  update::Entry e;
  update::Payload up;
  if (!journal_->lastOf(update::kind::kRadioUpdate, &e) || !update::decode(e, &up)) return;
  if (up.kind != update::Kind::kFirstInstall || up.state != update::State::kDone) return;
  if (e.seq <= awaiting_since_seq_) return;
  awaiting_first_install_ = false;
  awaiting_since_seq_ = 0;
  if (status_.health == Health::kAwaitingFirstInstall) status_.health = Health::kUnknown;
  storeRecord();
  Payload p = payloadNow(Wrote::kFirstInstallDone);
  p.detail = e.seq;
  (void)journalEntry(Wrote::kFirstInstallDone, p);
}

void Supervisor::markAwaitingFirstInstall() {
  awaiting_first_install_ = true;
  awaiting_since_seq_ = 0;
  // A stop in force was about the radio that is being erased: lifted, as reset-caps lifts it.
  // The counts stay - the day's caps are about the battery, which is the same battery.
  if (stopped_) {
    stopped_ = false;
    stop_reason_ = StopReason::kNone;
    stopped_after_ = 0;
    stopped_ms_ = 0;
    if (state_ == State::kStopped) state_ = State::kIdle;
  }
  by_hand_ = true;
  std::uint32_t seq = 0;
  if (journalEntry(Wrote::kAwaitingFirstInstall, payloadNow(Wrote::kAwaitingFirstInstall), &seq)) {
    awaiting_since_seq_ = seq;
  }
  by_hand_ = false;
  storeRecord();
  refreshStatus();
}

void Supervisor::loadIncidents() {
  IncidentRecord r{};
  if (!retained_.load(interfaces::RetainedSlot::kSupervisorIncidents, &r, sizeof r)) return;
  incident_count_ = r.count <= kIncidentsKept ? r.count : kIncidentsKept;
  for (std::size_t i = 0; i < incident_count_; ++i) incidents_[i] = r.items[i];
}

void Supervisor::storeIncidents() {
  IncidentRecord r{};
  r.count = static_cast<std::uint8_t>(incident_count_);
  for (std::size_t i = 0; i < incident_count_; ++i) r.items[i] = incidents_[i];
  (void)retained_.store(interfaces::RetainedSlot::kSupervisorIncidents, &r, sizeof r);
}

bool Supervisor::lastEntry(Payload* out) const {
  if (journal_ == nullptr) return false;
  update::Entry e;
  if (!journal_->lastOf(update::kind::kSupervisor, &e)) return false;
  return decode(e, out);
}

Payload Supervisor::payloadNow(Wrote what) {
  Payload p;
  p.what = what;
  p.cause = cause_;
  p.counts = counts_;
  p.window_epoch = window_epoch_;
  p.window_ms = low32(window_ms_);
  interfaces::WallTime wt;
  if (wall_.now(&wt)) p.at_epoch = wt.epoch;
  p.at_ms = low32(clock_.nowMs());
  p.battery_mv = battery_mv_;
  p.reset_reason = reset_reason_;
  p.restarts = cap8(restarts_);
  p.uptime_s = last_uptime_s_;
  p.silence = status_.silence_seen;
  p.health = status_.health;
  if (battery_known_) p.flags |= kFlagBatteryKnown;
  if (by_hand_) p.flags |= kFlagByHand;
  if (status_.window_reanchored) p.flags |= kFlagReanchored;
  if (cause_ == Cause::kBootLoop) p.flags |= kFlagBootLoop;
  if (awaiting_first_install_) p.flags |= kFlagAwaitingFirstInstall;
  const Incident* in = nullptr;
  if (incident_count_ > 0 && incidents_[0].outcome == Outcome::kRunning) in = &incidents_[0];
  if (in != nullptr) p.rungs_tried = in->rungs_tried;
  return p;
}

bool Supervisor::journalEntry(Wrote what, const Payload& about, std::uint32_t* out_seq) {
  if (journal_ == nullptr) return false;
  std::uint8_t bytes[kPayloadBytes];
  encode(about, bytes);
  std::uint32_t seq = 0;
  if (journal_->append(update::kind::kSupervisor, bytes, sizeof bytes, &seq) !=
      update::JournalError::kNone) {
    return false;
  }
  if (out_seq != nullptr) *out_seq = seq;
  ++entries_;
  status_.entries = entries_;
  if (first_seq_ == 0 && what == Wrote::kIncident) {
    first_seq_ = seq;
    (void)journal_->protect(seq);
  }
  return true;
}

// ------------------------------------------------------------------- the day and its caps

bool Supervisor::rollWindow() {
  if (!window_open_) return false;
  interfaces::WallTime wt;
  const bool have_time = wall_.now(&wt);
  if (have_time && window_epoch_ != 0) {
    // A time of day is the only thing that can measure a day across a restart. It governs.
    if (wt.epoch >= window_epoch_ && wt.epoch - window_epoch_ >= kCapWindowS) {
      counts_ = Counts{};
      window_open_ = false;
      window_epoch_ = 0;
      window_ms_ = 0;
      status_.window_reanchored = false;
      storeRecord();
      return true;
    }
    return false;
  }
  const std::uint64_t now = clock_.nowMs();
  if (now < window_ms_) {
    // The clock restarted (a deep sleep, interfaces/clock.h): the window's age cannot be
    // measured, so the worst is assumed - the window begins again here and the counts stay.
    window_ms_ = now;
    status_.window_reanchored = true;
    if (have_time) window_epoch_ = wt.epoch;
    storeRecord();
    return false;
  }
  if (have_time && window_epoch_ == 0) {
    // A time of day arrived during the window: it is adopted, dated back by what the monotonic
    // clock can account for, so the day ends when it should and not a day later.
    const std::uint64_t elapsed_s = (now - window_ms_) / 1000u;
    window_epoch_ = wt.epoch > elapsed_s ? static_cast<std::uint32_t>(wt.epoch - elapsed_s) : wt.epoch;
    storeRecord();
  }
  if (now - window_ms_ >= kCapWindowMs) {
    counts_ = Counts{};
    window_open_ = false;
    window_epoch_ = 0;
    window_ms_ = 0;
    status_.window_reanchored = false;
    storeRecord();
    return true;
  }
  return false;
}

bool Supervisor::capReached(Rung rung) const {
  switch (rung) {
    case Rung::kReset: return counts_.resets >= caps_.resets;
    case Rung::kPowerCycle: return counts_.power_cycles >= caps_.power_cycles;
    case Rung::kGoldenImage: return counts_.flashes >= caps_.flashes;
    default: return false;
  }
}

void Supervisor::countAction(Rung rung) {
  if (!window_open_) {
    window_open_ = true;
    window_ms_ = clock_.nowMs();
    interfaces::WallTime wt;
    window_epoch_ = wall_.now(&wt) ? wt.epoch : 0;
  }
  switch (rung) {
    case Rung::kReset: ++counts_.resets; break;
    case Rung::kPowerCycle: ++counts_.power_cycles; break;
    case Rung::kGoldenImage: ++counts_.flashes; break;
    default: break;
  }
  storeRecord();
}

// ------------------------------------------------------------------- the battery

bool Supervisor::batteryFor(Rung rung, std::uint16_t* mv, bool* fresh) {
  update::BatteryReading reading;
  const bool have = battery_.read(&reading);
  if (have) {
    battery_known_ = true;
    battery_mv_ = reading.millivolts;
    battery_at_ms_ = clock_.nowMs();
  }
  if (mv != nullptr) *mv = battery_known_ ? battery_mv_ : 0;
  if (fresh != nullptr) *fresh = have;
  if (!battery_known_) {
    // Nothing has ever been read. A reset and a power cycle are partly how one finds out whether
    // the radio is there to be asked at all, so an unknown battery does not refuse them; the
    // incident records that it was unknown, and the update machine refuses a flash of its own
    // accord (core/update/radio_update.h, Reason::kBatteryUnknown).
    return true;
  }
  const std::uint16_t threshold = rung == Rung::kGoldenImage ? kFlashMinMv : kPowerCycleMinMv;
  return battery_mv_ >= threshold;
}

// ------------------------------------------------------------------- the heartbeat

void Supervisor::recordAttention(std::uint8_t reason, std::uint16_t detail, std::uint8_t repeat) {
  // §8: the radio repeats one attention up to ten times, a second apart, until a request of ours
  // arrives. Those are one attention, not ten.
  if (attention_count_ > 0) {
    Attention& newest = attentions_[attention_count_ - 1];
    const std::uint64_t now = clock_.nowMs();
    if (newest.reason == reason && newest.detail == detail && now >= newest.at_ms &&
        now - newest.at_ms <= kAttentionRepeatWindowMs) {
      newest.repeat = repeat;
      return;
    }
  }
  if (attention_count_ >= kAttentionsKept) {
    // §3's rule for the client's own queue, and the same here: a Walter that slept through fifty
    // attentions wants the first four, not the last.
    ++attentions_dropped_;
    return;
  }
  Attention& a = attentions_[attention_count_++];
  a = Attention{};
  a.present = true;
  a.reason = reason;
  a.detail = detail;
  a.repeat = repeat;
  a.at_ms = clock_.nowMs();
  interfaces::WallTime wt;
  if (wall_.now(&wt)) a.at_epoch = wt.epoch;
  Payload p = payloadNow(Wrote::kAttention);
  p.rung = static_cast<Rung>(0);  // not a rung: the reason is in `detail`'s low byte
  p.outcome = reason;
  p.detail = static_cast<std::uint32_t>(detail) | (static_cast<std::uint32_t>(repeat) << 16);
  (void)journalEntry(Wrote::kAttention, p);
}

void Supervisor::acknowledgeAttentions() {
  // §8: the radio repeats an attention "until any valid request arrives". One of ours has.
  for (std::size_t i = 0; i < attention_count_; ++i) attentions_[i].acknowledged = true;
}

void Supervisor::drainEvents() {
  radio_.poll();
  link::client::QueuedEvent e;
  // Bounded: the client's queue is kEventQueue deep and poll() does not refill it here.
  for (std::size_t guard = 0; guard < link::client::kEventQueue + 1 && radio_.takeEvent(&e); ++guard) {
    if (e.type == link::type::kEvAttention) {
      link::EvAttention a;
      if (link::decode(e.view(), &a)) recordAttention(a.reason, a.detail, a.repeat);
      continue;
    }
    // An EV_BOOT reaches this object through onRadioRestart() - the client notices it as it
    // reads it - and the mesh client's replies are phase 4's. Counted, not acted on.
    if (e.type != link::type::kEvBoot) ++events_dropped_;
  }
}

void Supervisor::noteUptime(std::uint32_t uptime_s) {
  const std::uint64_t now = clock_.nowMs();
  // The second way a boot loop shows (software-goals must-have 6): an uptime that never gets
  // past kBootLoopUptimeS, asked twice at least that far apart. One short uptime is a radio that
  // has just booted, which is not a fault; two, a boot-loop window apart, is a radio that keeps
  // starting over.
  if (last_uptime_known_ && uptime_s < kBootLoopUptimeS && last_uptime_s_ < kBootLoopUptimeS &&
      now >= last_status_ms_ && now - last_status_ms_ >= static_cast<std::uint64_t>(kBootLoopUptimeS) * 1000u) {
    short_uptime_twice_ = true;
  }
  last_uptime_s_ = uptime_s;
  last_status_ms_ = now;
  last_uptime_known_ = true;
  status_.uptime_s = uptime_s;
}

bool Supervisor::bootLooping() const {
  return restarts_ >= kBootLoopBoots || short_uptime_twice_;
}

bool Supervisor::ask() {
  const std::uint64_t now = clock_.nowMs();
  status_.last_asked_ms = now;
  next_ask_ms_ = now + kHeartbeatIntervalMs;
  asked_once_ = true;
  drainEvents();

  // §9: a session first when there is none - HELLO is the first request of a session, and its
  // three silences are what decide whether the other end is NoiseScope.
  if (!radio_.session().up || radio_.mode() == link::client::Mode::kUnknown) {
    (void)radio_.hello();
    acknowledgeAttentions();
  }
  if (radio_.mode() == link::client::Mode::kText) {
    status_.health = Health::kScanner;
    return false;
  }

  // §5.2: the heartbeat proper. "It proves the firmware's main loop is turning, which a reset
  // line cannot."
  link::client::Answer<link::PingResponse> ping = radio_.ping(low32(now));
  acknowledgeAttentions();
  if (!ping.ok()) {
    // One HELLO, which also runs §9's scanner probe, and one more PING: that is the link's three
    // tries, twice over, inside one question.
    const link::client::Answer<link::HelloResponse> hello = radio_.hello();
    acknowledgeAttentions();
    if (radio_.mode() == link::client::Mode::kText) {
      status_.health = Health::kScanner;
      return false;
    }
    if (!hello.ok()) {
      status_.health = Health::kSilent;
      return false;
    }
    ping = radio_.ping(low32(clock_.nowMs()));
    acknowledgeAttentions();
    if (!ping.ok()) {
      status_.health = Health::kSilent;
      return false;
    }
  }

  ++status_.pings;
  status_.last_answer_ms = clock_.nowMs();
  status_.health = Health::kHealthy;
  status_.boot_id = radio_.session().boot_id;
  if (!status_.ever_answered) {
    status_.ever_answered = true;
    status_.healthy_since_ms = status_.last_answer_ms;
  }
  noteUptime(ping.message.uptime_s);

  // A restart the supervisor has seen: the clock first (§5.4 - the first-batch boards have no
  // RTC, so the radio's clock is lost at every reset and "the Walter sets it after every EV_BOOT
  // and every HELLO whose boot_id it has not seen - before anything else it asks the RAK to
  // do"), then STATUS again, because everything it said belonged to the boot before.
  if (restart_seen_) {
    restart_seen_ = false;
    status_.healthy_since_ms = status_.last_answer_ms;
    // The client sets the clock itself on the first call after a restart it noticed (§5.4), so by
    // now it may already be done: what matters is whether the radio is still waiting for one.
    (void)radio_.serviceTime();
    status_.time_set = !radio_.timePending();
    status_.time_skipped = radio_.timePending();
    const link::client::Answer<link::StatusResponse> st = radio_.status();
    acknowledgeAttentions();
    if (st.ok()) {
      reset_reason_ = st.message.reset_reason;
      status_.reset_reason = st.message.reset_reason;
      noteUptime(st.message.uptime_s);
      if (st.message.attention != link::attention::kNone) {
        // §5.3: a pending attention stays visible in STATUS until the Walter has made a request,
        // so a Walter that woke for its own reasons sees it too (§8).
        recordAttention(st.message.attention, 0, 0);
      }
    }
  }

  // The battery, while the link is up: this is the reading a rung will use when it is not
  // (core/update/last_good_battery.h keeps it).
  update::BatteryReading reading;
  if (battery_.read(&reading)) {
    battery_known_ = true;
    battery_mv_ = reading.millivolts;
    battery_at_ms_ = clock_.nowMs();
  }
  return true;
}

// ------------------------------------------------------------------- the incidents

Incident* Supervisor::current() {
  if (incident_count_ == 0) return nullptr;
  return incidents_[0].outcome == Outcome::kRunning ? &incidents_[0] : nullptr;
}

void Supervisor::beginIncident(Cause cause) {
  cause_ = cause;
  entries_ = 0;
  first_seq_ = 0;
  // Newest first: the oldest goes when there is no room (kIncidentsKept).
  for (std::size_t i = kIncidentsKept - 1; i > 0; --i) incidents_[i] = incidents_[i - 1];
  incidents_[0] = Incident{};
  if (incident_count_ < kIncidentsKept) ++incident_count_;
  Incident& in = incidents_[0];
  in.present = true;
  in.cause = cause;
  in.outcome = Outcome::kRunning;
  in.began_ms = clock_.nowMs();
  interfaces::WallTime wt;
  if (wall_.now(&wt)) in.began_epoch = wt.epoch;
  in.battery_mv = battery_known_ ? battery_mv_ : 0;
  in.reset_reason = reset_reason_;
  in.restarts = cap8(restarts_);
  in.last_uptime_s = last_uptime_s_;
  // A boot loop takes the ladder from rung 3: "a reset will not cure what a reset is already
  // doing". Rungs 1 and 2 are recorded as skipped so the report says why they were not used.
  if (cause == Cause::kBootLoop) {
    in.rung[0] = RungOutcome::kSkipped;
    in.rung[1] = RungOutcome::kSkipped;
    next_rung_ = Rung::kPowerCycle;
  } else {
    next_rung_ = Rung::kAskAgain;
  }
  storeIncidents();
  (void)journalEntry(Wrote::kIncident, payloadNow(Wrote::kIncident));
}

void Supervisor::noteRung(Rung rung, RungOutcome outcome) {
  rung_ = rung;
  status_.rung = rung;
  Incident* in = current();
  if (in == nullptr) return;
  const std::size_t i = static_cast<std::size_t>(rung) - 1;
  if (i < kRungCount) {
    in->rung[i] = outcome;
    in->rungs_tried = static_cast<std::uint8_t>(in->rungs_tried | rungBit(rung));
  }
  in->battery_mv = battery_known_ ? battery_mv_ : 0;
  in->reset_reason = reset_reason_;
  in->restarts = cap8(restarts_);
  storeIncidents();
}

void Supervisor::endIncident(Outcome outcome, Rung cured_at, StopReason stop) {
  Incident* in = current();
  if (in != nullptr) {
    in->outcome = outcome;
    in->cured_at = cured_at;
    in->stop = stop;
    const std::uint64_t now = clock_.nowMs();
    in->lasted_ms = now >= in->began_ms ? low32(now - in->began_ms) : 0;
    if (cured_at != Rung::kNone) {
      const std::size_t i = static_cast<std::size_t>(cured_at) - 1;
      if (i < kRungCount) in->rung[i] = RungOutcome::kCured;
    }
    storeIncidents();
  }
  Payload p = payloadNow(Wrote::kOutcome);
  p.rung = cured_at;
  p.outcome = static_cast<std::uint8_t>(outcome);
  p.detail = static_cast<std::uint32_t>(stop);
  if (in != nullptr) p.rungs_tried = in->rungs_tried;
  (void)journalEntry(Wrote::kOutcome, p);
  if (journal_ != nullptr) journal_->unprotect();
  cause_ = Cause::kNone;
  next_rung_ = Rung::kNone;
  by_hand_ = false;
  flash_started_ = false;
  entries_ = 0;
  first_seq_ = 0;
  // The restarts that were the evidence have been acted on: the count starts again from here,
  // so that a radio which keeps looping is a *new* incident each time - and spends the caps -
  // rather than one incident that never ends. The radio restarting because of our own reset or
  // kill is part of what is forgotten here.
  restarts_ = 0;
  restart_window_ms_ = clock_.nowMs();
  short_uptime_twice_ = false;
  last_uptime_known_ = false;
  storeRecord();
}

// ------------------------------------------------------------------- the steps

Step Supervisor::enter(State next, const char* note) {
  Step s;
  s.before = state_;
  s.after = next;
  s.changed = next != state_;
  s.note = note;
  s.rung = rung_;
  s.silence = status_.silence_seen;
  state_ = next;
  status_.state = next;
  return s;
}

Step Supervisor::waiting(const char* note) {
  Step s;
  s.before = state_;
  s.after = state_;
  s.waited = true;
  s.note = note;
  s.rung = rung_;
  return s;
}

Step Supervisor::rungStep(Rung rung, RungOutcome outcome, const char* note, State next, Rung then) {
  noteRung(rung, outcome);
  next_rung_ = then;
  Step s = enter(next, note);
  s.changed = true;  // the rung happened, whether or not the state moved
  s.rung = rung;
  s.rung_outcome = outcome;
  return s;
}

void Supervisor::refreshStatus() {
  status_.state = state_;
  status_.counts = counts_;
  status_.caps = caps_;
  status_.window_began_ms = window_ms_;
  status_.window_began_epoch = window_epoch_;
  status_.stopped = stopped_;
  status_.stop_reason = stop_reason_;
  status_.stopped_after = stopped_after_;
  status_.stopped_ms = stopped_ms_;
  status_.restarts = restarts_;
  status_.restarts_total = restarts_total_;
  status_.boot_loop = bootLooping();
  status_.battery_known = battery_known_;
  status_.battery_mv = battery_mv_;
  const std::uint64_t now = clock_.nowMs();
  status_.battery_age_ms = battery_known_ && now >= battery_at_ms_ ? low32(now - battery_at_ms_) : 0;
  status_.cause = cause_;
  status_.rung = rung_;
  status_.entries = entries_;
  status_.attentions = static_cast<std::uint32_t>(attention_count_);
  status_.awaiting_first_install = awaiting_first_install_;
  status_.awaiting_since_seq = awaiting_since_seq_;
  status_.quiet_left_ms = 0;
  if (stopped_) {
    if (now < stopped_ms_) {
      status_.quiet_left_ms = kQuietPeriodMs;  // a new clock epoch: the whole period again
    } else if (now - stopped_ms_ < kQuietPeriodMs) {
      status_.quiet_left_ms = kQuietPeriodMs - low32(now - stopped_ms_);
    }
  }
}

void Supervisor::begin() {
  begun_ = true;
  boot_ms_ = clock_.nowMs();
  loadRecord();
  loadIncidents();
  (void)rollWindow();
  // A stop whose quiet period has passed is lifted here, so that the first step of the next wake
  // climbs the ladder once more rather than waiting for a person (the task: "or until a long
  // quiet period has passed ... after which the ladder may be tried once more").
  if (stopped_) {
    const std::uint64_t now = clock_.nowMs();
    const bool over = now >= stopped_ms_ && now - stopped_ms_ >= kQuietPeriodMs;
    if (over) {
      Payload p = payloadNow(Wrote::kQuietOver);
      p.rungs_tried = stopped_after_;
      p.outcome = static_cast<std::uint8_t>(stop_reason_);
      (void)journalEntry(Wrote::kQuietOver, p);
      stopped_ = false;
      stop_reason_ = StopReason::kNone;
      stopped_after_ = 0;
      stopped_ms_ = 0;
    }
  }
  state_ = stopped_ ? State::kStopped : State::kIdle;
  storeRecord();
  // A first install that committed while this Walter was down, or just before it restarted,
  // clears the mark here (task p5-03).
  reconcileFirstInstall();
  refreshStatus();
}

Step Supervisor::step() {
  Step s;
  reconcileFirstInstall();
  switch (state_) {
    case State::kIdle: s = stepIdle(); break;
    case State::kClassify: s = stepClassify(); break;
    case State::kClimbing: s = stepClimbing(); break;
    case State::kWaiting: s = stepWaiting(); break;
    case State::kJudging: s = stepJudging(); break;
    case State::kFlashing: s = stepFlashing(); break;
    case State::kStopped: s = stepStopped(); break;
  }
  refreshStatus();
  return s;
}

bool Supervisor::climbing() const {
  switch (state_) {
    case State::kClassify:
    case State::kClimbing:
    case State::kWaiting:
    case State::kJudging:
    case State::kFlashing:
      return true;
    case State::kIdle:
    case State::kStopped:
      return false;
  }
  return false;
}

Silence Supervisor::classify() {
  // The order matters: each of the first three is an explanation that forbids acting, and the
  // cheapest to be sure of comes first (§9, and ARCHITECTURE §2 rule 5 for the glitch).
  if (status_.health == Health::kScanner || radio_.mode() == link::client::Mode::kText) {
    return Silence::kScanner;
  }
  // Our own power-up reset the radio (§2 rule 5: a power-up of the ESP32-S3 glitches GPIO1-14 low
  // for about 60 us, and that reaches nRESET), and the rail may still be settling.
  const health::ResetReason why = health_.thisBoot().reason;
  const bool we_just_powered_up = why == health::ResetReason::kPowerOn ||
                                  why == health::ResetReason::kBrownout ||
                                  why == health::ResetReason::kUnknown;
  // Measured from this Walter's boot to the moment the asking began, not to now: asking a radio
  // that does not answer costs the link's three attempts twice over (§3), which is seconds, and
  // a silence seen while the radio could still have been booting is not evidence of anything.
  const std::uint64_t asked = status_.last_asked_ms >= boot_ms_ ? status_.last_asked_ms - boot_ms_ : 0;
  if ((we_just_powered_up && asked < kWalterPowerUpGraceMs) ||
      pins_.power() == board::RadioPower::kUnknown) {
    return Silence::kWalterPoweredUp;
  }
  // An update: not ours to disturb (S-7 and the journal's own account of it).
  if (update_ != nullptr && update_->active()) return Silence::kUpdateRunning;
  if (journal_ != nullptr) {
    update::Entry e;
    update::Payload up;
    if (journal_->lastOf(update::kind::kRadioUpdate, &e) && update::decode(e, &up) &&
        !update::isTerminal(up.state)) {
      return Silence::kUpdateRunning;
    }
    if (journal_->lastOf(update::kind::kSelfUpdate, &e) && e.size >= 1 &&
        update::self_update::unconfirmed(e.payload[0])) {
      return Silence::kUpdateRunning;
    }
  }
  // Task p5-03: a radio that `rak provision` gave its base and nothing to run is silent on
  // purpose, until `rak update --first-install` commits.
  if (awaiting_first_install_) return Silence::kAwaitingFirstInstall;
  return Silence::kNotAnswering;
}

Step Supervisor::stepIdle() {
  if (stopped_) return enter(State::kStopped, "stopped: the ladder was climbed and nothing worked");
  const std::uint64_t now = clock_.nowMs();
  if (asked_once_ && now < next_ask_ms_) return waiting("the heartbeat is not due yet");
  if (ask()) {
    status_.silence_seen = Silence::kNone;
    // A radio that answers and keeps restarting is in a boot loop all the same, and takes the
    // ladder from rung 3.
    if (bootLooping()) {
      beginIncident(Cause::kBootLoop);
      return enter(State::kClimbing, "the radio is restarting itself: the ladder from rung 3");
    }
    by_hand_ = false;  // `rak ladder` found a radio that answers: there is nothing to climb
    return waiting("the radio answered: healthy");
  }
  return enter(State::kClassify, "no answer: what does the silence mean");
}

Step Supervisor::stepClassify() {
  const Silence what = classify();
  status_.silence_seen = what;
  switch (what) {
    case Silence::kScanner:
      status_.health = Health::kScanner;
      return enter(State::kIdle, describe(Silence::kScanner));
    case Silence::kWalterPoweredUp: {
      // Wait: the radio was reset by our own power-up and is still booting, or the rail is still
      // settling (§2 rule 3 leaves it *unknown* for the settle time). Asked again when the
      // grace is over, not after a whole heartbeat interval.
      const std::uint64_t now = clock_.nowMs();
      next_ask_ms_ = now < kWalterPowerUpGraceMs ? kWalterPowerUpGraceMs
                                                 : now + pins_.timing().settle_ms;
      asked_once_ = true;
      return enter(State::kIdle, describe(Silence::kWalterPoweredUp));
    }
    case Silence::kUpdateRunning:
      return enter(State::kIdle, describe(Silence::kUpdateRunning));
    case Silence::kAwaitingFirstInstall:
      status_.health = Health::kAwaitingFirstInstall;
      return enter(State::kIdle, describe(Silence::kAwaitingFirstInstall));
    case Silence::kNone:
    case Silence::kNotAnswering:
      break;
  }
  beginIncident(bootLooping() ? Cause::kBootLoop : Cause::kSilence);
  return enter(State::kClimbing,
               cause_ == Cause::kBootLoop ? "the radio is restarting itself: the ladder from rung 3"
                                          : "the radio is not answering: the ladder");
}

Step Supervisor::stepClimbing() {
  (void)rollWindow();
  switch (next_rung_) {
    case Rung::kAskAgain: {
      // Rung 1: the link's own three tries have already gone (that is what found the silence);
      // this is the pause before three more.
      wait_until_ms_ = clock_.nowMs() + kAskAgainPauseMs;
      return rungStep(Rung::kAskAgain, RungOutcome::kDone, "asking again after a pause",
                      State::kWaiting, Rung::kReset);
    }
    case Rung::kReset: {
      if (capReached(Rung::kReset)) {
        Payload p = payloadNow(Wrote::kRung);
        p.rung = Rung::kReset;
        p.outcome = static_cast<std::uint8_t>(RungOutcome::kRefusedCap);
        (void)journalEntry(Wrote::kRung, p);
        return rungStep(Rung::kReset, RungOutcome::kRefusedCap,
                        "today's reset cap is reached: refused, and the refusal is reported",
                        State::kClimbing, Rung::kPowerCycle);
      }
      // The count and its journal entry go down *before* the pulse: a Walter that dies in the
      // middle of one has still spent it (S-5 and §6's rule for every step of an update).
      countAction(Rung::kReset);
      Payload p = payloadNow(Wrote::kRung);
      p.rung = Rung::kReset;
      p.outcome = static_cast<std::uint8_t>(RungOutcome::kDone);
      (void)journalEntry(Wrote::kRung, p);
      const board::SlotHold hold(slots_, slot_);
      if (!hold.ok()) {
        return failedRung(Rung::kReset, board::describe(hold.result()), Rung::kPowerCycle,
                          static_cast<std::uint32_t>(hold.result()));
      }
      // ARCHITECTURE §2 rule 2: the pulse waits out the spacing rather than violating it.
      const board::Outcome r = pins_.resetPulse();
      if (r != board::Outcome::kOk) {
        return failedRung(Rung::kReset, board::describe(r), Rung::kPowerCycle,
                          static_cast<std::uint32_t>(r));
      }
      radio_.radioWasReset();  // §5.1, §9: everything this end believed about it is gone
      wait_until_ms_ = clock_.nowMs() + kAfterResetMs;
      return rungStep(Rung::kReset, RungOutcome::kDone, "nRESET pulsed; waiting for it to boot",
                      State::kWaiting, Rung::kPowerCycle);
    }
    case Rung::kPowerCycle: {
      if (capReached(Rung::kPowerCycle)) {
        Payload p = payloadNow(Wrote::kRung);
        p.rung = Rung::kPowerCycle;
        p.outcome = static_cast<std::uint8_t>(RungOutcome::kRefusedCap);
        (void)journalEntry(Wrote::kRung, p);
        return rungStep(Rung::kPowerCycle, RungOutcome::kRefusedCap,
                        "today's power-cycle cap is reached: refused, and the refusal is reported",
                        State::kClimbing, Rung::kGoldenImage);
      }
      std::uint16_t mv = 0;
      bool fresh = false;
      if (!batteryFor(Rung::kPowerCycle, &mv, &fresh)) {
        Payload p = payloadNow(Wrote::kRung);
        p.rung = Rung::kPowerCycle;
        p.outcome = static_cast<std::uint8_t>(RungOutcome::kRefusedBattery);
        p.detail = mv;
        if (!fresh) p.flags |= kFlagBatteryStale;
        (void)journalEntry(Wrote::kRung, p);
        // A battery too low for the rail to come back up is also too low for a reflash: the
        // climb ends here, with the reason.
        noteRung(Rung::kPowerCycle, RungOutcome::kRefusedBattery);
        return stopNow(StopReason::kBatteryTooLow);
      }
      countAction(Rung::kPowerCycle);
      Payload p = payloadNow(Wrote::kRung);
      p.rung = Rung::kPowerCycle;
      p.outcome = static_cast<std::uint8_t>(RungOutcome::kDone);
      p.detail = mv;
      if (!fresh) p.flags |= kFlagBatteryStale;
      (void)journalEntry(Wrote::kRung, p);
      const board::SlotHold hold(slots_, slot_);
      if (!hold.ok()) {
        return failedRung(Rung::kPowerCycle, board::describe(hold.result()), Rung::kGoldenImage,
                          static_cast<std::uint32_t>(hold.result()));
      }
      // ARCHITECTURE §2 rules 3 and 4: every other pin is a plain input first, the line is
      // driven high or not at all, and the rail is given its settle time before anything drives
      // again. All of that is RadioPins'; this only asks.
      const board::Outcome r = pins_.killCycle(kKillOffMs);
      if (r != board::Outcome::kOk) {
        return failedRung(Rung::kPowerCycle, board::describe(r), Rung::kGoldenImage,
                          static_cast<std::uint32_t>(r));
      }
      radio_.radioWasReset();  // the rail went down: a new boot over there, and the UART was taken
      wait_until_ms_ = clock_.nowMs() + kAfterPowerCycleMs;
      return rungStep(Rung::kPowerCycle, RungOutcome::kDone,
                      "the rail was cut and brought back; waiting for it to boot", State::kWaiting,
                      Rung::kGoldenImage);
    }
    case Rung::kGoldenImage:
      return lookAtChip();
    case Rung::kStop:
    case Rung::kNone:
      break;
  }
  return stopNow(StopReason::kEverythingTried);
}

Step Supervisor::stepWaiting() {
  if (clock_.nowMs() < wait_until_ms_) return waiting("the wait after the rung");
  return enter(State::kJudging, "the wait is over: asking the radio again");
}

Step Supervisor::stepJudging() {
  next_ask_ms_ = 0;
  asked_once_ = false;
  const bool answered = ask();
  Payload p = payloadNow(Wrote::kJudged);
  p.rung = rung_;
  p.outcome = static_cast<std::uint8_t>(answered ? RungOutcome::kCured : RungOutcome::kDone);
  (void)journalEntry(Wrote::kJudged, p);
  if (answered) {
    // The radio is back. The rungs above this one are not used - which is the whole shape of the
    // ladder, and what the bench's "remove the jumper during the ladder" item watches.
    const Rung cured_at = rung_;
    endIncident(Outcome::kCured, cured_at, StopReason::kNone);
    return enter(State::kIdle, "the radio is back: the climb stops here");
  }
  // The scanner showing up mid-climb is an answer of a kind: something is running over there,
  // and it is not this unit's business to reflash it from under a person.
  if (status_.health == Health::kScanner) {
    endIncident(Outcome::kCured, rung_, StopReason::kNone);
    return enter(State::kIdle, describe(Silence::kScanner));
  }
  return enter(State::kClimbing, "still silent: the next rung");
}

// Rung 4: what is on the chip, read-only (Flasher::verify() writes nothing and does not reset).
Step Supervisor::lookAtChip() {
  if (store_ == nullptr || flasher_ == nullptr || update_ == nullptr) {
    noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
    return stopNow(StopReason::kNoStore);
  }
  const flasher::BoardProfile* profile = flasher::profileForSlot(slot_);
  if (profile == nullptr) {
    noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
    return stopNow(StopReason::kNoStore);
  }
  image::Record golden;
  if (store_->read(image::Which::kGolden, slot_, &golden) != image::StoreError::kNone ||
      !golden.present) {
    noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
    return stopNow(StopReason::kNoGoldenImage);
  }
  image::StoredImage bytes;
  if (store_->open(image::Which::kGolden, slot_, &bytes) != image::StoreError::kNone) {
    noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
    return stopNow(StopReason::kNoGoldenImage);
  }
  flasher::Image image;
  image.source = &bytes;
  image.load_address = golden.load_address;
  image.sha256 = golden.sha256;
  Payload p = payloadNow(Wrote::kRung);
  p.rung = Rung::kGoldenImage;
  p.outcome = static_cast<std::uint8_t>(RungOutcome::kNotTried);
  (void)journalEntry(Wrote::kRung, p);
  const flasher::Result r = flasher_->verify(image, *profile, slot_);
  if (r.ok() && r.settings_valid) {
    // The firmware is intact and flashing it again would change nothing: rung 5, with that as
    // the reason. A radio that will not answer with the right bytes on its chip is a hardware
    // fault (docs/ARCHITECTURE.md §8, spec/SYSTEM.md §6).
    noteRung(Rung::kGoldenImage, RungOutcome::kIntact);
    return stopNow(StopReason::kFirmwareIntact);
  }
  if (r.step == flasher::Step::kProtected) {
    // Phase 1's rule: the flasher detects a protected chip and refuses; opening one is a bench
    // procedure for a person, and nothing here may erase a whole chip to get past it.
    noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
    return stopNow(StopReason::kChipProtected);
  }
  const bool differs = r.ok() ||  // the bytes are there but the bootloader would not start them
                       r.step == flasher::Step::kReadBack || r.step == flasher::Step::kDigest;
  if (!differs) {
    noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
    return stopNow(StopReason::kChipUnreadable);
  }
  // It differs. The golden image goes back through the update machine's rollback path - the same
  // flash, boot and health check a rollback uses, journalled as one (core/update/radio_update.h,
  // Kind::kRoleRepeater).
  if (capReached(Rung::kGoldenImage)) {
    Payload cap = payloadNow(Wrote::kRung);
    cap.rung = Rung::kGoldenImage;
    cap.outcome = static_cast<std::uint8_t>(RungOutcome::kRefusedCap);
    (void)journalEntry(Wrote::kRung, cap);
    noteRung(Rung::kGoldenImage, RungOutcome::kRefusedCap);
    return stopNow(StopReason::kCapReached);
  }
  std::uint16_t mv = 0;
  bool fresh = false;
  if (!batteryFor(Rung::kGoldenImage, &mv, &fresh)) {
    Payload low = payloadNow(Wrote::kRung);
    low.rung = Rung::kGoldenImage;
    low.outcome = static_cast<std::uint8_t>(RungOutcome::kRefusedBattery);
    low.detail = mv;
    if (!fresh) low.flags |= kFlagBatteryStale;
    (void)journalEntry(Wrote::kRung, low);
    noteRung(Rung::kGoldenImage, RungOutcome::kRefusedBattery);
    return stopNow(StopReason::kBatteryTooLow);
  }
  update::Request request;
  request.kind = update::Kind::kRoleRepeater;
  request.slot = slot_;
  request.link_known_down = true;  // the supervisor has classified it: this is why we are here
  if (update_->active() || !update_->start(request)) {
    noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
    return stopNow(StopReason::kFlashFailed);
  }
  countAction(Rung::kGoldenImage);
  Payload go = payloadNow(Wrote::kRung);
  go.rung = Rung::kGoldenImage;
  go.outcome = static_cast<std::uint8_t>(RungOutcome::kDone);
  go.detail = mv;
  (void)journalEntry(Wrote::kRung, go);
  flash_started_ = true;
  Step s = enter(State::kFlashing, "the chip does not hold the golden image: flashing it back");
  s.changed = true;
  s.rung = Rung::kGoldenImage;
  s.rung_outcome = RungOutcome::kDone;
  return s;
}

Step Supervisor::stepFlashing() {
  if (update_ == nullptr) return stopNow(StopReason::kNoStore);
  if (update_->active()) {
    const update::StepResult u = update_->step();
    Step s;
    s.before = state_;
    s.after = state_;
    s.rung = Rung::kGoldenImage;
    s.changed = u.changed;
    s.waited = u.waited;
    s.note = u.note;
    if (!u.changed && u.reason == update::Reason::kJournal) {
      // The machine cannot write its journal and has stopped where it is. So does this.
      noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
      return stopNow(StopReason::kFlashFailed);
    }
    return s;
  }
  const update::State ended = update_->state();
  const update::Reason why = update_->reason();
  update_->clear();
  if (ended != update::State::kDone) {
    // Why the machine did not finish decides what this is. The flasher failing is one thing -
    // the chip, the lines or the debug port - and the golden image going back and the radio
    // still not answering is another: that is the hardware fault of docs/ARCHITECTURE.md §8,
    // "a radio that does not answer after the golden image is back".
    noteRung(Rung::kGoldenImage, RungOutcome::kFailed);
    return stopNow(why == update::Reason::kFlashFailed        ? StopReason::kFlashFailed
                   : why == update::Reason::kNoImageToReturnTo ? StopReason::kNoGoldenImage
                                                              : StopReason::kEverythingTried);
  }
  // The machine proved the image itself (HELLO, the clock, STATUS, the proving period): the
  // radio is answering. The wait before judging is only the room between the two.
  wait_until_ms_ = clock_.nowMs() + kAfterGoldenImageMs;
  next_ask_ms_ = 0;
  asked_once_ = false;
  return rungStep(Rung::kGoldenImage, RungOutcome::kDone,
                  "the golden image is back and proved; asking again", State::kWaiting,
                  Rung::kStop);
}

// A rung that could not be done: the pins refused, or the slot was held elsewhere. The count has
// already gone down (it is spent whether or not the pin moved), and the failure is journalled in
// its turn so that the story is not left saying the rung was done.
Step Supervisor::failedRung(Rung rung, const char* why, Rung then, std::uint32_t detail) {
  Payload p = payloadNow(Wrote::kRung);
  p.rung = rung;
  p.outcome = static_cast<std::uint8_t>(RungOutcome::kFailed);
  p.detail = detail;
  (void)journalEntry(Wrote::kRung, p);
  return rungStep(rung, RungOutcome::kFailed, why, State::kClimbing, then);
}

Step Supervisor::stopNow(StopReason reason) {
  Incident* in = current();
  stopped_ = true;
  stop_reason_ = reason;
  stopped_after_ = in != nullptr ? in->rungs_tried : 0;
  stopped_ms_ = clock_.nowMs();
  noteRung(Rung::kStop, RungOutcome::kDone);
  Payload p = payloadNow(Wrote::kStopped);
  p.rung = Rung::kStop;
  p.outcome = static_cast<std::uint8_t>(reason);
  p.rungs_tried = stopped_after_;
  (void)journalEntry(Wrote::kStopped, p);
  endIncident(Outcome::kStopped, Rung::kNone, reason);
  storeRecord();
  Step s = enter(State::kStopped, describe(reason));
  s.changed = true;
  s.rung = Rung::kStop;
  s.rung_outcome = RungOutcome::kDone;
  return s;
}

Step Supervisor::stepStopped() {
  const std::uint64_t now = clock_.nowMs();
  if (now >= stopped_ms_ && now - stopped_ms_ >= kQuietPeriodMs) {
    Payload p = payloadNow(Wrote::kQuietOver);
    p.rungs_tried = stopped_after_;
    p.outcome = static_cast<std::uint8_t>(stop_reason_);
    (void)journalEntry(Wrote::kQuietOver, p);
    stopped_ = false;
    stop_reason_ = StopReason::kNone;
    stopped_after_ = 0;
    stopped_ms_ = 0;
    storeRecord();
    next_ask_ms_ = 0;
    asked_once_ = false;
    return enter(State::kIdle, "the quiet period has passed: the ladder may be tried once more");
  }
  if (now < stopped_ms_) {
    // A new clock epoch (a deep sleep): the period cannot be measured from here, so it begins
    // again - the stop is kept, which is the side to err on.
    stopped_ms_ = now;
    storeRecord();
  }
  return waiting("stopped: nothing until a person, the server, or the quiet period");
}

// ------------------------------------------------------------------- the console's commands

bool Supervisor::runLadder() {
  if (climbing()) return false;
  by_hand_ = true;
  // A stop does not forbid a person: `rak ladder` is the bench's way of climbing it now.
  if (stopped_) {
    // A person is the other way out of rung 5 ("until a person or the server says otherwise"),
    // and it is journalled as such: kQuietOver with kFlagByHand is "the stop was lifted by
    // hand", as against the quiet period passing by itself.
    Payload p = payloadNow(Wrote::kQuietOver);
    p.rungs_tried = stopped_after_;
    p.outcome = static_cast<std::uint8_t>(stop_reason_);
    (void)journalEntry(Wrote::kQuietOver, p);
    stopped_ = false;
    stop_reason_ = StopReason::kNone;
    stopped_after_ = 0;
    stopped_ms_ = 0;
    storeRecord();
  }
  state_ = State::kIdle;
  next_ask_ms_ = 0;
  asked_once_ = false;
  status_.silence_seen = Silence::kNone;
  refreshStatus();
  return true;
}

void Supervisor::resetCaps() {
  counts_ = Counts{};
  window_open_ = false;
  window_epoch_ = 0;
  window_ms_ = 0;
  status_.window_reanchored = false;
  status_.counts_from_journal = false;
  stopped_ = false;
  stop_reason_ = StopReason::kNone;
  stopped_after_ = 0;
  stopped_ms_ = 0;
  restarts_ = 0;
  short_uptime_twice_ = false;
  if (state_ == State::kStopped) state_ = State::kIdle;
  storeRecord();
  by_hand_ = true;
  (void)journalEntry(Wrote::kCapsReset, payloadNow(Wrote::kCapsReset));
  by_hand_ = false;
  refreshStatus();
}

const Incident& Supervisor::incidentAt(std::size_t index) const {
  if (index >= incident_count_) return kNoIncident;
  return incidents_[index];
}

bool Supervisor::takeAttention(Attention* out) {
  if (attention_count_ == 0) return false;
  if (out != nullptr) *out = attentions_[0];
  for (std::size_t i = 1; i < attention_count_; ++i) attentions_[i - 1] = attentions_[i];
  --attention_count_;
  attentions_[attention_count_] = Attention{};
  status_.attentions = static_cast<std::uint32_t>(attention_count_);
  return true;
}

const Attention& Supervisor::attentionAt(std::size_t index) const {
  if (index >= attention_count_) return kNoAttention;
  return attentions_[index];
}

}  // namespace supervisor
}  // namespace core
