// The one JSON writer this firmware sends anything with.
//
// Bounded: it never writes past `cap`, and it remembers that it ran out, so a caller answers 0
// rather than half a body. Every envelope the board sends - the heartbeat, an observation batch,
// a job result - is written straight into the output buffer by this, rather than built as a
// document on the heap first: a twenty-item batch is 6 KB, and a board that is its own exit node
// has 320 KB of RAM in all.
//
// The bytes it makes are System.Text.Json's for the same value, which is what the contract
// fixtures hold both ends to: camelCase keys in the record's own field order, no spaces, no
// indentation, a null field left out entirely, a double written as the shortest decimal that
// reads back as itself (.NET Core 3.0 onward), and a string escaped as the **relaxed** encoder
// escapes one (JavaScriptEncoder.UnsafeRelaxedJsonEscaping). Until task p1.6-24 that last one
// was the default encoder's, which wrote `+`, `<`, `>`, `&`, `'`, the backtick and every
// character outside ASCII as \uXXXX; meshmaster.dk's task p4.15-129 moved the server to the
// relaxed one, because these envelopes are JSON to agents and never markup in a page - so a
// version's `+` is a `+` again, a `> ` in a CLI reply is a `> `, and a Danish repeater name
// travels as its own UTF-8 instead of six bytes a code point. The rule is written down one
// clause at a time in tests/fixtures/contracts/README.md and in meshmaster.dk's
// docs/dev/unit-protocol.md §8, tests/fixtures/contracts/JsonEscaping.json carries a character
// per clause, and test/test_json_escaping holds this writer to both.
//
// **Where this writer differs from the encoder, and the only way it does:** clause 7's
// *unassigned* code points. .NET escapes those as \uXXXX because it carries Unicode's table of
// what is assigned; this writer has no such table and will not grow one for a board with 320 KB
// of RAM, so an unassigned code point of the BMP is written as itself, in UTF-8. The enumerable
// half of that clause - the surrogates, the private-use area, the non-characters, the byte-order
// mark and the space separators - is ranges, and is implemented. Nothing a repeater or the
// server puts in front of this firmware is an unassigned code point in practice, and either way
// the two forms are the **same string** to a parser: the server reads both, which is what
// test_json_escaping holds for this clause (a parse of the bytes, not a byte comparison).
//
// Header-only and host-portable, so the native tests use it as the board does.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

class JsonWriter {
 public:
  JsonWriter(char* out, size_t cap) : out_(out), cap_(cap) {
    if (cap_ > 0) out_[0] = '\0';
  }

  void raw(const char* text) {
    if (text == nullptr) return;
    for (const char* p = text; *p != '\0'; p++) put(*p);
  }

  void put(char c) {
    if (len_ + 1 >= cap_) {
      overflow_ = true;
      return;
    }
    out_[len_++] = c;
    out_[len_] = '\0';
  }

  // A JSON string, escaped as System.Text.Json's **relaxed** encoder escapes one
  // (JavaScriptEncoder.UnsafeRelaxedJsonEscaping), clause by clause as
  // tests/fixtures/contracts/README.md states the rule and JsonEscaping.json proves it:
  //
  //   1  a quote and a backslash                     a backslash and the character
  //   2  backspace, tab, LF, FF, CR                  the five short forms
  //   3  every other control U+0001-U+001F, DEL,     \u00XX, with uppercase hex
  //      the C1 controls U+0080-U+009F, NBSP
  //   4  U+2028 and U+2029                             and
  //   5  every other assigned character of the BMP   itself, in UTF-8 - the whole of what the
  //                                                  default encoder escaped for HTML's sake
  //                                                  (`+ < > & '`, the backtick, `/`), and
  //                                                  every letter: `Å`, `…`, the soft hyphen
  //   6  a code point above the BMP                  a surrogate pair, uppercase hex
  //   7  private use, the non-characters, the BOM,   \uXXXX - and see the header: an
  //      the space separators other than a space     *unassigned* code point is clause 5 here
  //   8  a surrogate, or a byte that is not UTF-8    �; it is not a character
  //
  // Clause 5 is what task p1.6-24 changed, and it is where every byte this writer now saves
  // comes from: tests/fixtures/contracts/ConfigReadResult.json carries a plain `>`, and a
  // repeater called Grenå costs six bytes in a name instead of ten.
  void string(const char* text) {
    if (text == nullptr) {
      string(text, 0);
      return;
    }
    string(text, std::strlen(text));
  }

