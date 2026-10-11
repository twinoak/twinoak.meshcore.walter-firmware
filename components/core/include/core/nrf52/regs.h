// The nRF52840's registers as this firmware and its simulator know them (task p1-04). Every
// number here was read from the document it names:
//
//   PS    Nordic Semiconductor, nRF52840 Product Specification, the HTML rendition at
//         docs.nordicsemi.com/bundle/ps_nrf52840 ("corresponds to the latest version"), read
//         2026-10-11: the chapters FICR, UICR, NVMC, "Debug and trace", Memory. The same
//         offsets are in Nordic's own MDK header nrf52840.h (nrfx, 2018), which was read beside
//         it.
//   ARM   Arm, Armv7-M Architecture Reference Manual, DDI 0403E.e (ID021621): B3.2.6 AIRCR,
//         C1.6.2 DHCSR, C1.6.5 DEMCR.
//   NOTE  Nordic, "Working with the nRF52 Series' improved APPROTECT" (DevZone, 2022-02-05).
//
// The constants nothing may write - NVMC.ERASEALL, NVMC.ERASEUICR, the control access port's
// ERASEALL, every address in UICR - are here too, named, so that the simulator can trip on them
// and a person can see what is being refused. host_test/core/nrf52/test_nrf52_forbidden.cpp
// reads every source under components/ and fails if one of their names appears anywhere but in
// this header. S-5: "no chip erase, ever, by anything automatic".
#pragma once

#include <cstdint>

