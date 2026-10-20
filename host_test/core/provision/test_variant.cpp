// The variant table (task p5-03): a lookup from INFO.VARIANT, never a guess - A-D legacy, F
// improved, everything else refused, and the word read as text first so that "Unspecified" and
// a misread cannot land on a row.
#include <doctest.h>

#include <string>

#include "core/provision/variant.h"

using core::provision::kVariantTable;
using core::provision::kVariantTableSize;
using core::provision::lookupVariant;
using core::provision::Variant;
using core::provision::VariantEntry;
using core::provision::VariantKind;

TEST_CASE("the table: A, B, C, D are legacy, F is improved, E and after are not in it") {
  struct Case {
    std::uint32_t raw;
    const char* text;
    VariantKind kind;
  };
  const Case cases[] = {
      {0x41414141, "AAAA", VariantKind::kLegacy},  // MDK: an engineering sample of hardware A
      {0x41414241, "AABA", VariantKind::kLegacy},
      {0x41414330, "AAC0", VariantKind::kLegacy},
      {0x41414430, "AAD0", VariantKind::kLegacy},  // DevZone 79941: revision 2
      {0x41414431, "AAD1", VariantKind::kLegacy},
      {0x41414630, "AAF0", VariantKind::kImproved},  // NOTE: revision 3, build codes Fx0
      {0x41414631, "AAF1", VariantKind::kImproved},
      {0x42414141, "BAAA", VariantKind::kLegacy},  // MDK: another package variant, hardware A
      {0x41414530, "AAE0", VariantKind::kUnknown},  // E: engineering, never in the table
      {0x41414730, "AAG0", VariantKind::kUnknown},  // after F: not seen, not guessed
      {0x41415A30, "AAZ0", VariantKind::kUnknown},
  };
  for (const Case& c : cases) {
    const Variant v = lookupVariant(c.raw);
    CHECK(std::string(v.text) == c.text);
    CHECK(v.well_formed);
    CHECK(v.kind == c.kind);
    CHECK(v.raw == c.raw);
    if (c.kind != VariantKind::kUnknown) CHECK(v.build_code == c.text[2]);
  }
}

TEST_CASE("a word that is not a variant is not looked up at all") {
  // FICR's "Unspecified", a word with a digit where a letter belongs, lower case, binary.
  for (const std::uint32_t raw : {0xFFFFFFFFu, 0x41413030u, 0x61616630u, 0x00000000u, 0x41414600u}) {
    const Variant v = lookupVariant(raw);
    CHECK(!v.well_formed);
    CHECK(v.kind == VariantKind::kUnknown);
    CHECK(v.build_code == 0);
  }
  CHECK(std::string(lookupVariant(0xFFFFFFFF).text) == "????");
  CHECK(std::string(lookupVariant(0x41414600).text) == "AAF?");
}

TEST_CASE("an engineering sample's letter in the configuration place is well formed; a digit in the revision place is not") {
  CHECK(lookupVariant(0x41414641).well_formed);   // "AAFA"
  CHECK(lookupVariant(0x41414641).kind == VariantKind::kImproved);
  CHECK(!lookupVariant(0x41413130).well_formed);  // "AA10"
}

TEST_CASE("a test can extend the table, which is what a person does for a radio the table stopped") {
  CHECK(lookupVariant(0x41414530).kind == VariantKind::kUnknown);
  const VariantEntry extended[] = {
      {'A', VariantKind::kLegacy}, {'B', VariantKind::kLegacy}, {'C', VariantKind::kLegacy},
      {'D', VariantKind::kLegacy}, {'F', VariantKind::kImproved}, {'E', VariantKind::kLegacy},
  };
  CHECK(lookupVariant(0x41414530, extended, 6).kind == VariantKind::kLegacy);
  // And the built-in table is what the header says it is: five rows, nothing for E.
  CHECK(kVariantTableSize == 5);
  for (std::size_t i = 0; i < kVariantTableSize; ++i) CHECK(kVariantTable[i].build_code != 'E');
}

TEST_CASE("describe() says what the command will do with each kind") {
  CHECK(std::string(core::provision::describe(VariantKind::kLegacy)).find("erased") != std::string::npos);
  CHECK(std::string(core::provision::describe(VariantKind::kImproved)).find("0x5A") != std::string::npos);
  CHECK(std::string(core::provision::describe(VariantKind::kUnknown)).find("NOT IN THE TABLE") != std::string::npos);
}
