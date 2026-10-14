// interfaces::OwnUpdate over ESP-IDF's OTA machinery (task p2-04): the two `ota_0`/`ota_1`
// partitions of partitions.csv, their otadata states with rollback enabled
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, sdkconfig.defaults), esp_ota_begin/write/end into the
// other slot, esp_ota_set_boot_partition, esp_ota_mark_app_valid_cancel_rollback and
// esp_ota_mark_app_invalid_rollback_and_reboot. Thin: every method is one or two calls into
// components/app_update (ESP-IDF v5.5.5), and the decisions - what a trial is, when to confirm,
// when to reject - are core/update/self_update.h's, tested on the host against a fake that
// carries the bootloader's rules.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "interfaces/own_update.h"

namespace platform {

class EspOwnUpdate final : public interfaces::OwnUpdate {
 public:
  bool running(interfaces::OwnSlot* out) override;
  bool other(interfaces::OwnSlot* out) override;
  bool beginWrite(std::uint32_t size) override;
  bool write(const void* data, std::size_t size) override;
  bool endWrite() override;
  void abortWrite() override;
  bool writing() const override { return writing_; }
  bool readSlot(std::uint8_t index, std::uint32_t offset, void* out, std::uint32_t count) const override;
  bool setNextBoot() override;
  bool confirm() override;
  bool rejectAndRestart() override;

 private:
  // The partition of a slot by its index (0 = ota_0, 1 = ota_1), and a slot's description.
  static const esp_partition_t* partitionOf(std::uint8_t index);
  static bool describe(const esp_partition_t* partition, interfaces::OwnSlot* out);

  bool writing_ = false;
  esp_ota_handle_t handle_ = 0;
  const esp_partition_t* target_ = nullptr;
};

}  // namespace platform
