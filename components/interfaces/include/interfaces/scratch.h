// Room for the length of one command, borrowed and handed back (task p16-01).
//
// This firmware's own code allocates nothing (every `no_allocation` check in host_test/) and
// everything it needs is a member of some object a reader can size. One thing does not fit that
// rule: a scanner's lines. `spec/LINK-PROTOCOL.md` §9's NoiseScope speaks NDJSON on the box
// link, and one `sweep` line of the band is about 4 kB where the link client keeps 320 bytes of
// a line - so whoever wants the lines whole has to find the room for them, and room that size
// cannot be a static of a firmware whose start-up margin is counted in kilobytes
// (`tools/heap_at_start.py`, rail R22).
//
// So it is borrowed for the length of a command and given back, which is the one shape that
// costs nothing when nobody is asking: `rak say` takes 8 192 bytes, prints what came, and gives
// them back before the prompt.
//
// It is an interface and not a call to the allocator because core/ may include nothing of
// ESP-IDF (rail R12), because a host test hands over a block of its own, and because task
// p16-05 gives a unit with PSRAM a much larger room through this same seam without a line of
// core/ changing.
#pragma once

#include <cstddef>

namespace interfaces {

class Scratch {
 public:
  virtual ~Scratch() = default;

  // `bytes` of writable room, or nullptr when there is no block that large. Never zeroed: the
  // caller writes before it reads.
  virtual void* take(std::size_t bytes) = 0;

  // Hands back exactly what take() answered. A null pointer is allowed and does nothing.
  virtual void give(void* room) = 0;

  // The largest block this could answer with now, so that a caller can say what was missing
  // instead of only that something was. 0 when it cannot tell.
  virtual std::size_t largestBlock() = 0;
};

}  // namespace interfaces
