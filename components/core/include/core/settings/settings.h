// The settings store (task p3-03): the table of keys.h over interfaces/key_value.h.
//
// Everything is read into RAM once, at begin(), and every getter answers from there. Two reasons,
// and both matter:
//
//   * **the pointers are stable.** core/uplink/policy.h keeps the APN and the PIN as
//     `const char*`, and so will whatever reads the server's token; a getter that returned a
//     pointer to a buffer it filled on the way out would hand out a pointer that is wrong by the
//     next call. The buffers here are the store's own and live as long as it does.
//   * **a read costs no flash.** `status` and a wake read several keys; NVS reads are not free
//     and a wake's energy is the thing this firmware is written around.
//
// A value that does not validate - a store written by another firmware, a flipped bit, a key a
// person put there with `nvs_set_str` - is **ignored**: the key reads as unset, the fallback is
// used, and begin() reports it so that the console can say one line about it. A store that will
// not open at all is the same thing for every key. "A corrupt or empty key-value store at boot:
// fallbacks, a log line, no crash" (tasks/phase-3/03-settings-and-console.md).
//
// No heap, no ESP-IDF: one Settings is its buffers and a few words.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/net/url.h"
#include "core/settings/keys.h"
#include "interfaces/key_value.h"

namespace core {
namespace settings {

class Settings {
 public:
  explicit Settings(interfaces::KeyValue& store) : store_(store) {}

  // Reads every key. Once, before anything else; a second call re-reads, which is what a test
  // that simulates a reboot does.
  void begin();
  bool begun() const { return begun_; }

  // Whether the store itself is there. False: every key falls back and `get` and `set` say so.
  bool storeReady() const { return store_.ready(); }

  // How many stored values begin() threw away because they did not validate, and the name of
  // the first of them - a console line, and nothing more.
  std::size_t rejected() const { return rejected_; }
  const char* firstRejected() const { return first_rejected_; }

  // What is stored, or "" when nothing is. This is what `get` prints from.
  const char* stored(Key key) const;
  bool isSet(Key key) const { return stored(key)[0] != 0; }
  std::size_t setCount() const;

  // What the unit uses: the stored value, else the fallback, else "".
  const char* value(Key key) const;
  std::int32_t number(Key key) const;  // kInt
  bool on(Key key) const;              // kOnOff
  Level level() const;

  // -- what the firmware asks for, by name -------------------------------------------------------

  const char* apn() const { return value(Key::kLteApn); }
  const char* pin() const { return value(Key::kLtePin); }
  // Which PIN this is (core/uplink/policy.h, *The PIN*): a counter kept beside the keys and
  // bumped whenever the PIN changes or is forgotten, so that "this PIN was refused" in retained
  // storage is about the PIN that was refused and not about the one typed after it. It is not a
  // setting - nobody sets it, `get` does not print it - and `erase confirm` bumps it rather than
  // clearing it, because a new PIN must never inherit an old refusal.
  std::uint32_t pinEpoch() const { return pin_epoch_; }
  bool psm() const { return on(Key::kLtePsm); }
  const char* serverUrl() const { return value(Key::kServerUrl); }
  // The host and port of `server.url`, taken apart by core/net/url.h at every change. "" when
  // the key is unset or its URL would not parse: no server is configured then, which is what
  // core/update/uplink_check.h calls not applicable.
  const char* serverHost() const { return server_host_; }
  std::uint16_t serverPort() const { return server_port_; }
  const char* serverToken() const { return value(Key::kServerToken); }
  const char* fetchUrl() const { return value(Key::kFetchUrl); }
  // 0 is `auto`: the battery chooses (task p3-04).
  std::uint32_t wakeIntervalMinutes() const { return minutes(value(Key::kWakeInterval)); }
  bool wakeOnRadio() const { return on(Key::kWakeRadio); }
  std::int32_t swdKhz() const { return number(Key::kSwdKhz); }
  std::uint32_t swdHalfPeriodUs() const { return halfPeriodUsForKhz(swdKhz()); }
  const char* unitName() const { return value(Key::kUnitName); }

  // -- writing -----------------------------------------------------------------------------------

  // Validates, stores, and makes it what is in force. False, with one line in `err` (at most
  // `cap` bytes) and nothing stored, when the value is refused or the store would not take it.
  // What must *happen* when a key changes is the console's (core/console/console.cpp): this
  // class keeps the value and nothing else.
  //
  // `put` and not `set`, which is what `set <key> <value>` at the console does: the agent
  // firmware's SettingsStore has the same name, and a method called `set` is also the one word
  // host_test/checks/no_allocation.h reads as a `std::set` and refuses.
  bool put(const KeyDef& key, const char* value, char* err, std::size_t cap);

  // Forgets every key of the table, one by one - **the settings and only the settings**: the
  // images, the journal, the bad list and the supervisor's caps are in other partitions and in
  // retained storage and are not touched. Bumps the PIN's epoch. False, with a line in `err`,
  // when the store would not forget one; the rest are forgotten all the same.
  bool eraseAll(char* err, std::size_t cap);

 private:
  void load(Key key);
  void applyUrl();
  bool bumpPinEpoch();

  interfaces::KeyValue& store_;
  char values_[kKeyCount][kMaxValueLen + 1] = {};
  char server_host_[net::kMaxHostLength + 1] = {};
  std::uint16_t server_port_ = 443;
  std::uint32_t pin_epoch_ = 0;
  std::size_t rejected_ = 0;
  const char* first_rejected_ = "";
  bool begun_ = false;
};

// Where the PIN's epoch is kept. Beside the keys, not among them.
constexpr const char* kPinEpochName = "lte_pin_gen";
static_assert(sizeof("lte_pin_gen") - 1 <= interfaces::kKeyNameMax, "the epoch's name has to fit");

}  // namespace settings
}  // namespace core