namespace core {
namespace nrf52 {

// ---- FICR, factory information configuration registers (PS "FICR", base 0x10000000) ----------
// "pre-programmed in factory and cannot be erased by the user"; all read-only.
namespace ficr {
constexpr std::uint32_t kBase = 0x10000000;
constexpr std::uint32_t kCodePageSize = kBase + 0x010;  // "Code memory page size"
constexpr std::uint32_t kCodeSize = kBase + 0x014;      // "Code memory size in number of pages"
constexpr std::uint32_t kDeviceId0 = kBase + 0x060;     // "Device identifier", least significant word
constexpr std::uint32_t kDeviceId1 = kBase + 0x064;     // most significant word
constexpr std::uint32_t kInfoPart = kBase + 0x100;      // "Part code": 0x52840
constexpr std::uint32_t kInfoVariant = kBase + 0x104;   // "Build code ... encoded in ASCII": four characters, first in bits [31:24]
constexpr std::uint32_t kInfoPackage = kBase + 0x108;   // "Package option"
constexpr std::uint32_t kInfoRam = kBase + 0x10C;       // "RAM variant", in KB
constexpr std::uint32_t kInfoFlash = kBase + 0x110;     // "Flash variant", in KB (0x400 = 1 MB)

constexpr std::uint32_t kPartNrf52840 = 0x52840;      // INFO.PART "nRF52840"
constexpr std::uint32_t kUnspecified = 0xFFFFFFFF;    // INFO.PART, INFO.VARIANT, INFO.FLASH: "Unspecified"
constexpr std::uint32_t kFlashK1024 = 0x400;          // INFO.FLASH "1 MByte FLASH"
}  // namespace ficr

// ---- UICR, user information configuration registers (PS "UICR", base 0x10001000) -------------
// READ-ONLY TO THIS FIRMWARE. UICR is flash: it "is written in the same way as flash" and erased
// by ERASEUICR or ERASEALL (PS NVMC); REGOUT0 and APPROTECT live here, and an erased UICR drops
// a bare core's GPIO domain to 1.8 V (hardware repository, BENCH-BREADBOARD.md caveat 5).
// Nothing in components/ writes an address in this range; the simulator fails the test if
// anything tries.
namespace uicr {
constexpr std::uint32_t kBase = 0x10001000;
constexpr std::uint32_t kEnd = kBase + 0x308;            // REGOUT0 is the last register, at +0x304
constexpr std::uint32_t kNrfFw0 = kBase + 0x014;         // NRFFW[0], "Reserved for Nordic firmware design": the bootloader's address
constexpr std::uint32_t kPselReset0 = kBase + 0x200;     // "Mapping of the nRESET function"
constexpr std::uint32_t kPselReset1 = kBase + 0x204;
constexpr std::uint32_t kApprotect = kBase + 0x208;      // "Access port protection"
constexpr std::uint32_t kRegOut0 = kBase + 0x304;        // "Output voltage from REG0 regulator stage"

// APPROTECT.PALL (PS "UICR > APPROTECT"; NOTE): 0xFF Disabled, 0x00 Enabled, and on build codes
// Fxx and later 0x5A HwDisabled - the value a module must hold for the port to be open after a
// reset, with the firmware also writing APPROTECT.DISABLE = 0x5A at run time (NOTE; PS "Debug
// and trace > Access port protection").
constexpr std::uint32_t kApprotectDisabled = 0xFF;
constexpr std::uint32_t kApprotectEnabled = 0x00;
constexpr std::uint32_t kApprotectHwDisabled = 0x5A;
}  // namespace uicr

// ---- NVMC, non-volatile memory controller (PS "NVMC", base 0x4001E000) -----------------------
namespace nvmc {
constexpr std::uint32_t kBase = 0x4001E000;
constexpr std::uint32_t kReady = kBase + 0x400;       // "Ready flag": bit 0, 1 = ready, 0 = "busy (on-going write or erase operation)"
constexpr std::uint32_t kReadyNext = kBase + 0x408;   // "NVMC can accept a new write operation"
constexpr std::uint32_t kConfig = kBase + 0x504;      // "Configuration register": WEN, bits [1:0]
constexpr std::uint32_t kErasePage = kBase + 0x508;   // "Register for erasing a page in code area": the page's address
// NEVER WRITTEN BY THIS FIRMWARE. Named for the simulator's tripwire and the grep test.
constexpr std::uint32_t kEraseAll = kBase + 0x50C;    // "Register for erasing all non-volatile user memory" - flash and UICR
constexpr std::uint32_t kEraseUicr = kBase + 0x514;   // "Register for erasing user information configuration registers"
constexpr std::uint32_t kErasePagePartial = kBase + 0x518;
constexpr std::uint32_t kErasePagePartialCfg = kBase + 0x51C;

constexpr std::uint32_t kReadyBit = 1u << 0;
// CONFIG.WEN (PS "NVMC > CONFIG"): "Ren = 0 (Read only access). Wen = 1 (Write enabled).
// Een = 2 (Erase enabled)."
constexpr std::uint32_t kConfigRen = 0;
constexpr std::uint32_t kConfigWen = 1;
constexpr std::uint32_t kConfigEen = 2;
constexpr std::uint32_t kConfigMask = 0x3;

// PS "NVMC > Electrical specification > Flash programming" (the HTML rendition gives typical
// values only, footnoted "applies when HFXO is used; timing varies according to HFINT accuracy
// when HFINT is used"): tWRITE 41 us typical, tERASEPAGE 85 ms typical, nWRITE 2 ("number of
// times a 32-bit word can be written before erase"). The bounds this firmware waits are below,
// in nrf52.h, with their margins.
constexpr std::uint32_t kWriteTypicalUs = 41;
constexpr std::uint32_t kErasePageTypicalMs = 85;
constexpr std::uint32_t kWritesPerEraseMax = 2;
}  // namespace nvmc

// ---- Flash (PS "Memory"): "256 pages of 4 kB each", at 0x00000000 -----------------------------
namespace flash {
constexpr std::uint32_t kBase = 0x00000000;
constexpr std::uint32_t kPageSize = 4096;    // confirmed at run time from FICR.CODEPAGESIZE
constexpr std::uint32_t kPagesNrf52840 = 256;
constexpr std::uint32_t kSizeNrf52840 = kPageSize * kPagesNrf52840;  // 1 MB
constexpr std::uint32_t kErased = 0xFFFFFFFF;  // "After erasing a flash page, all bits in the page are set to 1"
}  // namespace flash

// ---- CTRL-AP, the control access port (PS "Debug and trace > CTRL-AP"), AP 1 -----------------
// "A custom access port that enables control of the device when other access ports in the DAP are
// disabled by the access port protection." Byte offsets in the AP.
namespace ctrl_ap {
constexpr std::uint8_t kApsel = 1;
constexpr std::uint8_t kReset = 0x00;            // "Soft reset triggered through CTRL-AP": 1 = "Device is held in reset"
// NEVER WRITTEN BY THIS FIRMWARE: "Erase all flash and RAM" - and UICR with them. Opening a
// protected module is a bench procedure (hardware repository, BENCH-BREADBOARD.md caveat 6).
constexpr std::uint8_t kEraseAll = 0x04;
constexpr std::uint8_t kEraseAllStatus = 0x08;   // 0 "ERASEALL is ready", 1 "busy (on-going)"
constexpr std::uint8_t kApprotectStatus = 0x0C;  // bit 0: 0 = "Access port protection enabled", 1 = "not enabled"
constexpr std::uint8_t kIdr = 0xFC;              // reset value 0x02880000 (PS "CTRL-AP > IDR")

constexpr std::uint32_t kApprotectStatusOpen = 1u << 0;
// PS gives IDR's reset value as 0x02880000, which decodes by ADIv5 C1.3.2 as designer JEP106
// continuation 2, identity 0x44 (Nordic Semiconductor), class 0 ("no defined class"), revision 0.
constexpr std::uint32_t kIdrValue = 0x02880000;
constexpr std::uint16_t kDesignerNordic = (0x2u << 7) | 0x44u;  // 0x144, as ApIdrFields::designer
}  // namespace ctrl_ap

// ---- The core's debug registers (ARM, System Control Space) ----------------------------------
namespace scs {
// DHCSR, Debug Halting Control and Status Register (ARM C1.6.2), 0xE000EDF0.
constexpr std::uint32_t kDhcsr = 0xE000EDF0;
constexpr std::uint32_t kDbgKey = 0xA05Fu << 16;   // "A debugger must write 0xA05F to this field to enable write accesses to bits[15:0]"
constexpr std::uint32_t kCDebugEn = 1u << 0;       // "Halting debug is enabled when C_DEBUGEN is set to 1"
constexpr std::uint32_t kCHalt = 1u << 1;
constexpr std::uint32_t kCStep = 1u << 2;
constexpr std::uint32_t kCMaskInts = 1u << 3;
constexpr std::uint32_t kSRegRdy = 1u << 16;
constexpr std::uint32_t kSHalt = 1u << 17;         // "1 In Debug state"
constexpr std::uint32_t kSSleep = 1u << 18;
constexpr std::uint32_t kSLockup = 1u << 19;
constexpr std::uint32_t kSRetireSt = 1u << 24;
constexpr std::uint32_t kSResetSt = 1u << 25;      // "At least one reset since last DHCSR read" - sticky, cleared by the read

// DEMCR, Debug Exception and Monitor Control Register (ARM C1.6.5), 0xE000EDFC.
constexpr std::uint32_t kDemcr = 0xE000EDFC;
constexpr std::uint32_t kVcCoreReset = 1u << 0;    // "Enable Reset Vector Catch. This causes a Local reset to halt a running system."
constexpr std::uint32_t kTrcEna = 1u << 24;

// AIRCR, Application Interrupt and Reset Control Register (ARM B3.2.6), 0xE000ED0C.
constexpr std::uint32_t kAircr = 0xE000ED0C;
constexpr std::uint32_t kVectKey = 0x05FAu << 16;  // "Register writes must write 0x05FA to this field, otherwise the write is ignored"
constexpr std::uint32_t kVectKeyStat = 0xFA05u << 16;  // "On reads, returns 0xFA05"
constexpr std::uint32_t kSysResetReq = 1u << 2;    // "Writing 1 to this bit asserts a signal to the external system to request a Local reset"
constexpr std::uint32_t kVectReset = 1u << 0;      // UNPREDICTABLE unless halted; never written here
}  // namespace scs

}  // namespace nrf52
}  // namespace core
