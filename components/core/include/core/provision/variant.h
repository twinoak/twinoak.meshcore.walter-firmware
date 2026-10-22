// The variant table (task p5-03; spec/SYSTEM.md S-5a): which kind of access-port protection a
// part has, looked up from FICR.INFO.VARIANT and never guessed from behaviour.
//
// What the documents say, each read for this file:
//
//   PS    nRF52840 Product Specification, the HTML rendition at docs.nordicsemi.com (bundle
//         ps_nrf52840), read 2026-10-20. "FICR > INFO.VARIANT": "Build code, last two letters of
//         Package Variant and first two characters of Build Code, encoded in ASCII" - four
//         characters, the first in bits [31:24] (core/nrf52/regs.h), so INFO.VARIANT reads
//         "AAF0" as 0x41414630. "Debug and trace > Access port protection": on build codes
//         "Dxx and earlier" the protection is "By default ... disabled" and hardware-only; on
//         "Fxx and later" it is "By default ... enabled", and keeping it open after a reset
//         needs "Program UICR.APPROTECT to HwDisabled" and "Firmware must write
//         APPROTECT.DISABLE to SwDisable". The kind is therefore a function of the build code's
//         first character, which is INFO.VARIANT's third.
//   NOTE  Nordic, "Working with the nRF52 Series' improved APPROTECT" (DevZone, 2022): "nRF52840
//         (revision 3, build codes Fx0)" has the improved mechanism; it points at IN-141 for the
//         revision 3 changes. IN-141 itself could not be fetched from this machine (every URL
//         for it answers 404 or 403; tasks/phase-5/reports/02.md found the same), so the PS
//         chapter above stands in for it, as it did for the door.
//   DZ    Nordic DevZone 79941 ("How to find out nrf 52840 revision"): a Nordic engineer reads
//         "QIAAD0" as "AA = function variant code ... D = hardware revision code" and "revision
//         2"; DevZone 52606: the four characters are variant (two letters), hardware version (A,
//         B, C, D released at the time) and production configuration (a digit for production,
//         a letter for engineering samples).
//   MDK   Nordic's nrf52840_bitfields.h (nrfx MDK 8.30.2, on this machine) enumerates AAAA,
//         AAAB, AABA, AABB, AACA, BAAA, CAAA: the first two letters are not always "AA", which is
//         why the table keys on the build code alone and prints the rest.
//
// The table: hardware revision A, B, C or D -> legacy (hardware-only APPROTECT, UICR.APPROTECT
// left erased, which reads Disabled); F -> improved (revision 3, UICR.APPROTECT = 0x5A,
// HwDisabled); anything else - E, G and after, a letter where a digit should be, a word that is
// not four printable characters - is not in the table, and the command stops. The simulated
// chip's own model (host_test/sim/nrf52_chip.h) is the silicon's and is looser ("F and after");
// this table is what the firmware will *act* on, and it is strict on purpose: a part this file
// has never seen gets a person's attention, not a guess.
//
// A test extends the table by passing one of its own (lookupVariant's `table`): that is the
// "extend the table and run it again" of a radio whose variant stopped the command after the
// erase, shown on the host. No ESP-IDF include, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace provision {

enum class VariantKind : std::uint8_t {
  kUnknown,   // not in the table: the command stops
  kLegacy,    // build codes A-D: hardware-only APPROTECT, UICR.APPROTECT stays erased
  kImproved,  // build code F, revision 3: UICR.APPROTECT = 0x5A (HwDisabled)
};
const char* describe(VariantKind kind);

// One row: the build code's first character (INFO.VARIANT's third) and what it means.
struct VariantEntry {
  char build_code;
  VariantKind kind;
};

// The table, with its sources above.
constexpr VariantEntry kVariantTable[] = {
    {'A', VariantKind::kLegacy},
    {'B', VariantKind::kLegacy},
    {'C', VariantKind::kLegacy},
    {'D', VariantKind::kLegacy},
    {'F', VariantKind::kImproved},
};
constexpr std::size_t kVariantTableSize = sizeof kVariantTable / sizeof kVariantTable[0];

struct Variant {
  std::uint32_t raw = 0;
  char text[5] = {};            // the four characters, '?' for one that is not printable
  bool well_formed = false;     // two upper-case letters, a letter, a letter or a digit
  char build_code = 0;          // the third character, when well formed
  VariantKind kind = VariantKind::kUnknown;
};

// INFO.VARIANT -> the table's answer. `table` is kVariantTable unless a test says otherwise.
Variant lookupVariant(std::uint32_t info_variant, const VariantEntry* table = kVariantTable,
                      std::size_t table_size = kVariantTableSize);

}  // namespace provision
}  // namespace core