  // The same, for text that is not NUL-terminated or is longer than it should be.
  void string(const char* text, size_t len) {
    put('"');
    size_t i = 0;
    while (text != nullptr && i < len && text[i] != '\0') {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c < 0x80) {
        escape(c);
        i++;
        continue;
      }
      // A UTF-8 sequence: its length from the lead byte, and the code point out of it.
      size_t need = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 0));
      uint32_t cp = need == 4 ? (c & 0x07u) : (need == 3 ? (c & 0x0Fu) : (c & 0x1Fu));
      bool ok = need > 0 && i + need <= len;
      for (size_t k = 1; ok && k < need; k++) {
        const unsigned char cc = static_cast<unsigned char>(text[i + k]);
        if ((cc & 0xC0) != 0x80) ok = false;
        cp = (cp << 6) | (cc & 0x3Fu);
      }
      if (!ok || (cp >= 0xD800 && cp <= 0xDFFF)) {
        // Clause 8: a cut or malformed sequence, and a surrogate that was encoded as if it were
        // a character, which is what .NET makes of a lone one in a UTF-16 string.
        unicode(0xFFFD);
        i += ok ? need : 1;
        continue;
      }
      if (cp >= 0x10000) {
        cp -= 0x10000;
        unicode(0xD800 + (cp >> 10));
        unicode(0xDC00 + (cp & 0x3FF));
      } else if (cp < 0x80) {
        escape(static_cast<unsigned char>(cp));  // an overlong encoding of an ASCII byte
      } else if (escapedAboveAscii(cp)) {
        unicode(cp);
      } else {
        for (size_t k = 0; k < need; k++) put(text[i + k]);  // clause 5: the character itself
      }
      i += need;
    }
    put('"');
  }

  void integer(long long v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%lld", v);
    raw(buf);
  }

  void boolean(bool v) { raw(v ? "true" : "false"); }

  // The shortest decimal that reads back as the same double, which is what System.Text.Json
  // writes. Every number this firmware sends (a frequency, an SNR, a voltage, a round trip) is
  // in a range where %g never chooses the exponent form, and the process never calls setlocale(),
  // so the decimal point is a point.
  void number(double v) {
    char buf[40];
    if (std::isnan(v) || std::isinf(v)) {
      raw("0");
      return;
    }
    for (int digits = 15; digits <= 17; digits++) {
      std::snprintf(buf, sizeof buf, "%.*g", digits, v);
      if (std::strtod(buf, nullptr) == v) break;
    }
    raw(buf);
  }

  void key(const char* name) {
    comma();
    string(name);
    put(':');
  }

  // Between two members of the same object or array. A container that has just been closed
  // leaves `first_` false, which is right: whatever follows it in the parent needs a comma.
  void comma() {
    if (!first_) put(',');
    first_ = false;
  }

  void beginObject() {
    put('{');
    first_ = true;
  }

  void endObject() {
    put('}');
    first_ = false;
  }

  void beginArray() {
    put('[');
    first_ = true;
  }

  void endArray() {
    put(']');
    first_ = false;
  }

  bool overflowed() const { return overflow_; }
  size_t length() const { return len_; }

 private:
  void escape(unsigned char c) {
    switch (c) {
      case '"': raw("\\\""); return;
      case '\\': raw("\\\\"); return;
      case '\b': raw("\\b"); return;
      case '\f': raw("\\f"); return;
      case '\n': raw("\\n"); return;
      case '\r': raw("\\r"); return;
      case '\t': raw("\\t"); return;
      default: break;
    }
    if (c < 0x20 || c == 0x7F) {
      unicode(c);
      return;
    }
    put(static_cast<char>(c));
  }

  // Clauses 3, 4 and 7 for a code point of the BMP above ASCII: everything the relaxed encoder
  // still escapes and that is a range rather than a table. The one thing left out is clause 7's
  // unassigned code points, which need Unicode's own table; the header says so.
  static bool escapedAboveAscii(uint32_t cp) {
    if (cp <= 0xA0) return true;                                      // the C1 controls, and NBSP
    if (cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A)) return true;  // space separators (Zs)
    if (cp == 0x2028 || cp == 0x2029) return true;                    // line, paragraph (Zl, Zp)
    if (cp == 0x202F || cp == 0x205F || cp == 0x3000) return true;    // the rest of the Zs
    if (cp >= 0xE000 && cp <= 0xF8FF) return true;                    // private use
    if (cp >= 0xFDD0 && cp <= 0xFDEF) return true;                    // the non-characters
    if (cp == 0xFEFF || cp == 0xFFFE || cp == 0xFFFF) return true;    // the BOM and the top two
    return false;
  }

  void unicode(uint32_t unit) {
    char esc[8];
    std::snprintf(esc, sizeof esc, "\\u%04X", static_cast<unsigned>(unit & 0xFFFF));
    raw(esc);
  }

  char* out_;
  size_t cap_;
  size_t len_ = 0;
  bool overflow_ = false;
  bool first_ = true;
};

}  // namespace mm
