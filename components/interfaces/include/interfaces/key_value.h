// Where the settings live (task p3-03): a handful of short strings under names, kept across a
// power cut. On the ESP32-S3 that is NVS in the `nvs` partition, which `partitions.csv` puts
// **above** the application slots so that a factory image cannot reach it (docs/ARCHITECTURE.md
// §11); on the host, a map the test can fill with rubbish, make read-only or take away.
//
// Deliberately smaller than NVS: strings only, no blobs, no integers, no iteration. The types,
// the ranges and the fallbacks are core/settings/keys.h's, which is host-portable and tested
// there; this is the part that touches the flash.
//
// There is no eraseAll(): `erase confirm` forgets the keys of the table one by one
// (core/settings/settings.cpp), so that "the settings and **only** the settings" is the shape of
// the code and not a promise about a namespace.
//
// **Not encrypted.** Flash encryption is the owner's choice and is not assumed: without it, a
// person with the board in hand and a cable can read the SIM's PIN and the server's token out of
// this partition. Said out loud in docs/console.md rather than hidden, as the agent firmware
// says it.
#pragma once

#include <cstddef>

namespace interfaces {

// The longest name a key may have. NVS's own limit is 15 characters plus the terminator
// (nvs.h, NVS_KEY_NAME_MAX_SIZE 16), and that is the limit here: core/settings/keys.h holds every
// key's stored name to it with a static_assert, so a name too long is a failed build.
constexpr std::size_t kKeyNameMax = 15;

// The longest value. core/settings/keys.h's longest key (server.token, 160 characters) fits.
constexpr std::size_t kKeyValueMax = 160;

class KeyValue {
 public:
  virtual ~KeyValue() = default;

  // Did the store open. False is a unit whose `nvs` partition is missing or unusable: the
  // settings then fall back for every key and the console says so, which is better than
  // pretending a `set` was kept.
  virtual bool ready() const = 0;

  // The value stored under `name` into `out` (at most `cap` bytes including the terminator),
  // always terminated. False, and `out` set to "", when there is none or it does not fit.
  virtual bool read(const char* name, char* out, std::size_t cap) = 0;

  // Stores `value` under `name`, committed before this returns: a power cut after a `set` that
  // answered `ok` must not lose it. False when the store would not take it.
  virtual bool write(const char* name, const char* value) = 0;

  // Forgets one name. True when it is gone - including when it was never there.
  virtual bool erase(const char* name) = 0;
};

}  // namespace interfaces
