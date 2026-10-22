// The observation ring: what the radio heard, waiting to be posted.
//
// RAM only, and bounded - a board that is its own exit node has no journal and no disk
// (EXIT-NODES section 5, "What it cannot do"), so a long outage overwrites its own oldest
// entries rather than growing a file. The ring drops the **oldest** when it is full and counts
// every drop; the count is the heartbeat's `observationsDropped`, which is how the server is
// told that a board at a busy window was away for a while.
//
// Sixty-four entries at about 120 bytes each is 7.6 KB of the 320 KB of RAM, three batches of
// the twenty the flush sends - see tasks/phase-1/reports/02.md for the arithmetic against task
// 01's heap figure. An entry is what an advert carries and nothing else: the key, the path it
// arrived by, SNR and RSSI, and the name, position and role as decoded. Never a message's text,
// and never a frame's payload (README.md, "Rails").
//
// Host-portable: no Arduino, no MeshCore. The board fills the struct in src/mesh/AgentMesh.cpp
// and the native tests fill it by hand.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// Eight hops at the three-byte path hash: further than an advert worth reporting has travelled
// (EXIT-NODES section 12: one hop is the goal, three is diagnostic-only).
constexpr size_t kMaxObservationPathBytes = 24;

// MeshCore's own bound on a node name (helpers/ContactInfo.h: char name[32]).
constexpr size_t kMaxObservationNameLen = 32;

constexpr size_t kPublicKeyLen = 32;

// No SNR, no RSSI: the two are optional in the record and absent is not zero.
constexpr int8_t kNoSnrQ4 = INT8_MIN;
constexpr int16_t kNoRssi = INT16_MIN;

// ADV_TYPE_* of MeshCore's helpers/AdvertDataHelpers.h, and one value for "the advert did not
// say", which is what the role reads as then.
constexpr uint8_t kAdvTypeUnknown = 0xFF;

struct HeardAdvert {
  uint8_t publicKey[kPublicKeyLen] = {};
  uint32_t heardAtMillis = 0;  // the board's uptime clock, turned into a timestamp when the batch is built
  // UNIX seconds the advert was heard, when that is known **instead** of the uptime clock: 0 for
  // an advert the mesh pushed (the batch stamps it from `heardAtMillis` and the clock, as ever),
  // and the moment it was heard for one restored from retained storage after a deep sleep (task
  // p1.6-22), where the uptime clock it was heard on is gone with the sleep. The batch prefers
  // this when it is set, so a late advert is stamped with when it was heard and not with when
  // the unit woke. The mesh never sets it.
  uint32_t heardAtEpoch = 0;

  uint8_t pathBytes[kMaxObservationPathBytes] = {};
  uint8_t pathByteLen = 0;   // how much of pathBytes is real
  uint8_t pathHashSize = 1;  // 1, 2 or 3 bytes a hop
  uint8_t hops = 0;          // what the packet said, even when the path itself did not fit

  int8_t snrQ4 = kNoSnrQ4;   // SNR x 4, as a packet carries it
  int16_t rssi = kNoRssi;    // whole dBm

  uint8_t advType = kAdvTypeUnknown;
  bool hasPosition = false;
  int32_t latE6 = 0;
  int32_t lonE6 = 0;
  char name[kMaxObservationNameLen + 1] = {0};
};

// "companion" | "repeater" | "roomserver" | "sensor" | "unknown", the names the .NET agent's
// OpCodes.RoleName gives the same advert types. nullptr when the advert did not say.
const char* roleName(uint8_t advType);

class ObservationRing {
 public:
  static constexpr size_t kCapacity = 64;

  // Add one, dropping the oldest when the ring is full.
  void push(const HeardAdvert& advert);

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == kCapacity; }

  // 0 is the oldest, size()-1 the newest. Out of range is the oldest.
  const HeardAdvert& at(size_t index) const;

  // Forget the `count` oldest: what a batch the server accepted (or definitively refused) costs.
  void consume(size_t count);

  // Forget the `count` **newest**, counted as dropped: what a kept image too small for the whole
  // ring costs at the end of a wake (task p1.6-22) - the oldest are kept, because they were
  // heard first and the image is written oldest first, and the ring is made to agree with the
  // image so that `dropped()` means what the heartbeat says it means.
  void dropNewest(size_t count);
  // Drops that happened before this object existed - the count a kept image carried across a
  // sleep - so the heartbeat's figure is the unit's and not this boot's.
  void addDropped(uint32_t count) { dropped_ += count; }

  uint32_t dropped() const { return dropped_; }
  uint32_t heard() const { return heard_; }

  void clear();

 private:
  HeardAdvert items_[kCapacity];
  size_t first_ = 0;
  size_t size_ = 0;
  uint32_t dropped_ = 0;
  uint32_t heard_ = 0;
};

// A batch goes when it would hold twenty, or when the oldest entry has waited sixty seconds,
// whichever comes first (tasks/phase-1/02).
constexpr size_t kObservationBatchSize = 20;
constexpr uint32_t kObservationFlushMs = 60000;

// `oldestWaitedMs` is how long the oldest pending entry has been waiting; it is not read when
// nothing is pending.
bool observationBatchDue(size_t pending, uint32_t oldestWaitedMs);

}  // namespace mm
