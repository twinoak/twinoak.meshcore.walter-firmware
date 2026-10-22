// What the radio heard, into the library's ring (task p4-04): `spec/LINK-PROTOCOL.md` §7's
// `OBS_PULL` drained at every wake and mapped record for record into `mm::ObservationRing`'s
// `HeardAdvert` (components/agent-core, agent/Observations.h), which the library's loop posts as
// `POST /agent/v1/observations` batches. The report of this task has the mapping table and its
// two gaps.
//
// What is here and nowhere else:
//
//   - **the advert's own bytes are decoded here.** §7 hands `app_data` over "untouched: flags,
//     position, name", which is MeshCore's advert payload, and the library's struct wants the
//     type, the position and the name as fields. The layout is
//     `AdvertDataBuilder::encodeTo`/`AdvertDataParser` of MeshCore's
//     `src/helpers/AdvertDataHelpers.cpp` at `d9296435`: one flags byte whose low nibble is the
//     `ADV_TYPE_*`, then latitude and longitude as little-endian `int32` of degrees x 1e6 when
//     `0x10` is set, two bytes each for the two future features when `0x20` and `0x40` are, and
//     then the rest of the payload is the name when `0x80` is.
//   - **records of this unit's own key are dropped before they are kept.** The RAK hears its own
//     repeater advertise and the repeater *is* this exit node (spec/SYSTEM.md S-1), so an
//     observation of it is this unit telling the server about itself; meshmaster.dk ignores one
//     (its task p4.14-119) and the bytes are not worth an attach either way.
//   - **a stranger's name is never logged.** §7: "The name inside `app_data` is something a
//     stranger chose. Neither end logs it." It is copied into the ring, bounded by the library's
//     own `validUtf8PrefixLength` because a JSON string has to be UTF-8, and it reaches no log
//     line and no console line here (spec/SYSTEM.md S-12; `agent obs` prints the key, the hops
//     and the signal and **not** the name).
//   - **the drops are the radio's, counted once.** §6.4's `obs_dropped` is what the RAK's own
//     ring lost since it booted, and the library's `observationsDropped` is what the heartbeat
//     carries for its ring - the same thing one hop earlier, so the increase since the last wake
//     is added to the library's count through `ObservationRing::addDropped` and the figure the
//     server sees is the unit's and not one ring's. §7's per-pull `dropped` is recorded here for
//     the console and **not** added, because adding both would count a loss twice.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/Observations.h>

#include "core/link/client/client.h"
#include "core/link/codec/messages.h"
#include "walter_agent/link_mesh.h"

namespace agent {

// MeshCore's advert payload (src/helpers/AdvertDataHelpers.h at d9296435).
namespace advert {
constexpr std::uint8_t kTypeMask = 0x0F;  // ADV_TYPE_*: 1 chat, 2 repeater, 3 room, 4 sensor
constexpr std::uint8_t kLatLon = 0x10;    // ADV_LATLON_MASK: two int32 of degrees x 1e6
constexpr std::uint8_t kFeat1 = 0x20;     // ADV_FEAT1_MASK: u16, "FUTURE"
constexpr std::uint8_t kFeat2 = 0x40;     // ADV_FEAT2_MASK: u16, "FUTURE"
constexpr std::uint8_t kName = 0x80;      // ADV_NAME_MASK: the rest of the payload
}  // namespace advert

// How many `OBS_PULL` requests one wake makes at most. §7 answers "as many as fit in one
// payload", which is five of the smallest record (`link::kMaxObservations`), and the RAK's ring
// is "at least 32"; sixteen pulls is eighty records, more than the library's ring holds
// (`mm::ObservationRing::kCapacity`, 64), so the bound is never what ends a drain of a healthy
// radio - it is there so that a radio answering `remaining` for ever cannot hold a wake open.
constexpr std::uint8_t kMaxPullsPerWake = 16;

struct ObservationCounters {
  std::uint32_t pulls = 0;         // §7 requests that answered
  std::uint32_t faults = 0;        // ones that did not (no session, no capability, link down)
  std::uint32_t records = 0;       // records handed over
  std::uint32_t kept = 0;          // of those, ones that went into the library's ring
  std::uint32_t own_key = 0;       // of those, ones about this unit's own repeater: dropped
  std::uint32_t unreadable = 0;    // ones whose `app_data` is not an advert payload
  std::uint32_t pull_dropped = 0;  // §7's per-pull `dropped`, summed: for the console only
  std::uint32_t radio_dropped = 0; // §6.4's `obs_dropped` as last read: what the ring was told
  std::uint8_t remaining = 0;      // what the last answer said was still in the RAK's ring
};

class Observations {
 public:
  // The mesh is where the library's ring is (`AgentLoop::begin` hands it to `mm::IMesh`), so
  // this writes through it rather than holding a ring of its own: there is one ring and it is
  // the loop's.
  Observations(lc::Client& radio, LinkMesh& mesh);

  // This unit's own repeater's key, 32 bytes, or nullptr for "the radio has no identity yet".
  // Task p4-04's `Identity` is where it comes from.
  void useOwnKey(const std::uint8_t* key);

  // One wake's drain: §7 until the RAK's ring is empty, bounded by kMaxPullsPerWake, every
  // record mapped and kept. `nowMs` is the platform's millisecond clock - the library stamps a
  // batch from it and §7's `age_s`, which is measured by the radio and so is right whatever
  // either clock said. Returns how many records went into the ring.
  std::size_t drain(std::uint32_t nowMs);

  // §6.4's `obs_dropped`, as `LinkMesh` last read it: the increase since the last call is added
  // to the library's ring count. Called by `drain()`, and separately by a wake that read the
  // counters without pulling.
  void reconcileDrops();

  const ObservationCounters& counters() const { return counters_; }
  // The ring this writes into, or nullptr when the loop has not handed one over yet.
  mm::ObservationRing* ring() const { return mesh_.ring(); }

  void forget();

 private:
  bool mapRecord(const core::link::Observation& record, std::uint32_t nowMs, mm::HeardAdvert* out);
  bool isOwnKey(const core::link::Bytes& key) const;

  lc::Client& radio_;
  LinkMesh& mesh_;
  bool own_key_known_ = false;
  std::uint8_t own_key_[core::link::kPubkeySize] = {};
  bool drops_seen_ = false;
  std::uint32_t drops_reported_ = 0;
  ObservationCounters counters_;
};

// `app_data` into the three fields the library's struct has for it. False when the payload is
// not an advert payload at all (shorter than its own flags say), and `out`'s three fields are
// then left as they were - which the heartbeat reads as "the advert did not say".
bool parseAdvertData(const std::uint8_t* app_data, std::size_t size, mm::HeardAdvert* out);

}  // namespace agent
