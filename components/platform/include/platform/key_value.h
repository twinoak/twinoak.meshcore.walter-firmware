// interfaces::KeyValue over NVS (task p3-03): the settings in the `nvs` partition of
// partitions.csv, which docs/ARCHITECTURE.md §11 puts **above** the application slots so that a
// factory image - which ends where the application ends - cannot reach them. The agent firmware
// learned that the expensive way; this table was written to it from the first.
//
// One namespace, `walter`, one entry per key of core/settings/keys.h. Thin, as everything in
// platform/ is: nvs_flash_init, one open handle, nvs_get_str, nvs_set_str, nvs_erase_key, and a
// commit after every write - a `set` that answered `ok` has to survive the power cut after it.
//
// **Not encrypted.** NVS encryption needs the flash-encryption key and is the owner's choice;
// without it, a person with the board in hand and a cable can read the SIM's PIN and the
// server's token out of this partition. docs/console.md says so rather than hiding it.
#pragma once

#include "nvs.h"

#include "interfaces/key_value.h"

namespace platform {

// The namespace the settings live in. NVS allows 15 characters here too.
constexpr const char* kSettingsNamespace = "walter";

class NvsKeyValue final : public interfaces::KeyValue {
 public:
  // Initialises NVS and opens the namespace. False when the partition is missing, or is from a
  // firmware whose NVS layout this one cannot read and could not be erased and rebuilt: said
  // once at boot, and every setting then falls back.
  bool begin();

  bool ready() const override { return open_; }
  bool read(const char* name, char* out, std::size_t cap) override;
  bool write(const char* name, const char* value) override;
  bool erase(const char* name) override;

  // For the boot line: whether nvs_flash_init had to erase the partition and start again (a new
  // unit, or a layout change), and how NVS answered when it would not open at all.
  bool wasRebuilt() const { return rebuilt_; }
  int lastError() const { return last_error_; }

 private:
  nvs_handle_t handle_ = 0;
  bool open_ = false;
  bool rebuilt_ = false;
  int last_error_ = 0;
};

}  // namespace platform
