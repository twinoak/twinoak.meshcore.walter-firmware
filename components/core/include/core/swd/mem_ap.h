// The memory access port (task p1-04): memory reads and writes through an AHB-AP, over the debug
// port of task p1-03. Arm, ADIv5.0 to ADIv5.2, IHI 0031G (ID022122), chapter C2 "The Memory
// Access Port" for the registers and their rules; Arm, Cortex-M4 Technical Reference Manual,
// DDI 0439B, section 8.2 for what this core's AHB-AP makes of them. No ESP-IDF include, no heap,
// no loop without a bound that is an argument or a constant.
//
//   open()        reads IDR once (C1.3.2: "an IDR value of zero indicates that there is no AP
//                 present"), insists on CLASS = Memory Access Port, and writes CSW for 32-bit
//                 accesses with single auto-increment;
//   read32/write32 one word at TAR;
//   readBlock/writeBlock
//                 words at consecutive addresses with auto-increment, TAR rewritten at every
//                 1 KB boundary: ADIv5 C2.2.2 guarantees the increment "on the 10 least
//                 significant bits of the address" only - "auto address incrementing at a 1KB
//                 boundary is IMPLEMENTATION DEFINED". (The Cortex-M4's own AHB-AP "increments
//                 and wraps within a 4-KB address boundary", DDI 0439B Table 8-6; a host that
//                 rewrites at every 1 KB is right for both, and for any MEM-AP.)
//
// Every answer carries the memory address it was for, so that a failure names the word.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/swd/dp.h"

namespace core {
namespace swd {

// The MEM-AP register map (ADIv5 C2.5, Table C2-6: "All registers that are listed in Table C2-6
// are required in every MEM-AP implementation"). Byte offsets in the AP, as Dp::readAp takes them.
namespace mem_ap {
constexpr std::uint8_t kCsw = 0x00;  // Control/Status Word (C2.6.5)
constexpr std::uint8_t kTar = 0x04;  // Transfer Address Register (C2.6.8)
constexpr std::uint8_t kDrw = 0x0C;  // Data Read/Write (C2.6.6)
constexpr std::uint8_t kIdr = 0xFC;  // Identification Register (C1.3.2)

// CSW fields (C2.6.5 "Field Descriptions"; the AHB-AP's own in DDI 0439B Table 8-6).
constexpr std::uint32_t kCswSizeMask = 0x7u;       // Size, bits[2:0]
constexpr std::uint32_t kCswSizeWord = 0b010u;     // "Word (32-bits)", the one size every MEM-AP supports (Table C2-7)
constexpr std::uint32_t kCswAddrIncMask = 0x3u << 4;  // AddrInc, bits[5:4]
constexpr std::uint32_t kCswAddrIncSingle = 0b01u << 4;  // "Address increment-single enabled"
constexpr std::uint32_t kCswDeviceEn = 1u << 6;    // "Transactions can be issued through the MEM-AP" (read-only)
constexpr std::uint32_t kCswTrInProg = 1u << 7;    // "A transfer is in progress"
constexpr std::uint32_t kCswHprot1 = 1u << 25;     // DDI 0439B Table 8-6: HPROT[1], privileged; reset value 1
constexpr std::uint32_t kCswBit24 = 1u << 24;      // DDI 0439B Table 8-6: "Reserved, 0b1"
// DDI 0439B Table 8-6, MasterType: "0 = core. 1 = debug", and footnote a: "When clear, this bit
// prevents the debugger from setting the C_DEBUGEN bit in the Debug Halting Control and Status
// Register, and so prevents the debugger from being able to halt the processor."
constexpr std::uint32_t kCswMasterTypeDebug = 1u << 29;
// What this host writes: a debug master, privileged, word accesses, single increment -
// 0x23000012; read back with DeviceEn (read-only) set it is the 0x23000052 debuggers show.
constexpr std::uint32_t kCswValue = kCswMasterTypeDebug | kCswHprot1 | kCswBit24 | kCswAddrIncSingle | kCswSizeWord;

// The block inside which auto-increment is guaranteed (C2.2.2: "only guaranteed to operate on
// the 10 least significant bits").
constexpr std::uint32_t kAutoIncrementBlock = 1024;

// IDR fields (C1.3.2): REVISION[31:28], DESIGNER[27:17] (JEP106 continuation [27:24], identity
// [23:17]), CLASS[16:13], VARIANT[7:4], TYPE[3:0]; bits [12:8] RES0 ("if a debugger reads a
// non-zero value in this field, it must treat the AP as unidentifiable").
constexpr std::uint32_t kIdrClassMemAp = 0b1000u;  // "Memory Access Port"
constexpr std::uint32_t kIdrClassNone = 0b0000u;   // "No defined class"

// The Cortex-M4's AHB-AP: DDI 0439B Table 8-5, "0xFC IDR RO 0x24770011". Revision 2, Arm,
// class MEM-AP, variant 1, type 1 (AHB). The nRF52840 has a Cortex-M4 and "a default CPU access
// port (AHB-AP)" (Product Specification, "Debug and trace > DAP"); the bench confirms the number
// (docs/bench.md section 4). The host insists only on CLASS = MEM-AP and reports the rest.
constexpr std::uint32_t kCortexM4AhbApIdr = 0x24770011;
}  // namespace mem_ap

struct ApIdrFields {
  std::uint8_t revision;
  std::uint16_t designer;  // JEP106 continuation [3:0] << 7 | identity [6:0], as DpidrFields
  std::uint8_t ap_class;
  std::uint8_t variant;
  std::uint8_t type;
  bool reserved_clear;  // bits [12:8] are zero: the AP is identifiable
};
ApIdrFields decodeApIdr(std::uint32_t idr);

struct MemResult {
  Status status = Status::kOk;
  std::uint32_t address = 0;    // the memory address the failing (or read) access was for
  std::uint32_t value = 0;      // read32's data; open()'s IDR
  std::uint32_t ctrl_stat = 0;  // on kFault: the sticky flags as the DP read them
  bool ok() const { return status == Status::kOk; }
};

class MemAp {
 public:
  MemAp(Dp& dp, std::uint8_t ap);

