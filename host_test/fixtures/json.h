// A JSON reader for the tests, and nothing more: spec/fixtures/link/link-v1.json is read at run
// time rather than turned into a header, because the link fixtures are a table of cases and the
// point of the coverage test is to notice a case that was added (task p1-01).
//
// Test-side code: it allocates, it throws, and it is not compiled into anything that runs on the
// board. Two things in it are not ordinary JSON handling and are the reason it is here instead of
// a library:
//
//   - **every key read is remembered**, so a test can ask an object which of its keys nobody
//     looked at. That is what makes "a fixture that gains a case, or a field, fails the tests" a
//     property of the tests and not of someone's attention;
//   - **numbers are integers**, parsed into long long and refused if they are not: every number
//     in these fixtures is a byte, a length, a counter or a timestamp, and a double would quietly
//     round the ones that are 32 bits wide.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace jsonfix {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
 public:
  enum class Kind { kNull, kBool, kInt, kString, kArray, kObject };

  Value() = default;
  explicit Value(bool value) : kind_(Kind::kBool), boolean_(value) {}
  explicit Value(long long value) : kind_(Kind::kInt), integer_(value) {}
  explicit Value(std::string value) : kind_(Kind::kString), string_(std::move(value)) {}
  explicit Value(Array value) : kind_(Kind::kArray), array_(std::move(value)) {}
  explicit Value(Object value) : kind_(Kind::kObject), object_(std::move(value)) {}

  Kind kind() const { return kind_; }
  bool isNull() const { return kind_ == Kind::kNull; }
  bool isObject() const { return kind_ == Kind::kObject; }
  bool isArray() const { return kind_ == Kind::kArray; }

  bool boolean() const {
    expect(Kind::kBool);
    return boolean_;
  }
  long long integer() const {
    expect(Kind::kInt);
    return integer_;
  }
  const std::string& string() const {
    expect(Kind::kString);
    return string_;
  }
  const Array& array() const {
    expect(Kind::kArray);
    return array_;
  }
  const Object& object() const {
    expect(Kind::kObject);
    return object_;
  }

  std::size_t size() const {
    if (kind_ == Kind::kArray) {
      return array_.size();
    }
    expect(Kind::kObject);
    return object_.size();
  }

  bool has(const std::string& key) const {
    expect(Kind::kObject);
    return object_.find(key) != object_.end();
  }

  // The member, and a note that somebody read it.
  const Value& operator[](const std::string& key) const {
    expect(Kind::kObject);
    const auto at = object_.find(key);
    if (at == object_.end()) {
      throw std::runtime_error("the fixture has no key \"" + key + "\"");
    }
    read_.insert(key);
    return at->second;
  }

  const Value& at(std::size_t index) const {
    expect(Kind::kArray);
    if (index >= array_.size()) {
      throw std::runtime_error("the fixture's array is shorter than " + std::to_string(index + 1));
    }
    return array_[index];
  }

  // The keys of this object nobody has read yet, comma-separated and in order - empty when every
  // one of them was used. A test prints this, so it has to read like a sentence.
  std::string unreadKeys() const {
    expect(Kind::kObject);
    std::string out;
    for (const auto& entry : object_) {
      if (read_.find(entry.first) == read_.end()) {
        out += out.empty() ? "" : ", ";
        out += entry.first;
      }
    }
    return out;
  }

  // Every key, read or not, so that a test can say what was there.
  std::string keys() const {
    expect(Kind::kObject);
    std::string out;
    for (const auto& entry : object_) {
      out += out.empty() ? "" : ", ";
      out += entry.first;
    }
    return out;
  }

 private:
  void expect(Kind kind) const {
    if (kind_ != kind) {
      throw std::runtime_error("the fixture's value is not of the kind the test asked for");
    }
  }

  Kind kind_ = Kind::kNull;
  bool boolean_ = false;
  long long integer_ = 0;
  std::string string_;
  Array array_;
  Object object_;
  mutable std::set<std::string> read_;
};

