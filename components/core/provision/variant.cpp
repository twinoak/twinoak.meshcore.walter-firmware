// The variant table (task p5-03): see variant.h for the documents.
#include "core/provision/variant.h"

namespace core {
namespace provision {

const char* describe(VariantKind kind) {
  switch (kind) {
    case VariantKind::kUnknown: return "NOT IN THE TABLE (core/provision/variant.h): the command stops";
    case VariantKind::kLegacy: return "legacy APPROTECT (build code A-D): UICR.APPROTECT left erased";
    case VariantKind::kImproved: return "improved APPROTECT (build code F, revision 3): UICR.APPROTECT = 0x5A";
  }
  return "?";
}

namespace {

bool upperLetter(char c) { return c >= 'A' && c <= 'Z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

// PS FICR INFO.VARIANT: "encoded in ASCII", the first character in the top byte (regs.h). The
// word is read as text first; the table is consulted only for a well-formed one, so that
// 0xFFFFFFFF ("Unspecified") and a misread cannot land on a row by accident.
Variant lookupVariant(std::uint32_t info_variant, const VariantEntry* table, std::size_t table_size) {
  Variant v;
  v.raw = info_variant;
  for (int i = 0; i < 4; ++i) {
    const char c = static_cast<char>((info_variant >> (24 - 8 * i)) & 0xFFu);
    v.text[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  v.text[4] = 0;
  // Two letters of package variant, a letter of hardware revision, a digit (production) or a
  // letter (an engineering sample) of configuration (PS, DZ).
  v.well_formed = upperLetter(v.text[0]) && upperLetter(v.text[1]) && upperLetter(v.text[2]) &&
                  (digit(v.text[3]) || upperLetter(v.text[3]));
  if (!v.well_formed) return v;
  v.build_code = v.text[2];
  for (std::size_t i = 0; i < table_size; ++i) {
    if (table[i].build_code == v.build_code) {
      v.kind = table[i].kind;
      return v;
    }
  }
  return v;
}

}  // namespace provision
}  // namespace core
