#include "walter_agent/agent_probe.h"

namespace agent {

namespace {

void copyInto(char* out, std::size_t cap, const char* text) {
  if (cap == 0) return;
  out[0] = 0;
  if (text == nullptr) return;
  std::size_t i = 0;
  for (; i + 1 < cap && text[i] != 0; ++i) out[i] = text[i];
  out[i] = 0;
}

void hexKey(char* out, std::size_t cap, const std::uint8_t* key) {
  if (cap < 2 * core::link::kPubkeySize + 1 || key == nullptr) {
    if (cap > 0) out[0] = 0;
    return;
  }
  for (std::size_t i = 0; i < core::link::kPubkeySize; ++i) {
    const char digits[] = "0123456789abcdef";
    out[i * 2] = digits[key[i] >> 4];
    out[i * 2 + 1] = digits[key[i] & 0x0F];
  }
  out[2 * core::link::kPubkeySize] = 0;
}

}  // namespace

IdentityProbe::IdentityProbe(Identity& identity, LinkMesh& mesh, Observations& observations,
                             WalterPlatform& platform, core::cycle::DutyCycle* duty,
                             const mm::IJobExtension* extension)
    : identity_(identity),
      mesh_(mesh),
      observations_(observations),
      platform_(platform),
      duty_(duty),
      extension_(extension) {}

mm::WakeIntent IdentityProbe::intent() const {
  if (duty_ == nullptr) return intentFor(core::power::Band::kUnknown, core::power::kUnknownMinutes * 60u);
  if (duty_->decided()) return intentFor(duty_->decision());
  // No reading has been taken in this wake and asking for one costs a POWER_GET, which `agent
  // id` does not spend (`status` does not either): the band the last wake ended in, and that
  // band's own interval.
  const core::power::Band band = duty_->reason().band();
  return intentFor(band, core::power::minutesFor(band) * 60u);
}

HeartbeatExtras IdentityProbe::extras() const {
  HeartbeatExtras out;
  out.at = platform_.wallClock().now();
  out.overheard = static_cast<long long>(mesh_.overheard());
  const mm::ObservationRing* ring = observations_.ring();
  out.observations_dropped = ring == nullptr ? 0 : static_cast<long long>(ring->dropped());
  out.reboots = static_cast<long long>(platform_.selfReboots());
  // The other three - the reconnect count, the HTTP failures and the results the offline buffer
  // had to drop - are the loop's own counters and there is no loop until task p4-05
  // (walter_agent/identity.h, HeartbeatExtras).
  return out;
}

bool IdentityProbe::identity(core::console::AgentIdentityView* out) {
  if (out == nullptr) return false;
  *out = core::console::AgentIdentityView{};
  const bool known = identity_.refresh();
  // The local door follows the identity wherever it is read (task p8-04): `LinkMesh::supports()`
  // carries no mesh job until this end knows which repeater is its own, so a line that printed
  // the thirteen types while the door was still shut would be a line the next heartbeat
  // contradicts. `AgentBackend` hands the key over the same way before every wake; this is the
  // console's own read of the radio, and `rak hello` then `agent id` is one of the two ways it
  // first appears.
  mesh_.setOwnKey(identity_.publicKey());
  observations_.useOwnKey(identity_.publicKey());
  out->known = known;
  out->session = identity_.sessionUp();
  // Three states, not two (task p10-03): the one place that decides them answered this at the
  // refresh above, and the console prints what it is given.
  out->identity = identity_.identityState();
  out->caps_identity_lost = identity_.capsSayIdentityLost();
  copyInto(out->public_key, sizeof out->public_key,
           identity_.publicKeyHex() == nullptr ? "" : identity_.publicKeyHex());
  copyInto(out->companion_name, sizeof out->companion_name, identity_.name());
  copyInto(out->companion_firmware, sizeof out->companion_firmware, identity_.companionFirmware());
  copyInto(out->companion_board, sizeof out->companion_board, identity_.board());
  copyInto(out->companion_upstream, sizeof out->companion_upstream, identity_.upstream());
  const mm::HeartbeatIdentity& id = identity_.heartbeatIdentity();
  copyInto(out->agent_version, sizeof out->agent_version, id.agentVersion);
  copyInto(out->board, sizeof out->board, id.board);
  copyInto(out->transport, sizeof out->transport, id.transport);
  out->boot_id = identity_.bootId();

  const RadioBlock& radio = identity_.radio();
  out->radio_known = radio.known;
  out->freq_mhz = radio.parameters.freqMHz;
  out->bw_khz = radio.parameters.bwKHz;
  out->sf = radio.parameters.sf;
  out->cr = radio.parameters.cr;

  // The list the heartbeat would carry, as one line: the same loop the library runs, through the
  // heartbeat the identity builds, so that this line and those bytes cannot disagree.
  mm::Heartbeat heartbeat;
  mm::ErrorCounter errors[Identity::kErrorCount];
  const mm::WakeIntent wanted = intent();
  identity_.fillHeartbeat(&heartbeat, &mesh_, extension_, extras(), wanted, errors,
                          Identity::kErrorCount);
  out->job_type_count = heartbeat.jobTypeCount;
  std::size_t at = 0;
  for (int i = 0; i < heartbeat.jobTypeCount; ++i) {
    const char* type = heartbeat.jobTypes[i];
    if (type == nullptr) continue;
    if (at != 0 && at + 2 < sizeof out->job_types) {
      out->job_types[at++] = ',';
      out->job_types[at++] = ' ';
    }
    for (std::size_t k = 0; type[k] != 0 && at + 1 < sizeof out->job_types; ++k) {
      out->job_types[at++] = type[k];
    }
  }
  out->job_types[at] = 0;

  out->next_check_in_s = wanted.nextCheckInS;
  out->max_transactions_this_wake = wanted.maxTransactionsThisWake;
  out->band = duty_ != nullptr && duty_->decided()
                  ? core::power::name(duty_->decision().band)
                  : (duty_ != nullptr ? core::power::name(duty_->reason().band())
                                      : core::power::name(core::power::Band::kUnknown));

  mm::IClock& clock = platform_.clock();
  out->time_known = clock.hasTime();
  out->epoch = clock.now();
  out->time_source = clock.sourceName();
  out->free_heap = platform_.freeHeap();
  out->min_free_heap = platform_.minFreeHeap();

  // The local door as it stands, after `setOwnKey` above (task p9-04).
  const LinkMeshCounters& door = mesh_.linkCounters();
  out->door_open = mesh_.hasOwnKey();
  out->local_jobs = door.local_jobs;
  out->local_requests = door.local_requests;
  out->local_silent = door.local_silent;
  out->local_unserved = door.local_unserved;
  // §5.9 (link 1.2, task p9-06): the same question the door asks at every request send.
  out->door_local_request = mesh_.answersRequestsLocally();

  const IdentityCounters& counted = identity_.counters();
  out->identity_reads = counted.identity_reads;
  out->radio_reads = counted.radio_reads;
  out->boot_changes = counted.boot_changes;
  out->unreadable = counted.unreadable;
  out->no_identity = counted.no_identity;
  out->identity_lost = counted.identity_lost;
  return known;
}

const char* IdentityProbe::heartbeat(std::size_t* length) {
  // The body is of the identity as it stands, so the identity is read first: one §5.6 request
  // and one §5.7 line at most, and none when nothing has changed. `agent heartbeat` typed
  // without `agent id` before it must not print a body with no key in it.
  (void)identity_.refresh();
  const std::size_t n =
      identity_.writeHeartbeat(&mesh_, extension_, extras(), intent(), body_, sizeof body_);
  if (length != nullptr) *length = n;
  return n == 0 ? nullptr : body_;
}

void IdentityProbe::observations(core::console::AgentObservationsView* out) {
  if (out == nullptr) return;
  *out = core::console::AgentObservationsView{};
  const ObservationCounters& counted = observations_.counters();
  out->pulls = counted.pulls;
  out->own_key = counted.own_key;
  out->unreadable = counted.unreadable;
  out->radio_dropped = counted.radio_dropped;
  out->remaining = counted.remaining;
  const mm::ObservationRing* ring = observations_.ring();
  if (ring == nullptr) return;
  out->ring = true;
  out->count = ring->size();
  out->capacity = mm::ObservationRing::kCapacity;
  out->heard = ring->heard();
  out->dropped = ring->dropped();

  const std::uint32_t now_ms = platform_.millis();
  const std::uint32_t epoch = platform_.wallClock().now();
  const std::size_t size = ring->size();
  const std::size_t rows = size < core::console::AgentObservationsView::kRows
                               ? size
                               : core::console::AgentObservationsView::kRows;
  for (std::size_t i = 0; i < rows; ++i) {
    // The ring's 0 is the oldest; these are the newest first.
    const mm::HeardAdvert& advert = ring->at(size - 1 - i);
    core::console::AgentObservationRow& row = out->row[i];
    hexKey(row.public_key, sizeof row.public_key, advert.publicKey);
    row.ago_s = advert.heardAtEpoch != 0 && epoch != 0
                    ? (epoch > advert.heardAtEpoch ? epoch - advert.heardAtEpoch : 0)
                    : (now_ms - advert.heardAtMillis) / 1000u;
    row.heard_at_epoch = advert.heardAtEpoch != 0 ? advert.heardAtEpoch
                                                  : (epoch > row.ago_s ? epoch - row.ago_s : 0);
    row.hops = advert.hops;
    row.snr_known = advert.snrQ4 != mm::kNoSnrQ4;
    row.snr_q4 = advert.snrQ4;
    row.rssi_known = advert.rssi != mm::kNoRssi;
    row.rssi = advert.rssi;
    const char* role = mm::roleName(advert.advType);
    row.role = role == nullptr ? "" : role;
    row.has_position = advert.hasPosition;
    // **No name.** §7: "The name inside `app_data` is something a stranger chose. Neither end
    // logs it", and every line of this console is kept in the ring `dump` prints.
  }
  out->rows = rows;
}

std::size_t IdentityProbe::pull() {
  // The drain drops an advert about this unit's own repeater, so it needs the key: refreshing
  // first is also what keeps it right across a restart of the radio.
  (void)identity_.refresh();
  observations_.useOwnKey(identity_.publicKey());
  return observations_.drain(platform_.millis());
}

}  // namespace agent
