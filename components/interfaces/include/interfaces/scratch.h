// Room for the length of one command, borrowed and handed back (task p16-01), out of the internal
// heap or - for the one thing that may ask - out of the module's external RAM (task p16-05).
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
// ESP-IDF (rail R12), because a host test hands over a block of its own, and because the room of
// a session is 64 kB where this module has PSRAM and 8 192 bytes where it has not, and neither
// `rak say` nor the session had to learn which (task p16-05).
//
// -- the external half, and what it is not ------------------------------------------------------
//
// `takeExternal()` is the **only** way anything of this firmware asks for external memory, and
// the room of a session is its only caller: rail R24 holds both ends of that sentence. Nothing
// this board needs in order to boot, to keep its radio, to update or to reach the server may come
// to depend on it - `malloc`, `new`, every task's stack, `.data` and `.bss` stay internal - and a
// board with no PSRAM, with PSRAM of the other kind, or with one that does not answer is a board
// that boots, runs, and holds its sessions in the 8 192 bytes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

class Scratch {
 public:
  virtual ~Scratch() = default;

  // `bytes` of writable room of the **internal** heap, or nullptr when there is no block that
  // large. Never zeroed: the caller writes before it reads.
  virtual void* take(std::size_t bytes) = 0;

  // Hands back exactly what take() or takeExternal() answered. A null pointer is allowed and
  // does nothing. One call for both rooms, because the allocator behind it knows which heap a
  // block came out of and a caller that had to remember could get it wrong.
  virtual void give(void* room) = 0;

  // The largest block take() could answer with now, so that a caller can say what was missing
  // instead of only that something was. 0 when it cannot tell.
  virtual std::size_t largestBlock() = 0;

  // -- the external memory ----------------------------------------------------------------------

  // What this board's external RAM is, at this moment. Three of these are what `heap` prints.
  enum class External : std::uint8_t {
    kNone = 0,    // this build was not configured for any, or this board has none
    kAsleep = 1,  // this build has some to bring up, and nothing has brought it up in this boot
    kUp = 2,      // brought up: `bytes` is its size and `free_bytes` what is free of it
    kFailed = 3,  // it was asked for in this boot and did not come up (none answered, or the
                  // other kind of chip, or one that does not answer)
  };

  struct ExternalRoom {
    External state = External::kNone;
    std::size_t bytes = 0;       // the size of it, once it is up
    std::size_t free_bytes = 0;  // ... and how much of that is free
  };

  // Read, never brings anything up: `heap` says what a board has without costing it anything.
  virtual ExternalRoom external() = 0;

  // `bytes` of **external** room, asked for by name. Brings the external RAM up at the first
  // call of a boot and at no other moment, so a wake that asks for nothing never touches it.
  // nullptr when this board has none, when it did not come up, or when it has no block that
  // large; the caller then takes what take() will give it. give() hands it back.
  virtual void* takeExternal(std::size_t bytes) = 0;
};

}  // namespace interfaces
