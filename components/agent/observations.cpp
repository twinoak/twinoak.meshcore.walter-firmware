#include "walter_agent/observations.h"

#include <agent/Envelopes.h>

namespace agent {

namespace link = core::link;

namespace {

std::int32_t int32le(const std::uint8_t* at) {
  const std::uint32_t value = static_cast<std::uint32_t>(at[0]) |
                              (static_cast<std::uint32_t>(at[1]) << 8) |
                              (static_cast<std::uint32_t>(at[2]) << 16) |
                              (static_cast<std::uint32_t>(at[3]) << 24);
  return static_cast<std::int32_t>(value);
}

}  // namespace

bool parseAdvertData(const std::uint8_t* app_data, std::size_t size, mm::HeardAdvert* out) {
  if (out == nullptr) return false;
  // `app_len` of 0 is allowed by §7 and is an advert that said nothing: not an error, and
  // nothing to map.
  if (app_data == nullptr || size == 0) return true;
  const std::uint8_t flags = app_data[0];
  std::size_t at = 1;
  if ((flags & advert::kLatLon) != 0) {
    if (at + 8 > size) return false;
    out->hasPosition = true;
    out->latE6 = int32le(app_data + at);
    out->lonE6 = int32le(app_data + at + 4);
    at += 8;
  }
  // The two future features: their bytes are stepped over so that the name begins where the
  // builder put it. Neither has a field in the library's struct, so neither is carried - see the
  // report's gap.
  if ((flags & advert::kFeat1) != 0) {
    if (at + 2 > size) return false;
    at += 2;
  }
  if ((flags & advert::kFeat2) != 0) {
    if (at + 2 > size) return false;
    at += 2;
  }
  out->advType = static_cast<std::uint8_t>(flags & advert::kTypeMask);
  if ((flags & advert::kName) != 0 && at < size) {
    // The rest of the payload is the name (AdvertDataParser: "remainder of app_data"), bounded
    // by the field and then by the library's own UTF-8 rule, because a JSON string has to be
    // UTF-8 and this is whatever a stranger typed.
    char name[mm::kMaxObservationNameLen + 1] = {};
    std::size_t n = size - at;
    if (n > mm::kMaxObservationNameLen) n = mm::kMaxObservationNameLen;
    for (std::size_t i = 0; i < n; ++i) name[i] = static_cast<char>(app_data[at + i]);
    name[n] = 0;
    const std::size_t valid = mm::validUtf8PrefixLength(name, n);
    for (std::size_t i = 0; i < valid; ++i) out->name[i] = name[i];
    out->name[valid] = 0;
  }
  return true;
}

Observations::Observations(lc::Client& radio, LinkMesh& mesh) : radio_(radio), mesh_(mesh) {}

void Observations::useOwnKey(const std::uint8_t* key) {
  own_key_known_ = key != nullptr;
  for (std::size_t i = 0; i < link::kPubkeySize; ++i) {
    own_key_[i] = own_key_known_ ? key[i] : 0;
  }
}

void Observations::forget() {
  counters_ = ObservationCounters{};
  drops_seen_ = false;
  drops_reported_ = 0;
}

bool Observations::isOwnKey(const link::Bytes& key) const {
  if (!own_key_known_ || key.data == nullptr || key.size != link::kPubkeySize) return false;
  for (std::size_t i = 0; i < link::kPubkeySize; ++i) {
    if (key.data[i] != own_key_[i]) return false;
  }
  return true;
}

bool Observations::mapRecord(const link::Observation& record, std::uint32_t nowMs,
                             mm::HeardAdvert* out) {
  if (out == nullptr) return false;
  *out = mm::HeardAdvert{};
  if (record.pubkey.data == nullptr || record.pubkey.size != link::kPubkeySize) return false;
  for (std::size_t i = 0; i < link::kPubkeySize; ++i) out->publicKey[i] = record.pubkey.data[i];

  // §7's `age_s` is "seconds since this node heard it - measured, so it is right whatever the
  // clock said", so the moment it was heard is this clock's now less that. `heardAtEpoch` is
  // left at 0 on purpose: the library stamps a batch from `heardAtMillis` and its own clock
  // ("the mesh never sets it", agent/Observations.h), and a back-dated uptime is exactly what it
  // needs - the subtraction is unsigned and the difference the library takes is `age_s` again,
  // which is right across the wrap and right for an advert older than this boot.
  out->heardAtMillis = nowMs - record.age_s * 1000u;

  out->snrQ4 = record.snr_q4;
  // §7: `0x7FFF` is "no RSSI", and the library's own absent value is INT16_MIN.
  out->rssi = record.rssi == link::kRssiUnknown ? mm::kNoRssi : record.rssi;

  out->hops = link::pathHopCount(record.path_len);
  out->pathHashSize = link::pathHashSize(record.path_len);
  std::size_t bytes = record.path.size;
  if (bytes > mm::kMaxObservationPathBytes) bytes = mm::kMaxObservationPathBytes;
  for (std::size_t i = 0; i < bytes; ++i) out->pathBytes[i] = record.path.data[i];
  out->pathByteLen = static_cast<std::uint8_t>(bytes);

  return parseAdvertData(record.app_data.data, record.app_data.size, out);
}

std::size_t Observations::drain(std::uint32_t nowMs) {
  mm::ObservationRing* ring = mesh_.ring();
  std::size_t kept = 0;
  for (std::uint8_t pull = 0; pull < kMaxPullsPerWake; ++pull) {
    const lc::Answer<link::ObsPullResponse> answer = radio_.obsPull(link::kMaxObservations);
    if (!answer.ok()) {
      ++counters_.faults;
      break;
    }
    ++counters_.pulls;
    const link::ObsPullResponse& page = answer.message;
    counters_.remaining = page.remaining;
    counters_.pull_dropped += page.dropped;
    const std::size_t count =
        page.count < link::kMaxObservations ? page.count : link::kMaxObservations;
    for (std::size_t i = 0; i < count; ++i) {
      ++counters_.records;
      if (isOwnKey(page.records[i].pubkey)) {
        ++counters_.own_key;
        continue;
      }
      mm::HeardAdvert advert;
      if (!mapRecord(page.records[i], nowMs, &advert)) {
        ++counters_.unreadable;
        continue;
      }
      if (ring != nullptr) ring->push(advert);
      ++counters_.kept;
      ++kept;
    }
    // Records handed out are gone from the RAK's ring (§7), so the loop ends when it says there
    // is nothing left - or when it answered fewer than it could, which means the same thing.
    if (page.remaining == 0) break;
  }
  reconcileDrops();
  return kept;
}

void Observations::reconcileDrops() {
  const std::uint32_t cumulative = mesh_.radioObsDropped();
  if (!drops_seen_) {
    // The first reading of a boot is a baseline and not a loss: the RAK's count is since *its*
    // boot, and a Walter that woke to a radio which has been up for a week must not report that
    // week's drops as this wake's. What was lost while this unit slept is the radio's own
    // business and nobody could have collected it.
    drops_seen_ = true;
    drops_reported_ = cumulative;
    counters_.radio_dropped = cumulative;
    return;
  }
  if (cumulative < drops_reported_) {
    // The radio restarted and started counting again: the new figure is the whole of what has
    // been lost since.
    drops_reported_ = 0;
  }
  const std::uint32_t grown = cumulative - drops_reported_;
  drops_reported_ = cumulative;
  counters_.radio_dropped = cumulative;
  if (grown == 0) return;
  mm::ObservationRing* ring = mesh_.ring();
  if (ring != nullptr) ring->addDropped(grown);
}

}  // namespace agent
