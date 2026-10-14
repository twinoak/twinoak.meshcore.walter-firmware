#include "platform/own_update.h"

#include <cstring>

#include "esp_app_desc.h"

namespace platform {

namespace {

// esp_ota_img_states_t -> interfaces::OwnImageState, enumerator by enumerator
// (components/bootloader_support/include/esp_flash_partitions.h, v5.5.5).
interfaces::OwnImageState stateOf(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return interfaces::OwnImageState::kNew;
    case ESP_OTA_IMG_PENDING_VERIFY: return interfaces::OwnImageState::kPendingVerify;
    case ESP_OTA_IMG_VALID: return interfaces::OwnImageState::kValid;
    case ESP_OTA_IMG_INVALID: return interfaces::OwnImageState::kInvalid;
    case ESP_OTA_IMG_ABORTED: return interfaces::OwnImageState::kAborted;
    case ESP_OTA_IMG_UNDEFINED: return interfaces::OwnImageState::kUndefined;
  }
  return interfaces::OwnImageState::kUnknown;
}

}  // namespace

const esp_partition_t* EspOwnUpdate::partitionOf(std::uint8_t index) {
  if (index > 1) return nullptr;
  const esp_partition_subtype_t subtype =
      static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + index);
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
}

bool EspOwnUpdate::describe(const esp_partition_t* partition, interfaces::OwnSlot* out) {
  if (partition == nullptr || out == nullptr) return false;
  if (partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
      partition->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 1) {
    return false;  // a factory or test partition: this firmware has none (partitions.csv)
  }
  *out = interfaces::OwnSlot{};
  out->known = true;
  out->index = static_cast<std::uint8_t>(partition->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
  out->address = partition->address;
  out->size = partition->size;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  out->state = esp_ota_get_state_partition(partition, &state) == ESP_OK ? stateOf(state)
                                                                        : interfaces::OwnImageState::kUnknown;
  // esp_app_desc_t.version: PROJECT_VER, the string core::version() and the descriptor carry.
  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(partition, &desc) == ESP_OK) {
    std::memcpy(out->version, desc.version, sizeof out->version);
    out->version[sizeof out->version - 1] = 0;
  }
  return true;
}

bool EspOwnUpdate::running(interfaces::OwnSlot* out) {
  return describe(esp_ota_get_running_partition(), out);
}

bool EspOwnUpdate::other(interfaces::OwnSlot* out) {
  return describe(esp_ota_get_next_update_partition(nullptr), out);
}

bool EspOwnUpdate::beginWrite(std::uint32_t size) {
  if (writing_) return false;
  target_ = esp_ota_get_next_update_partition(nullptr);
  if (target_ == nullptr) return false;
  // esp_ota_begin erases room for `size` bytes and refuses while the running image is
  // pending-verify (ESP_ERR_OTA_ROLLBACK_INVALID_STATE): the platform's own word on "never stack
  // an update on an unconfirmed one", beside core's.
  if (esp_ota_begin(target_, size, &handle_) != ESP_OK) return false;
  writing_ = true;
  return true;
}

bool EspOwnUpdate::write(const void* data, std::size_t size) {
  if (!writing_) return false;
  return esp_ota_write(handle_, data, size) == ESP_OK;
}

bool EspOwnUpdate::endWrite() {
  if (!writing_) return false;
  writing_ = false;
  // esp_ota_end flushes the last partial block and validates the image's own format (its
  // header, its segments, its checksum). The handle is freed whatever the result.
  return esp_ota_end(handle_) == ESP_OK;
}

void EspOwnUpdate::abortWrite() {
  if (!writing_) return;
  writing_ = false;
  (void)esp_ota_abort(handle_);
}

bool EspOwnUpdate::readSlot(std::uint8_t index, std::uint32_t offset, void* out, std::uint32_t count) const {
  const esp_partition_t* partition = partitionOf(index);
  if (partition == nullptr || out == nullptr) return false;
  if (offset > partition->size || count > partition->size - offset) return false;
  if (count == 0) return true;
  return esp_partition_read(partition, offset, out, count) == ESP_OK;
}

bool EspOwnUpdate::setNextBoot() {
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (next == nullptr) return false;
  // Validates the image in the slot and writes its otadata entry with the state NEW, which the
  // bootloader turns into PENDING_VERIFY at the next boot (interfaces/own_update.h).
  return esp_ota_set_boot_partition(next) == ESP_OK;
}

bool EspOwnUpdate::confirm() {
  // VALID, unless it is already: esp_ota_current_ota_is_workable(true) writes only when the state
  // is not VALID and returns ESP_OK either way. On an image flashed over USB (UNDEFINED) it
  // writes VALID too, which is harmless.
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool EspOwnUpdate::rejectAndRestart() {
  // INVALID, then esp_restart(). Returns only when the platform refused: no other bootable
  // application (ESP_ERR_OTA_ROLLBACK_FAILED), or a factory image is running (ESP_FAIL).
  return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK;
}

}  // namespace platform
