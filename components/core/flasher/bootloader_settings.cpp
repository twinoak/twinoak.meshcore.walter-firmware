#include "core/flasher/bootloader_settings.h"

namespace core {
namespace flasher {
namespace bootloader_settings {

void encode(const Record& r, std::uint32_t out[kRecordWords]) {
  out[0] = static_cast<std::uint32_t>(r.bank_0) | (static_cast<std::uint32_t>(r.bank_0_crc) << 16);
  out[1] = static_cast<std::uint32_t>(r.bank_1) | 0xFFFF0000u;  // the padding at offset 6: erased
  out[2] = r.bank_0_size;
  out[3] = r.sd_image_size;
  out[4] = r.bl_image_size;
  out[5] = r.app_image_size;
  out[6] = r.sd_image_start;
}

Record decode(const std::uint32_t in[kRecordWords]) {
  Record r;
  r.bank_0 = static_cast<std::uint16_t>(in[0] & 0xFFFF);
  r.bank_0_crc = static_cast<std::uint16_t>(in[0] >> 16);
  r.bank_1 = static_cast<std::uint16_t>(in[1] & 0xFFFF);
  r.bank_0_size = in[2];
  r.sd_image_size = in[3];
  r.bl_image_size = in[4];
  r.app_image_size = in[5];
  r.sd_image_start = in[6];
  return r;
}

Record invalidRecord() {
  Record r;
  r.bank_0 = kBankInvalidApp;
  r.bank_0_crc = 0;
  r.bank_1 = kBankInvalidApp;
  r.bank_0_size = 0;
  r.sd_image_size = 0;
  r.bl_image_size = 0;
  r.app_image_size = 0;
  r.sd_image_start = 0;
  return r;
}

Record validRecord(std::uint32_t size) {
  Record r = invalidRecord();
  r.bank_0 = kBankValidApp;
  r.bank_0_size = size;
  return r;
}

}  // namespace bootloader_settings
}  // namespace flasher
}  // namespace core