  // IDR read and checked once, CSW written. Needs the debug power-up to have been acknowledged
  // (B2.3.2). value = IDR. Refuses with kNoMemAp when IDR is zero, when the class is not
  // Memory Access Port, or when the reserved bits are not.
  MemResult open();

  MemResult read32(std::uint32_t addr);
  MemResult write32(std::uint32_t addr, std::uint32_t value);
  // `words` of them, consecutive, addr word-aligned; TAR written at the start and at every
  // kAutoIncrementBlock boundary.
  MemResult readBlock(std::uint32_t addr, std::uint32_t* out, std::size_t words);
  MemResult writeBlock(std::uint32_t addr, const std::uint32_t* data, std::size_t words);

  bool opened() const { return opened_; }
  std::uint32_t idr() const { return idr_; }
  std::uint8_t ap() const { return ap_; }
  Dp& dp() { return dp_; }

  struct Counters {
    std::uint32_t idr_reads = 0;
    std::uint32_t csw_writes = 0;
    std::uint32_t tar_writes = 0;
    std::uint32_t drw_reads = 0;
    std::uint32_t drw_writes = 0;
    std::uint32_t errors = 0;
  };
  const Counters& counters() const { return counters_; }

 private:
  MemResult fromDp(const Result& r, std::uint32_t addr);
  MemResult setTar(std::uint32_t addr);
  void afterDrw(std::uint32_t addr);  // the TAR cache after a successful DRW access

  Dp& dp_;
  const std::uint8_t ap_;
  bool opened_ = false;
  std::uint32_t idr_ = 0;
  bool tar_known_ = false;
  std::uint32_t tar_ = 0;
  Counters counters_;
};

}  // namespace swd
}  // namespace core
