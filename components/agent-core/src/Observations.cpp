#include <agent/Observations.h>

namespace mm {

const char* roleName(uint8_t advType) {
  // ADV_TYPE_* of MeshCore's helpers/AdvertDataHelpers.h at the pin, named as the .NET agent's
  // OpCodes.RoleName names them, because the server stores the word.
  switch (advType) {
    case 1: return "companion";
    case 2: return "repeater";
    case 3: return "roomserver";
    case 4: return "sensor";
    case kAdvTypeUnknown: return nullptr;
    default: return "unknown";
  }
}

void ObservationRing::push(const HeardAdvert& advert) {
  heard_++;
  if (size_ == kCapacity) {
    first_ = (first_ + 1) % kCapacity;
    size_--;
    dropped_++;
  }
  items_[(first_ + size_) % kCapacity] = advert;
  size_++;
}

const HeardAdvert& ObservationRing::at(size_t index) const {
  if (index >= size_) index = 0;
  return items_[(first_ + index) % kCapacity];
}

void ObservationRing::consume(size_t count) {
  if (count > size_) count = size_;
  first_ = (first_ + count) % kCapacity;
  size_ -= count;
}

void ObservationRing::dropNewest(size_t count) {
  if (count > size_) count = size_;
  size_ -= count;
  dropped_ += static_cast<uint32_t>(count);
}

void ObservationRing::clear() {
  first_ = 0;
  size_ = 0;
}

bool observationBatchDue(size_t pending, uint32_t oldestWaitedMs) {
  if (pending == 0) return false;
  return pending >= kObservationBatchSize || oldestWaitedMs >= kObservationFlushMs;
}

}  // namespace mm
