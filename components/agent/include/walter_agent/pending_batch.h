// Since which wake a batch has been waiting for its answer (task p15-04).
//
// The library keeps the batch itself. A batch of adverts that reached meshmaster.dk and whose
// answer never came must go out again under the id it was built with, because the server
// de-duplicates on that id; the id lived only in `mm::AgentLoop`'s RAM until agent-core 0.7.3
// (the exit-node firmware's task p1.13-55), and this board's RAM does not survive a sleep, so
// every wake posted everything it still held as a new batch and the server stored it again - on
// 16 November 2026 batches of 13, 14, 15, 16, 17 and 18 adverts in six wakes running, and 19
// different adverts in the hundred newest rows. Since that release the id and how many of the
// ring's oldest adverts the batch holds are in the image a sleep keeps (`agent/KeptImage.h`,
// version 2), and `AgentLoop::pendingBatchId()` and `pendingBatchCount()` are them.
//
// **What is left for this board is the wake.** `agent obs` says that a batch is waiting, how
// many adverts it holds and since which wake - and that last number is nobody's but this
// board's: the library has no notion of a wake, and the image's layout is the library's and is
// not edited here. So one more small value of the settings store, beside the two `agent_calls`
// has and the one `agent_flood` has, and for the same reasons (`walter_agent/call_log.h`, *where
// it is kept*): RTC memory has no slot left (tasks/phase-3/reports/05.md) and the figure has to
// survive a power cut, which RTC memory does not.
//
// **What it costs the flash.** The value is written only when the answer this object gives
// changes - a batch that starts waiting, a batch that stops, a batch replaced by another one -
// which is only ever a wake whose observations call failed or the wake after one. A unit whose
// posts are answered never writes this value in its life, and a unit whose answer is lost at
// every wake writes it once and then not again until the batch goes.
//
// **Which batch the stored wake is about.** The id itself is in the image and not here: 36 bytes
// for something the loop already holds. What is stored beside the wake is a 32-bit fingerprint
// of it (FNV-1a over the id's bytes), which is what says *the batch waiting now is the batch the
// stored wake is about*. A batch the loop forgot and built again under a new id has a new
// fingerprint and so a new wake, which is what *since* has to mean. Two ids whose fingerprints
// collide would leave the figure one batch too early; it is one line of a console command and
// one in a report, and 36 bytes of a store whose values are 160 characters are better spent on
// what a person cannot otherwise get at.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/AgentLoop.h>

#include "interfaces/key_value.h"

namespace agent {

// The key, and what is kept under it: a version byte, the wake's number and the fingerprint of
// the id - little-endian, the shape `agent_flood` and `agent_wakes` have, read and written the
// same way.
constexpr const char kPendingBatchKey[] = "agent_batch";
constexpr std::uint8_t kPendingBatchVersion = 1;
constexpr std::size_t kPendingBatchBytes = 1 + sizeof(std::uint16_t) + sizeof(std::uint32_t);
constexpr std::size_t kPendingBatchHexBytes = 2 * kPendingBatchBytes + 1;
static_assert(kPendingBatchHexBytes - 1 <= interfaces::kKeyValueMax,
              "the waiting batch's wake has to fit one value of the store");

// Saturating 16-bit counters, for the reason `core::cycle::WakeCounterWrites` gives: this object
// is in static storage and the margin the main task's stack is found in is counted in tens of
// bytes (tools/heap_at_start.py).
struct PendingBatchWrites {
  std::uint16_t writes = 0;   // values written to the store, erasures among them
  std::uint16_t refused = 0;  // a stored value of another version or shape
};

class PendingBatch {
 public:
  // `store` may be null, or not ready: a build or a test without a settings store says *a batch
  // is waiting* and cannot say since when, which is what every total of this firmware does
  // without one.
  explicit PendingBatch(interfaces::KeyValue* store);

  // Once a boot, before the first wake, and **after the kept image has been read back**: the
  // loop whose pending batch this is about, and the wake read back from the store. A null loop
  // is a build with no agent, and then nothing is ever waiting.
  void begin(const mm::AgentLoop* loop);
  bool begun() const { return begun_; }
  // Whether a stored wake was read back: false is a first boot, a store that would not open, a
  // value of another shape, or a unit that has never had an answer go missing.
  bool survived() const { return survived_; }

  // The wake's number as the duty cycle counts it, before the exchange - the same number
  // `CallLog::atWake` is given, so that a batch and the failed call that left it waiting are
  // read against one list.
  void atWake(std::uint32_t wake);

  // After the wake, where the loop's own state is saved: what the loop is holding now against
  // what the store says. A batch that has just started waiting takes this wake's number; one
  // that is still the same batch keeps the number it had; one that is gone takes the value with
  // it. Nothing is written here - `save()` is.
  void note();

  // The value to the store, **only when the answer changed**. True when there was nothing to
  // write or the write (or the erasure) went through.
  bool save();

  // What `agent obs` and the report's `observations` section print. The count and the id are the
  // loop's own, read at the moment they are asked; the wake is this object's.
  bool waiting() const;
  std::size_t count() const;
  const char* id() const;
  // Since which wake it has been waiting, and whether that is known at all: a batch whose
  // fingerprint is not the stored one - a store with nothing in it, a value refused, a batch
  // this boot built after the stored one went - has no wake to name.
  bool sinceKnown() const;
  std::uint16_t sinceWake() const { return since_wake_; }

  const PendingBatchWrites& writes() const { return writes_; }

  // Everything forgotten and the key erased: the bench, and a test.
  void forget();

 private:
  void decode(const char* hex);
  // FNV-1a, 32 bits, over the id's bytes: the fingerprint stored beside the wake. 0 is "no
  // batch", so an id that hashes to 0 is stored as 1 - which costs nothing but a comment.
  static std::uint32_t fingerprintOf(const char* id);

  interfaces::KeyValue* store_;
  const mm::AgentLoop* loop_ = nullptr;
  // The batch the stored wake is about, and that wake. `stored_tag_` is 0 when none is.
  std::uint32_t stored_tag_ = 0;
  std::uint16_t since_wake_ = 0;
  std::uint16_t wake_ = 0;  // what `atWake` was last told
  bool begun_ = false;
  bool survived_ = false;
  bool dirty_ = false;
  PendingBatchWrites writes_;
};

}  // namespace agent