namespace detail {

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  Value parse() {
    const Value value = parseValue();
    skipSpace();
    if (at_ != text_.size()) {
      fail("trailing text");
    }
    return value;
  }

 private:
  [[noreturn]] void fail(const std::string& what) const {
    throw std::runtime_error("the fixture is not the JSON the test expects at byte " +
                             std::to_string(at_) + ": " + what);
  }

  void skipSpace() {
    while (at_ < text_.size() &&
           (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
      ++at_;
    }
  }

  char peek() {
    skipSpace();
    if (at_ >= text_.size()) {
      fail("the text ends");
    }
    return text_[at_];
  }

  void expect(char c) {
    if (peek() != c) {
      fail(std::string("expected '") + c + "'");
    }
    ++at_;
  }

  bool literal(const char* word) {
    const std::size_t n = std::string(word).size();
    if (text_.compare(at_, n, word) == 0) {
      at_ += n;
      return true;
    }
    return false;
  }

  Value parseValue() {
    switch (peek()) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': return Value(parseString());
      case 't': if (literal("true")) { return Value(true); } fail("true"); break;
      case 'f': if (literal("false")) { return Value(false); } fail("false"); break;
      case 'n': if (literal("null")) { return Value(); } fail("null"); break;
      default: return parseNumber();
    }
    fail("a value");
  }

  Value parseObject() {
    expect('{');
    Object object;
    if (peek() == '}') {
      ++at_;
      return Value(std::move(object));
    }
    for (;;) {
      const std::string key = parseString();
      expect(':');
      object.emplace(key, parseValue());
      const char c = peek();
      ++at_;
      if (c == '}') {
        return Value(std::move(object));
      }
      if (c != ',') {
        fail("expected ',' or '}'");
      }
    }
  }

  Value parseArray() {
    expect('[');
    Array array;
    if (peek() == ']') {
      ++at_;
      return Value(std::move(array));
    }
    for (;;) {
      array.push_back(parseValue());
      const char c = peek();
      ++at_;
      if (c == ']') {
        return Value(std::move(array));
      }
      if (c != ',') {
        fail("expected ',' or ']'");
      }
    }
  }

  std::string parseString() {
    expect('"');
    std::string out;
    for (;;) {
      if (at_ >= text_.size()) {
        fail("the string does not end");
      }
      const char c = text_[at_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (at_ >= text_.size()) {
        fail("the escape does not end");
      }
      const char escape = text_[at_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': out += parseUnicodeEscape(); break;
        default: fail("an escape the test does not know");
      }
    }
  }

  // linkref.py writes the fixtures with ensure_ascii=True, so a \u escape can only be an ASCII
  // character or something the tests were never meant to read.
  std::string parseUnicodeEscape() {
    if (at_ + 4 > text_.size()) {
      fail("a short \\u escape");
    }
    unsigned code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[at_++];
      code <<= 4;
      if (c >= '0' && c <= '9') {
        code |= static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        code |= static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        code |= static_cast<unsigned>(c - 'A' + 10);
      } else {
        fail("a \\u escape that is not hexadecimal");
      }
    }
    if (code > 0x7F) {
      fail("a \\u escape outside ASCII, which these fixtures do not have");
    }
    return std::string(1, static_cast<char>(code));
  }

  Value parseNumber() {
    const std::size_t start = at_;
    if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) {
      ++at_;
    }
    while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
      ++at_;
    }
    if (at_ == start) {
      fail("a number");
    }
    if (at_ < text_.size() && (text_[at_] == '.' || text_[at_] == 'e' || text_[at_] == 'E')) {
      fail("a number that is not an integer; every number in these fixtures is one");
    }
    return Value(std::stoll(text_.substr(start, at_ - start)));
  }

  const std::string& text_;
  std::size_t at_ = 0;
};

}  // namespace detail

inline Value parse(const std::string& text) { return detail::Parser(text).parse(); }

}  // namespace jsonfix
