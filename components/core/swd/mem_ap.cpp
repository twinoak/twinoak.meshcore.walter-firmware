#include "core/swd/mem_ap.h"

namespace core {
namespace swd {

ApIdrFields decodeApIdr(std::uint32_t idr) {
  ApIdrFields f;
  f.revision = static_cast<std::uint8_t>((idr >> 28) & 0xF);
  f.designer = static_cast<std::uint16_t>((idr >> 17) & 0x7FF);
  f.ap_class = static_cast<std::uint8_t>((idr >> 13) & 0xF);
  f.variant = static_cast<std::uint8_t>((idr >> 4) & 0xF);
  f.type = static_cast<std::uint8_t>(idr & 0xF);
  f.reserved_clear = ((idr >> 8) & 0x1F) == 0;
  return f;
}

MemAp::MemAp(Dp& dp, std::uint8_t ap) : dp_(dp), ap_(ap) {}

MemResult MemAp::fromDp(const Result& r, std::uint32_t addr) {
  MemResult m;
  m.status = r.status;
  m.address = addr;
  m.value = r.value;
  m.ctrl_stat = r.ctrl_stat;
  if (!r.ok()) {
    counters_.errors++;
    // C2.2.2: "if the DRW transaction completes with an error response, or the transaction is
    // aborted, the TAR is not incremented" - and after a WAIT that timed out or a lost line
    // nothing is known about what landed. The cache is dropped on every error.
    tar_known_ = false;
  }
  return m;
}

// C1.3.2: IDR "identifies the Access Port. An IDR value of zero indicates that there is no AP
// present"; CLASS 0b1000 is "Memory Access Port"; bits [12:8] non-zero mean "unidentifiable".
// Then CSW (C2.6.5): word accesses, increment-single, as a debug master (DDI 0439B Table 8-6).
MemResult MemAp::open() {
  opened_ = false;
  tar_known_ = false;
  counters_.idr_reads++;
  const Result idr = dp_.readAp(ap_, mem_ap::kIdr);
  MemResult m = fromDp(idr, 0);
  if (!m.ok()) return m;
  idr_ = idr.value;
  const ApIdrFields f = decodeApIdr(idr_);
  if (idr_ == 0 || f.ap_class != mem_ap::kIdrClassMemAp || !f.reserved_clear) {
    counters_.errors++;
    m.status = Status::kNoMemAp;
    return m;
  }
  counters_.csw_writes++;
  m = fromDp(dp_.writeAp(ap_, mem_ap::kCsw, mem_ap::kCswValue), 0);
  if (!m.ok()) return m;
  opened_ = true;
  m.value = idr_;
  return m;
}

MemResult MemAp::setTar(std::uint32_t addr) {
  if (tar_known_ && tar_ == addr) return MemResult{};
  counters_.tar_writes++;
  const MemResult m = fromDp(dp_.writeAp(ap_, mem_ap::kTar, addr), addr);
  if (m.ok()) {
    tar_known_ = true;
    tar_ = addr;
  }
  return m;
}

// After a successful DRW access TAR has moved on by four (C2.2.2, "Increment single") - unless
// that crossed the 1 KB boundary, where what the AP did is IMPLEMENTATION DEFINED and the cache
// says so by forgetting.
void MemAp::afterDrw(std::uint32_t addr) {
  const std::uint32_t next = addr + 4;
  if ((next % mem_ap::kAutoIncrementBlock) == 0) {
    tar_known_ = false;
  } else {
    tar_known_ = true;
    tar_ = next;
  }
}

MemResult MemAp::read32(std::uint32_t addr) {
  MemResult m;
  if (!opened_) {
    m.status = Status::kNoMemAp;
    m.address = addr;
    return m;
  }
  if ((addr & 0x3) != 0) {
    m.status = Status::kBadAddress;
    m.address = addr;
    return m;
  }
  m = setTar(addr);
  if (!m.ok()) return m;
  counters_.drw_reads++;
  m = fromDp(dp_.readAp(ap_, mem_ap::kDrw), addr);
  if (m.ok()) afterDrw(addr);
  return m;
}

MemResult MemAp::write32(std::uint32_t addr, std::uint32_t value) {
  MemResult m;
  if (!opened_) {
    m.status = Status::kNoMemAp;
    m.address = addr;
    return m;
  }
  if ((addr & 0x3) != 0) {
    m.status = Status::kBadAddress;
    m.address = addr;
    return m;
  }
  m = setTar(addr);
  if (!m.ok()) return m;
  counters_.drw_writes++;
  m = fromDp(dp_.writeAp(ap_, mem_ap::kDrw, value), addr);
  if (m.ok()) afterDrw(addr);
  return m;
}

// Each word is one DRW access with TAR left to the AP's auto-increment; read32/write32 keep the
// TAR cache, so the TAR write happens at the first word and again at every 1 KB boundary - where
// afterDrw() forgot it - and nowhere else. Bounded by `words`.
MemResult MemAp::readBlock(std::uint32_t addr, std::uint32_t* out, std::size_t words) {
  MemResult m;
  m.address = addr;
  if (out == nullptr || words == 0) {
    m.status = Status::kBadAddress;
    return m;
  }
  for (std::size_t i = 0; i < words; ++i) {
    const std::uint32_t at = addr + static_cast<std::uint32_t>(i) * 4u;
    m = read32(at);
    if (!m.ok()) return m;
    out[i] = m.value;
  }
  return m;
}

MemResult MemAp::writeBlock(std::uint32_t addr, const std::uint32_t* data, std::size_t words) {
  MemResult m;
  m.address = addr;
  if (data == nullptr || words == 0) {
    m.status = Status::kBadAddress;
    return m;
  }
  for (std::size_t i = 0; i < words; ++i) {
    const std::uint32_t at = addr + static_cast<std::uint32_t>(i) * 4u;
    m = write32(at, data[i]);
    if (!m.ok()) return m;
  }
  return m;
}

}  // namespace swd
}  // namespace core
