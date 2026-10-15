#include "core/cycle/telemetry_ring.h"

namespace core {
namespace cycle {

namespace {

void put32(std::uint8_t* at, std::uint32_t value) {
  at[0] = static_cast<std::uint8_t>(value);
  at[1] = static_cast<std::uint8_t>(value >> 8);
  at[2] = static_cast<std::uint8_t>(value >> 16);
  at[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t get32(const std::uint8_t* at) {
  return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
         (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

void put16(std::uint8_t* at, std::uint16_t value) {
  at[0] = static_cast<std::uint8_t>(value);
  at[1] = static_cast<std::uint8_t>(value >> 8);
}

std::uint16_t get16(const std::uint8_t* at) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(at[0]) |
                                    (static_cast<std::uint16_t>(at[1]) << 8));
}

std::uint16_t cap16(std::uint32_t value) {
  return value > 0xFFFFu ? static_cast<std::uint16_t>(0xFFFFu) : static_cast<std::uint16_t>(value);
}

// The temperature to the degree, rounded towards zero, and clamped to what a byte holds. -128 is
// "unknown" and is never written as a reading.
std::int8_t degreesOf(std::int16_t centi) {
  const int whole = centi / 100;
  if (whole > 127) return 127;
  if (whole < -127) return -127;
  return static_cast<std::int8_t>(whole);
}

}  // namespace

void TelemetryRing::begin() {
  if (begun_) return;
  begun_ = true;
  load();
}

void TelemetryRing::load() {
  std::uint8_t bytes[kRecordBytes] = {};
  survived_ = retained_.load(interfaces::RetainedSlot::kWakeCycle, bytes, sizeof bytes);
  if (!survived_ || bytes[0] != kRecordVersion) {
    survived_ = false;
    forget();
    return;
  }
  count_ = bytes[1] <= kReadingsKept ? bytes[1] : kReadingsKept;
  next_ = bytes[2] < kReadingsKept ? bytes[2] : 0;
  refusals_ = bytes[3] <= kRefusalsKept ? bytes[3] : kRefusalsKept;
  dropped_ = get16(bytes + 4);
  taken_ = get16(bytes + 6);
  for (std::size_t i = 0; i < kRefusalsKept; ++i) {
    const std::uint8_t* at = bytes + kRecordHeaderBytes + 2 * i;
    refusal_[i].ask = at[0] < static_cast<std::uint8_t>(interfaces::Ask::kCount) ? at[0] : 0;
    refusal_[i].why = at[1] < static_cast<std::uint8_t>(interfaces::Refusal::kCount) ? at[1] : 0;
  }
  for (std::size_t i = 0; i < kReadingsKept; ++i) {
    const std::uint8_t* at = bytes + kRecordHeaderBytes + kRefusalsKept * 2 + kReadingBytes * i;
    Taken& r = items_[i];
    r.at_epoch = get32(at);
    r.millivolts = get16(at + 4);
    r.current_ma = static_cast<std::int16_t>(get16(at + 6));
    const std::uint8_t flags = at[8];
    r.time_known = (flags & kReadingTimeKnown) != 0;
    r.battery_known = (flags & kReadingBatteryKnown) != 0;
    r.current_known = (flags & kReadingCurrentKnown) != 0;
    r.temp_known = (flags & kReadingTempKnown) != 0;
    r.temp_centi = static_cast<std::int16_t>(static_cast<std::int8_t>(at[9]) * 100);
    r.wake = flags >> 4;
  }
}

void TelemetryRing::save() {
  std::uint8_t bytes[kRecordBytes] = {};
  bytes[0] = kRecordVersion;
  bytes[1] = static_cast<std::uint8_t>(count_);
  bytes[2] = static_cast<std::uint8_t>(next_);
  bytes[3] = static_cast<std::uint8_t>(refusals_);
  put16(bytes + 4, cap16(dropped_));
  put16(bytes + 6, cap16(taken_));
  for (std::size_t i = 0; i < kRefusalsKept; ++i) {
    std::uint8_t* at = bytes + kRecordHeaderBytes + 2 * i;
    at[0] = refusal_[i].ask;
    at[1] = refusal_[i].why;
  }
  for (std::size_t i = 0; i < kReadingsKept; ++i) {
    std::uint8_t* at = bytes + kRecordHeaderBytes + kRefusalsKept * 2 + kReadingBytes * i;
    const Taken& r = items_[i];
    put32(at, r.at_epoch);
    put16(at + 4, r.millivolts);
    put16(at + 6, static_cast<std::uint16_t>(r.current_ma));
    // The wake's reason shares the flag byte's top nibble: there are nine of them
    // (core/cycle/wake_reason.h, Wake::kCount) and four flags, so one byte does for both.
    const std::uint8_t flags = static_cast<std::uint8_t>(
        (r.time_known ? kReadingTimeKnown : 0) | (r.battery_known ? kReadingBatteryKnown : 0) |
        (r.current_known ? kReadingCurrentKnown : 0) | (r.temp_known ? kReadingTempKnown : 0) |
        static_cast<std::uint8_t>((r.wake & 0x0Fu) << 4));
    at[8] = flags;
    at[9] = static_cast<std::uint8_t>(r.temp_known ? degreesOf(r.temp_centi) : 0);
  }
  (void)retained_.store(interfaces::RetainedSlot::kWakeCycle, bytes, sizeof bytes);
}

void TelemetryRing::add(const Taken& reading) {
  if (count_ == kReadingsKept) {
    // The oldest goes, and is counted. `next_` is where the oldest is when the ring is full.
    if (dropped_ != 0xFFFFFFFFu) ++dropped_;
  } else {
    ++count_;
  }
  items_[next_] = reading;
  // Only what the record can carry: a reading whose temperature is not known keeps none, and the
  // degree is what comes back, so what a test reads is what a sleep would have kept.
  items_[next_].temp_centi =
      reading.temp_known ? static_cast<std::int16_t>(degreesOf(reading.temp_centi) * 100) : 0;
  items_[next_].wake = static_cast<std::uint8_t>(reading.wake & 0x0Fu);
  next_ = (next_ + 1) % kReadingsKept;
  save();
}

Taken TelemetryRing::at(std::size_t index) const {
  if (index >= count_) return Taken{};
  // The oldest is `next_` back `count_` places, which is `next_` itself when the ring is full.
  const std::size_t oldest = (next_ + kReadingsKept - count_) % kReadingsKept;
  return items_[(oldest + index) % kReadingsKept];
}

void TelemetryRing::wereTaken(std::size_t offered) {
  if (offered == 0) return;
  const std::size_t gone = offered < count_ ? offered : count_;
  // The readings that stay are the newest `count_ - gone`; `next_` does not move, so dropping
  // the oldest is a subtraction.
  count_ -= gone;
  taken_ += static_cast<std::uint32_t>(gone);
  save();
}

void TelemetryRing::refused(interfaces::Ask ask, interfaces::Refusal why) {
  const std::uint8_t a = static_cast<std::uint8_t>(ask);
  const std::uint8_t w = static_cast<std::uint8_t>(why);
  for (std::size_t i = 0; i < refusals_; ++i) {
    if (refusal_[i].ask == a && refusal_[i].why == w) return;  // already there: not twice
  }
  if (refusals_ == kRefusalsKept) {
    for (std::size_t i = 1; i < kRefusalsKept; ++i) refusal_[i - 1] = refusal_[i];
    --refusals_;
  }
  refusal_[refusals_].ask = a;
  refusal_[refusals_].why = w;
  ++refusals_;
  save();
}

interfaces::BackendRefusal TelemetryRing::refusalAt(std::size_t index) const {
  if (index >= refusals_) return interfaces::BackendRefusal{};
  return refusal_[index];
}

void TelemetryRing::refusalsTold() {
  if (refusals_ == 0) return;
  refusals_ = 0;
  for (std::size_t i = 0; i < kRefusalsKept; ++i) refusal_[i] = interfaces::BackendRefusal{};
  save();
}

std::size_t TelemetryRing::offerTo(interfaces::BackendKnowledge* known) const {
  if (known == nullptr) return 0;
  const std::size_t n = count_ < interfaces::kBackendReadings ? count_
                                                             : interfaces::kBackendReadings;
  for (std::size_t i = 0; i < n; ++i) {
    const Taken r = at(i);
    interfaces::BackendReading& out = known->reading[i];
    out.time_known = r.time_known;
    out.at_epoch = r.time_known ? r.at_epoch : 0;
    out.battery_known = r.battery_known;
    out.battery_mv = r.millivolts;
    out.current_known = r.current_known;
    out.battery_ma = r.current_ma;
    out.temp_known = r.temp_known;
    out.temp_c = static_cast<std::int16_t>(r.temp_centi / 100);
    out.wake = r.wake;
  }
  known->readings = n;
  known->readings_dropped = dropped_;
  const std::size_t refusals = refusals_ < interfaces::kBackendSettingsMax
                                   ? refusals_
                                   : interfaces::kBackendSettingsMax;
  for (std::size_t i = 0; i < refusals; ++i) known->refusal[i] = refusal_[i];
  known->refusals = refusals;
  return n;
}

void TelemetryRing::forget() {
  for (std::size_t i = 0; i < kReadingsKept; ++i) items_[i] = Taken{};
  for (std::size_t i = 0; i < kRefusalsKept; ++i) refusal_[i] = interfaces::BackendRefusal{};
  count_ = 0;
  next_ = 0;
  refusals_ = 0;
  dropped_ = 0;
  taken_ = 0;
}

}  // namespace cycle
}  // namespace core
