// What outlives a sleep: the seam for a store the consumer chooses (task p1.6-22).
//
// A result the server did not take is kept in RAM and posted again - on a Heltec, which does not
// sleep, that is the whole story (EXIT-NODES §5, *offline is normal*). On a unit that deep-sleeps
// between wakes the RAM goes with the sleep: the result is lost and its job stays leased until the
// server's lease lapses, so the promise *no job left leased and silent* held only within one boot.
// The adverts a wake could not post went the same way. `WakeState` cannot carry them - twelve
// kilobytes of results against an RTC memory that is nearly full (the Walter's phase-3 review) -
// so this is a second, optional seam: **one image a wake**, written by the loop into whatever the
// consumer has (a flash region, an NVS blob, a file - the library neither knows nor includes any
// of them), and read back by the loop after the next boot.
//
// The surface is the smallest that holds: how many bytes the store takes, write one image whole,
// read it back. Nothing else - no slots, no offsets, no partial writes. The image's layout, its
// version and its checksum are the library's (agent/KeptImage.h); the store carries bytes and
// says how many it can carry.
//
//   capacity()   the most bytes an image may be. The loop writes no image larger than this (nor
//                larger than the scratch it was handed, AgentLoop::setRetained), and sizes what it
//                keeps to it: the kept results first, oldest first, as many whole ones as fit; then
//                the ring's waiting adverts, as many as fit after them. What does not fit is
//                dropped from RAM too and counted in the counters the heartbeat already carries
//                (`resultsDropped`, `observationsDropped`), so that the buffer and the image agree
//                and a loss is a figure on the exit node's page and not a surprise
//   write()      one image, `len` bytes, **whole or not at all**. True when it is stored. A torn
//                write - a power cut mid-way - is not the store's problem to detect: the image
//                carries its length and a checksum, and a torn one is refused whole at the next
//                read, counted and logged
//   read()       the image last written, into `out`, at most `cap` bytes; how many were read, or
//                0 when there is none (a first boot, an erased region). A store may hand back more
//                than the image is long - a whole flash region, say - because the image says its
//                own length and the loop reads no further
//
// **Wear.** A wake writes at most one image, and none when what it would hold is what the store
// already holds - in particular none when nothing is kept and the previous image was already
// empty. A unit whose wakes post everything writes nothing; a unit on a metered plan that keeps a
// result now and then writes a few images a day. Size a flash region for that, not for a write a
// wake.
//
// **Nothing secret goes to storage.** The image holds result bodies as they would be posted, and
// the adverts as the ring holds them - a public key, a path, a name, a position, a role. The
// runner's rule already keeps every credential out of a result (agent/Credentials.h: a password
// is in a job's fixed buffer for the job and wiped before the result is written; a `password.set`
// records `<set>` and never the value), so **the image may hold nothing a result does not**, and
// test/test_loop holds that on the image's own bytes with the fixtures' sample passwords. A store
// that is read back by a person with the flash in hand learns what the server would have.
//
// The Heltec passes none and is unchanged: `loop()` never reads or writes one, and nothing of
// this is linked into its image.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

class IRetained {
 public:
  virtual ~IRetained() = default;

  // The most bytes an image may be. Constant for the life of the store.
  virtual size_t capacity() const = 0;

  // Store `len` bytes as the one image, replacing whatever was there. True when stored whole.
  virtual bool write(const uint8_t* image, size_t len) = 0;

  // The image last stored, into `out`: how many bytes were put there (at most `cap`), or 0 when
  // the store holds none.
  virtual size_t read(uint8_t* out, size_t cap) = 0;
};

}  // namespace mm
