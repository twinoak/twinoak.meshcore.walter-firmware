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
// indentation, a null field left out entirely, and a double written as the shortest decimal that
// reads back as itself (.NET Core 3.0 onward).
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

  // A JSON string, escaped exactly as System.Text.Json's default encoder escapes one
  // (JavaScriptEncoder.Default): `"` and `\` with a backslash; \b \f \n \r \t in their short
  // forms; every other C0 control, the HTML-sensitive `<` `>` `&` `'` `+` and the backtick as
  // \u00XX with uppercase hex; and every non-ASCII code point as \uXXXX, a surrogate pair above
  // the BMP. The text has already been bounded to well-formed UTF-8; a byte that is not (a cut
  // sequence) is written as U+FFFD, which is what the .NET side would have made of it too. Same
  // value either way - the point of matching the bytes is that a fixture can be compared byte
  // for byte, and tests/fixtures/contracts/ConfigReadResult.json carries a `>`.
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
      if (!ok) {
        unicode(0xFFFD);
        i++;
        continue;
      }
      if (cp >= 0x10000) {
        cp -= 0x10000;
        unicode(0xD800 + (cp >> 10));
        unicode(0xDC00 + (cp & 0x3FF));
      } else {
        unicode(cp);
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
      case '<': case '>': case '&': case '\'': case '+': case '`': unicode(c); return;
      default: break;
    }
    if (c < 0x20 || c == 0x7F) {
      unicode(c);
      return;
    }
    put(static_cast<char>(c));
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
