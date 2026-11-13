#include "walter_agent/identity.h"


namespace agent {

namespace link = core::link;

namespace {

// A str8 of the link copied into a fixed field, truncated and always terminated. The link client
// already truncates HELLO's three strings into the session (lc::kVersionMax), so this is a copy
// of a copy and cannot grow.
void copyText(char* out, std::size_t cap, const char* text) {
  if (cap == 0) return;
  out[0] = 0;
  if (text == nullptr) return;
  std::size_t i = 0;
  for (; i + 1 < cap && text[i] != 0; ++i) out[i] = text[i];
  out[i] = 0;
}

void copyView(char* out, std::size_t cap, link::Text text) {
  if (cap == 0) return;
  std::size_t i = 0;
  for (; i + 1 < cap && i < text.size && text.data[i] != 0; ++i) out[i] = text.data[i];
  out[i] = 0;
}

char hexDigit(std::uint8_t nibble) {
  return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// One unsigned decimal, with an optional fractional part, out of `text` at `*at`. The value is
// built as an integer mantissa and one division, so that `869.618` is the same double the
// server's own `869.618` is: both are the nearest double to that decimal, because a correctly
// rounded division of two exactly representable integers is the nearest double to the quotient.
// False for anything that is not a number.
bool parseNumber(const char* text, std::size_t size, std::size_t* at, double* out) {
  std::size_t i = *at;
  if (i >= size || !isDigit(text[i])) return false;
  std::uint64_t mantissa = 0;
  std::uint64_t scale = 1;
  for (; i < size && isDigit(text[i]); ++i) {
    if (mantissa > 0x0020000000000000ull) return false;  // longer than a double can hold exactly
    mantissa = mantissa * 10u + static_cast<std::uint64_t>(text[i] - '0');
  }
  if (i < size && text[i] == '.') {
    ++i;
    for (; i < size && isDigit(text[i]); ++i) {
      if (mantissa > 0x0020000000000000ull) return false;
      mantissa = mantissa * 10u + static_cast<std::uint64_t>(text[i] - '0');
      scale *= 10u;
    }
  }
  *at = i;
  *out = static_cast<double>(mantissa) / static_cast<double>(scale);
  return true;
}

bool expect(const char* text, std::size_t size, std::size_t* at, char c) {
  if (*at >= size || text[*at] != c) return false;
  ++*at;
  return true;
}

}  // namespace

// -- the intent --------------------------------------------------------------------------------

int intent::forBand(core::power::Band band) {
  switch (band) {
    case core::power::Band::kPlenty: return kPlenty;
    case core::power::Band::kNormal: return kNormal;
    case core::power::Band::kSaving: return kSaving;
    case core::power::Band::kCritical: return kCritical;
    case core::power::Band::kUnknown: return kUnknown;
    case core::power::Band::kCount: break;
  }
  return kUnknown;
}

mm::WakeIntent intentFor(core::power::Band band, std::uint32_t interval_seconds) {
  mm::WakeIntent out;
  out.nextCheckInS = interval_seconds;
  out.maxTransactionsThisWake = intent::forBand(band);
  return out;
}

mm::WakeIntent intentFor(const core::power::Decision& decision) {
  return intentFor(decision.band, decision.interval_seconds);
}

// -- the radio block ---------------------------------------------------------------------------

bool parseRadioLine(const char* text, std::size_t size, mm::RadioParameters* out) {
  if (text == nullptr || out == nullptr) return false;
  std::size_t at = 0;
  // The repeater's console prefixes a `get` reply with "> " (CommonCLI::handleGetCmd at
  // d9296435). A reply without it is read all the same: the prefix is the console's manner and
  // not the value.
  while (at < size && (text[at] == ' ' || text[at] == '>')) ++at;
  double freq = 0, bw = 0, sf = 0, cr = 0;
  if (!parseNumber(text, size, &at, &freq)) return false;
  if (!expect(text, size, &at, ',')) return false;
  if (!parseNumber(text, size, &at, &bw)) return false;
  if (!expect(text, size, &at, ',')) return false;
  if (!parseNumber(text, size, &at, &sf)) return false;
  if (!expect(text, size, &at, ',')) return false;
  if (!parseNumber(text, size, &at, &cr)) return false;
  // Four numbers, and all four have to be numbers a radio could be programmed with: a reply we
  // half understood is no radio block at all, which the heartbeat leaves out. The bounds are the
  // ones the repeater's own `set radio` enforces (CommonCLI.cpp at d9296435: 150..2500 MHz,
  // 7..500 kHz, SF 5..12, CR 5..8), read as a sanity check and not as a policy of ours.
  if (freq < 150.0 || freq > 2500.0 || bw < 7.0 || bw > 500.0) return false;
  if (sf < 5.0 || sf > 12.0 || cr < 5.0 || cr > 8.0) return false;
  out->freqMHz = freq;
  out->bwKHz = bw;
  out->sf = static_cast<int>(sf);
  out->cr = static_cast<int>(cr);
  return true;
}

// -- the identity ------------------------------------------------------------------------------

Identity::Identity(lc::Client& radio, const char* version) : radio_client_(radio) {
  identity_.agentVersion = version == nullptr ? "" : version;
  identity_.transport = kTransport;
  identity_.board = kBoardName;
  identity_.companionFirmware = nullptr;
  forget();
}

int addRadioIdentityLost(mm::ErrorCounter* out, int count, int capacity, bool lost) {
  if (!lost || out == nullptr || count < 0 || count >= capacity) return count;
  out[count] = mm::ErrorCounter{kRadioIdentityLostKey, 1};
  return count + 1;
}

int addWakeCounters(mm::ErrorCounter* out, int count, int capacity,
                    const core::cycle::WakeTotals& totals) {
  // All four or none: a body with three of them would read on the page as a counter that had
  // stopped moving, which is the opposite of what they are for.
  if (out == nullptr || count < 0 || count + core::cycle::kWakeCounterCount > capacity) return count;
  out[count + 0] = mm::ErrorCounter{kWakesTimerKey, static_cast<long long>(totals.timer)};
  out[count + 1] = mm::ErrorCounter{kWakesRadioKey, static_cast<long long>(totals.radio)};
  out[count + 2] = mm::ErrorCounter{kWakesSpuriousKey, static_cast<long long>(totals.spurious)};
  out[count + 3] = mm::ErrorCounter{kWakesSilentKey, static_cast<long long>(totals.silent)};
  return count + core::cycle::kWakeCounterCount;
}

void Identity::forget() {
  key_known_ = false;
  state_ = link::IdentityState::kNotMadeYet;
  caps_lost_ = false;
  for (std::size_t i = 0; i < link::kPubkeySize; ++i) key_[i] = 0;
  key_hex_[0] = 0;
  name_[0] = 0;
  firmware_[0] = 0;
  board_[0] = 0;
  upstream_[0] = 0;
  boot_id_ = 0;
  boot_seen_ = false;
  radio_ = RadioBlock{};
  identity_.companionFirmware = nullptr;
  identity_.radio = mm::RadioParameters{};
}

bool Identity::open() {
  // A session first when there is none (task p8-04): one HELLO, the link client's own three
  // tries, and then whatever refresh() has to read. A radio that does not answer leaves this
  // unit with no key, which is what shuts the local door and - because `LinkMesh::supports()`
  // asks for the key - takes every mesh job out of the heartbeat's list for this wake.
  if (!radio_client_.session().up) {
    ++counters_.sessions_asked;
    if (!radio_client_.hello().ok()) ++counters_.no_session;
  }
  return refresh();
}

bool Identity::refresh() {
  const lc::Session& session = radio_client_.session();
  if (!session.up) {
    // No session: nothing is known and nothing is asked. Opening one is `HELLO`, and that is
    // whoever runs the wake's (the supervisor's check at step 2, `open()` above, or `rak hello`).
    if (boot_seen_) forget();
    return false;
  }
  if (boot_seen_ && session.boot_id != boot_id_) {
    // §5.1: a changed `boot_id` means the radio restarted, so every piece of per-boot state we
    // hold about it is gone - the key it answered, its settings, the version it reported.
    ++counters_.boot_changes;
    forget();
  }
  boot_seen_ = true;
  boot_id_ = session.boot_id;
  // These three cost nothing: the link client copied them out of HELLO when the session opened.
  copyText(firmware_, sizeof firmware_, session.fw_version);
  copyText(board_, sizeof board_, session.board);
  copyText(upstream_, sizeof upstream_, session.upstream);

  // The key is asked for until it is known, and then not again until the radio restarts: a RAK
  // that holds no application yet answers no key, and the heartbeat of the wake after the one
  // where it gets one has to carry it (tasks/phase-4/FOLLOWUPS.md's `[04]`). One §5.6 request a
  // wake while a unit has no radio identity is what that costs.
  bool asked = false;
  if (!key_known_) {
    asked = true;
    (void)readKey();
  }
  if (key_known_ && !radio_.known) {
    asked = true;
    (void)readRadio();
  }
  if (asked) ++counters_.refreshes;

  // The library reads `companionFirmware` and `radio` at every heartbeat and writes neither
  // unless there is a key (agent/AgentLoop.cpp's `buildHeartbeat`), but a struct that said
  // something about a radio we have not identified would be a struct a later release could
  // start sending: so it says nothing.
  identity_.companionFirmware = key_known_ && firmware_[0] != 0 ? firmware_ : nullptr;
  identity_.radio = radio_.known ? radio_.parameters : mm::RadioParameters{};
  return key_known_;
}

bool Identity::readKey() {
  const lc::Answer<link::IdentityResponse> answer = radio_client_.identity();
  const link::Bytes key = answer.message.pubkey;
  // The three states out of this one answer and the session it came from, in the one place that
  // decides them (task p10-03, core/link/radio_identity.h). A radio with no application, or one
  // that has not made an identity yet, answers a key of all zeros - which is not a key, and a
  // unit that sent it would be asking the server to know it by nothing
  // (tasks/phase-4/FOLLOWUPS.md's `[04]`); a radio that has **lost** its identity answers
  // ERR_STATE and needs a person, not an install.
  link::IdentityReading reading;
  reading.key = answer.ok() && link::keyIsReal(key.data, key.size);
  reading.refused_state =
      answer.fault == lc::Fault::kRefused && answer.status == link::status::kState;
  reading.caps = radio_client_.session().caps;
  state_ = link::identityState(reading);
  caps_lost_ = link::capsSayIdentityLost(reading.caps);
  // `identity_reads` is requests that were *answered*, as it always was - a radio that answered
  // a key of all zeros answered - and `no_identity` is the ones that left this end with no key.
  if (answer.ok()) ++counters_.identity_reads;
  if (!reading.key) {
    ++counters_.no_identity;
    if (state_ == link::IdentityState::kLost) ++counters_.identity_lost;
    return false;
  }
  for (std::size_t i = 0; i < link::kPubkeySize; ++i) {
    key_[i] = key.data[i];
    key_hex_[i * 2] = hexDigit(static_cast<std::uint8_t>(key.data[i] >> 4));
    key_hex_[i * 2 + 1] = hexDigit(static_cast<std::uint8_t>(key.data[i] & 0x0F));
  }
  key_hex_[2 * link::kPubkeySize] = 0;
  // The advert name is a stranger's only on the mesh; this one is the unit's own repeater's, and
  // it is still never logged (spec/SYSTEM.md S-12) - `agent id` prints it because a person at
  // the console is looking at their own box.
  copyView(name_, sizeof name_, answer.message.name);
  key_known_ = true;
  return true;
}

bool Identity::readRadio() {
  const lc::Answer<link::CliResponse> answer = radio_client_.cli(link::text(kRadioQuery));
  if (!answer.ok()) return false;
  mm::RadioParameters parameters;
  const link::Text reply = answer.message.text;
  if (!parseRadioLine(reply.data, reply.size, &parameters)) {
    ++counters_.unreadable;
    return false;
  }
  ++counters_.radio_reads;
  radio_.parameters = parameters;
  radio_.known = true;
  return true;
}

// -- the heartbeat -----------------------------------------------------------------------------

void Identity::fillHeartbeat(mm::Heartbeat* out, const mm::IMesh* mesh,
                             const mm::IJobExtension* extension, const HeartbeatExtras& extras,
                             const mm::WakeIntent& intent, mm::ErrorCounter* errors,
                             int error_capacity) {
  if (out == nullptr) return;
  *out = mm::Heartbeat{};
  out->agentVersion = identity_.agentVersion;
  out->transport = identity_.transport;
  out->at = extras.at;
  out->companionPublicKey = publicKeyHex();
  out->companionFirmware = key_known_ ? identity_.companionFirmware : nullptr;
  out->hasRadio = key_known_ && radio_.known;
  if (out->hasRadio) out->radio = radio_.parameters;

  // The thirteen the library has a handler for, less any the mesh says it cannot carry, then the
  // extension's: the same loop as `AgentLoop::buildHeartbeat`, because the server routes by this
  // list and a second list that disagreed would be a job sent to a unit that refuses it.
  int count = 0;
  if (mesh != nullptr) {
    for (int i = 0; i < mm::kJobTypeCount; ++i) {
      if (mesh->supports(mm::kJobTypes[i])) job_types_[count++] = mm::kJobTypes[i];
    }
    if (extension != nullptr) {
      const int extra = extension->typeCount() < mm::kMaxExtensionTypes ? extension->typeCount()
                                                                        : mm::kMaxExtensionTypes;
      for (int i = 0; i < extra; ++i) job_types_[count++] = extension->type(i);
    }
  }
  out->hasJobTypes = true;
  out->jobTypes = job_types_;
  out->jobTypeCount = count;

  out->hasBudget = true;
  out->budget = extras.budget;
  out->companionConnected = key_known_;
  out->queueDepth = extras.queue_depth;
  out->board = identity_.board;

  if (errors != nullptr && error_capacity >= kLibraryErrorCount) {
    errors[0] = mm::ErrorCounter{"overheard", extras.overheard};
    errors[1] = mm::ErrorCounter{"wifiReconnects", extras.wifi_reconnects};
    errors[2] = mm::ErrorCounter{"httpFailures", extras.http_failures};
    errors[3] = mm::ErrorCounter{"observationsDropped", extras.observations_dropped};
    errors[4] = mm::ErrorCounter{"resultsDropped", extras.results_dropped};
    errors[5] = mm::ErrorCounter{"reboots", extras.reboots};
    out->errors = errors;
    // And this board's own, while the radio refuses (task p10-03): the server's `errors` is an
    // open key space, so the one thing meshmaster.dk could not see - a radio that has lost its
    // identity, which reads on the page exactly like one awaiting its first install - costs no
    // new field. Nothing is added when the radio has not refused, so an ordinary unit's
    // heartbeat is byte for byte what it was.
    int written = addRadioIdentityLost(errors, kLibraryErrorCount, error_capacity, identityLost());
    // And the four of task p13-02, at **every** heartbeat: what woke this unit is not a fault
    // and is not written only while something is wrong. They go behind `radioIdentityLost` so
    // that a unit whose radio refuses keeps that counter where a reader of p10-03's bodies
    // already found it.
    written = addWakeCounters(errors, written, error_capacity, extras.wakes);
    out->errorCount = written;
  }

  // The two of an agent that paces itself, written only when set - as the library writes them.
  out->hasNextCheckInS = intent.nextCheckInS > 0;
  out->nextCheckInS = static_cast<int>(intent.nextCheckInS);
  out->hasMaxTransactionsThisWake = intent.maxTransactionsThisWake > 0;
  out->maxTransactionsThisWake = intent.maxTransactionsThisWake;
}

std::size_t Identity::writeHeartbeat(const mm::IMesh* mesh, const mm::IJobExtension* extension,
                                     const HeartbeatExtras& extras, const mm::WakeIntent& intent,
                                     char* out, std::size_t cap) {
  mm::Heartbeat heartbeat;
  mm::ErrorCounter errors[kErrorCount];
  fillHeartbeat(&heartbeat, mesh, extension, extras, intent, errors, kErrorCount);
  return mm::writeHeartbeat(heartbeat, out, cap);
}

}  // namespace agent
