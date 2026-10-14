#include "platform/key_value.h"

#include <cstring>

#include "esp_err.h"
#include "nvs_flash.h"

namespace platform {

bool NvsKeyValue::begin() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // The partition is full of pages this build cannot use, or was written by a newer NVS: the
    // only way on is to erase it and start again. **This erases the settings and nothing else**
    // - the images are in `rakfw`, the journal in `journal`, and the application slots are
    // elsewhere again (docs/ARCHITECTURE.md §11) - and it happens on a new unit's first boot,
    // which has no settings to lose. It is said out loud by main's boot line.
    if (nvs_flash_erase() != ESP_OK) {
      last_error_ = err;
      return false;
    }
    rebuilt_ = true;
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    last_error_ = err;
    return false;
  }
  err = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle_);
  if (err != ESP_OK) {
    last_error_ = err;
    return false;
  }
  open_ = true;
  return true;
}

bool NvsKeyValue::read(const char* name, char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = 0;
  if (!open_ || name == nullptr) return false;
  std::size_t length = cap;
  const esp_err_t err = nvs_get_str(handle_, name, out, &length);
  if (err != ESP_OK) {
    out[0] = 0;
    return false;
  }
  return out[0] != 0;
}

bool NvsKeyValue::write(const char* name, const char* value) {
  if (!open_ || name == nullptr || value == nullptr) return false;
  if (nvs_set_str(handle_, name, value) != ESP_OK) return false;
  // Committed here and not later: a `set` that answered `ok` must still be there after the power
  // cut that follows it.
  return nvs_commit(handle_) == ESP_OK;
}

bool NvsKeyValue::erase(const char* name) {
  if (!open_ || name == nullptr) return false;
  const esp_err_t err = nvs_erase_key(handle_, name);
  // A key that was never there is already forgotten.
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
  return nvs_commit(handle_) == ESP_OK;
}

}  // namespace platform
