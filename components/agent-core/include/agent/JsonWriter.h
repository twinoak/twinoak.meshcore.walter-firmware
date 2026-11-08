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
  //   8  a surrogate, or a byte sequence that        �, one per **maximal subpart** of the
  //      is not well-formed UTF-8                    ill-formed sequence; it is not a character
  //
  // Clause 5 is what task p1.6-24 changed, and it is where every byte this writer now saves
  // comes from: tests/fixtures/contracts/ConfigReadResult.json carries a plain `>`, and a
  // repeater called Grenå costs six bytes in a name instead of ten. Clause 8 is what task
  // p1.7-25 finished: the spec's rule is .NET's, whose strings are UTF-16 and whose only
  // ill-formed input is a lone surrogate, and this end reads **bytes** - so the clause is the
  // same answer over a wider input, and `string()` below is where the whole of it is.
  void string(const char* text) {
    if (text == nullptr) {
      string(text, 0);
      return;
    }
    string(text, std::strlen(text));
  }

  // The same, for text that is not NUL-terminated or is longer than it should be.
  //
  // This is also the only decoder between a stranger's bytes and the wire, and the bytes are a
  // stranger's: an advert's name is whatever was put in the advert and a CLI reply is whatever
  // the repeater's console answered. A JSON document is UTF-8 by definition and
  // System.Text.Json refuses one that is not - and a result the server refuses is kept and
  // retried at the head of this agent's ordered queue, blocking the results behind it until the
  // buffer evicts it. So **no output of this writer is ill-formed UTF-8, whatever came in**
  // (task p1.7-25): every byte that is not part of a well-formed sequence is U+FFFD here.
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
      // A UTF-8 sequence as **Table 3-7** of the standard defines one: the lead byte gives the
      // length *and* the range the byte after it may be in. That range is what makes an overlong
      // form (C0, C1, E0 80, F0 80), a surrogate (ED A0) and a code point above U+10FFFF
      // (F4 90, F5-FF) ill-formed at the byte that proves it, rather than at the code point they
      // would otherwise have decoded to - which is how such bytes used to reach the wire as
      // themselves.
      size_t need = 0;
      unsigned char lo = 0x80;
      unsigned char hi = 0xBF;
      if (c >= 0xC2 && c <= 0xDF) {
        need = 2;
      } else if (c >= 0xE0 && c <= 0xEF) {
        need = 3;
        if (c == 0xE0) lo = 0xA0;  // E0 80..9F: an overlong two-byte form
        if (c == 0xED) hi = 0x9F;  // ED A0..BF: a surrogate
      } else if (c >= 0xF0 && c <= 0xF4) {
        need = 4;
        if (c == 0xF0) lo = 0x90;  // F0 80..8F: an overlong three-byte form
        if (c == 0xF4) hi = 0x8F;  // F4 90..BF: above U+10FFFF
      }
      if (need == 0) {
        // A continuation byte on its own, C0 or C1 (nothing but an overlong form can follow
        // either), or F5-FF (nothing but a code point there is none of): no well-formed sequence
        // begins with this byte, so the maximal subpart is the byte itself.
        unicode(0xFFFD);
        i++;
        continue;
      }
      uint32_t cp = need == 4 ? (c & 0x07u) : (need == 3 ? (c & 0x0Fu) : (c & 0x1Fu));
      size_t k = 1;
      for (; k < need; k++) {
        if (i + k >= len || text[i + k] == '\0') break;  // cut: by the buffer, or by a NUL in it
        const unsigned char cc = static_cast<unsigned char>(text[i + k]);
        if (cc < (k == 1 ? lo : 0x80) || cc > (k == 1 ? hi : 0xBF)) break;
        cp = (cp << 6) | (cc & 0x3Fu);
      }
      if (k < need) {
        // Clause 8, by Unicode 3.9's **maximal subpart**: one U+FFFD for the longest prefix that
        // could still have become a character - the `k` bytes read - and the byte that broke it
        // is then read as a byte of its own. So a cut `E2 82 41` is one U+FFFD and an `A`, and a
        // sequence cut at the end of the buffer is one U+FFFD. It is what .NET makes of the same
        // bytes.
        unicode(0xFFFD);
        i += k;
        continue;
      }
      if (cp >= 0x10000) {
        cp -= 0x10000;
        unicode(0xD800 + (cp >> 10));
        unicode(0xDC00 + (cp & 0x3FF));
      } else if (escapedAboveAscii(cp)) {
        unicode(cp);
      } else {
        for (size_t b = 0; b < need; b++) put(text[i + b]);  // clause 5: the character itself
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
