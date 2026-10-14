#include "core/net/http1.h"

#include <cstring>

namespace core {
namespace net {

namespace {

bool isTokenChar(std::uint8_t c) {
  // RFC 7230 §3.2.6: tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." / "^" /
  // "_" / "`" / "|" / "~" / DIGIT / ALPHA.
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-':
    case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool sameName(const char* a, const char* b) {
  for (;; ++a, ++b) {
    if (lower(*a) != lower(*b)) return false;
    if (*a == 0) return true;
  }
}

bool isWhitespace(std::uint8_t c) { return c == ' ' || c == '\t'; }

// Appends `text` to out at *at, bounded; false when it does not fit.
bool append(char* out, std::size_t capacity, std::size_t* at, const char* text) {
  const std::size_t n = std::strlen(text);
  if (*at + n >= capacity) return false;
  std::memcpy(out + *at, text, n);
  *at += n;
  return true;
}

bool appendNumber(char* out, std::size_t capacity, std::size_t* at, std::uint64_t value) {
  char digits[21];
  std::size_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  if (*at + n >= capacity) return false;
  while (n > 0) out[(*at)++] = digits[--n];
  return true;
}

// A header name or value may carry no CR or LF: a value that did would end the line early and
// let a caller's string inject a header of its own.
bool cleanHeaderText(const char* text) {
  if (text == nullptr) return false;
  for (const char* p = text; *p != 0; ++p) {
    if (*p == '\r' || *p == '\n') return false;
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------------------------------ the writer

std::size_t writeRequestHead(const interfaces::HttpRequest& request, const char* user_agent,
                             char* out, std::size_t capacity) {
  if (out == nullptr || capacity == 0 || request.host == nullptr || request.host[0] == 0) return 0;
  if (request.path == nullptr || request.path[0] != '/') return 0;
  if (request.header_count > interfaces::kMaxRequestHeaders) return 0;
  if (!cleanHeaderText(request.host) || !cleanHeaderText(request.path)) return 0;
  for (const char* p = request.path; *p != 0; ++p) {
    if (*p == ' ' || static_cast<unsigned char>(*p) < 0x21 || static_cast<unsigned char>(*p) > 0x7E) return 0;
  }
  std::size_t at = 0;
  bool ok = append(out, capacity, &at, interfaces::describe(request.method)) &&
            append(out, capacity, &at, " ") && append(out, capacity, &at, request.path) &&
            append(out, capacity, &at, " HTTP/1.1\r\nHost: ") &&
            append(out, capacity, &at, request.host);
  if (ok && request.port != 443) {
    ok = append(out, capacity, &at, ":") && appendNumber(out, capacity, &at, request.port);
  }
  ok = ok && append(out, capacity, &at, "\r\nUser-Agent: ") &&
       append(out, capacity, &at, user_agent != nullptr ? user_agent : "twinoak-walter") &&
       append(out, capacity, &at, "\r\nAccept: */*\r\nConnection: close\r\n");
  for (std::size_t i = 0; ok && i < request.header_count; ++i) {
    const interfaces::HttpHeader& h = request.headers[i];
    if (h.name == nullptr || h.name[0] == 0 || !cleanHeaderText(h.name) || !cleanHeaderText(h.value)) {
      return 0;
    }
    for (const char* p = h.name; *p != 0; ++p) {
      if (!isTokenChar(static_cast<std::uint8_t>(*p))) return 0;
    }
    ok = append(out, capacity, &at, h.name) && append(out, capacity, &at, ": ") &&
         append(out, capacity, &at, h.value) && append(out, capacity, &at, "\r\n");
  }
  if (ok && request.method == interfaces::HttpMethod::kPost) {
    ok = append(out, capacity, &at, "Content-Length: ") &&
         appendNumber(out, capacity, &at, request.body_size) && append(out, capacity, &at, "\r\n");
  }
  ok = ok && append(out, capacity, &at, "\r\n");
  if (!ok) return 0;
  out[at] = 0;
  return at;
}

// ------------------------------------------------------------------------------------ the parser

const char* describe(ParseError error) {
  switch (error) {
    case ParseError::kNone: return "ok";
    case ParseError::kBadStatusLine: return "the status line is not HTTP/1.x and three digits";
    case ParseError::kInformational: return "a 1xx status, which this firmware never asks for";
    case ParseError::kBadLineEnding: return "a bare CR where a line should end";
    case ParseError::kObsoleteFold: return "a folded header line (obsolete, refused)";
    case ParseError::kBadHeaderName: return "a header name that is not a token, or too long";
    case ParseError::kNoColon: return "a header line without a colon";
    case ParseError::kBadContentLength: return "Content-Length is not a number, or two disagree";
    case ParseError::kBadTransferEncoding: return "a transfer coding other than chunked, or chunked with Content-Length";
    case ParseError::kBadChunk: return "a chunk whose size line is not hex, or not ended by CR LF";
    case ParseError::kTooLong: return "a header longer than this firmware holds";
    case ParseError::kSinkStopped: return "the body was refused by the sink";
  }
  return "?";
}

void ResponseParser::begin(bool expect_body, interfaces::HttpWantedHeader* wanted,
                           std::size_t wanted_count) {
  state_ = ParseState::kStatusLine;
  error_ = ParseError::kNone;
  expect_body_ = expect_body;
  wanted_ = wanted;
  wanted_count_ = wanted != nullptr ? wanted_count : 0;
  for (std::size_t i = 0; i < wanted_count_; ++i) {
    wanted_[i].present = false;
    wanted_[i].truncated = false;
    if (wanted_[i].value != nullptr && wanted_[i].capacity > 0) wanted_[i].value[0] = 0;
  }
  line_len_ = 0;
  in_reason_ = false;
  saw_cr_ = false;
  name_len_ = 0;
  value_len_ = 0;
  value_started_ = false;
  wanted_index_ = -1;
  wanted_written_ = 0;
  own_ = Own::kNone;
  own_len_ = 0;
  own_overflow_ = false;
  status_ = 0;
  has_content_length_ = false;
  content_length_ = 0;
  chunked_ = false;
  until_close_ = false;
  header_bytes_ = 0;
  body_bytes_ = 0;
  body_left_ = 0;
  chunk_size_ = 0;
  chunk_digits_ = 0;
  in_chunk_ext_ = false;
  trailer_line_empty_ = true;
}

void ResponseParser::fail(ParseError error) {
  state_ = ParseState::kError;
  error_ = error;
}

bool ResponseParser::deliver(const std::uint8_t* data, std::size_t size,
                             interfaces::HttpBodySink& sink) {
  if (size == 0) return true;
  body_bytes_ += size;
  if (!sink.piece(data, size)) {
    fail(ParseError::kSinkStopped);
    return false;
  }
  return true;
}

// `HTTP/1.` d SP ddd (SP reason)? CR LF
bool ResponseParser::statusByte(std::uint8_t c) {
  if (c == '\r') {
    saw_cr_ = true;
    return true;
  }
  if (c == '\n') {
    saw_cr_ = false;
    // The line is over: what was accumulated must be the whole status part.
    if (line_len_ < 12) {
      fail(ParseError::kBadStatusLine);
      return false;
    }
    const std::uint16_t code = static_cast<std::uint16_t>((line_[9] - '0') * 100 + (line_[10] - '0') * 10 + (line_[11] - '0'));
    if (code < 100) {
      fail(ParseError::kBadStatusLine);
      return false;
    }
    if (code < 200) {
      fail(ParseError::kInformational);
      return false;
    }
    status_ = code;
    state_ = ParseState::kHeaderStart;
    return true;
  }
  if (saw_cr_) {
    fail(ParseError::kBadLineEnding);
    return false;
  }
  if (in_reason_) return true;  // the reason phrase: ignored
  // Accumulate `HTTP/1.x NNN` (12 characters), then either a space (the reason follows) or the
  // line end; anything else at any position is not a status line.
  static const char kPrefix[] = "HTTP/1.";
  if (line_len_ < 7) {
    if (c != static_cast<std::uint8_t>(kPrefix[line_len_])) {
      fail(ParseError::kBadStatusLine);
      return false;
    }
  } else if (line_len_ == 7) {
    if (c != '0' && c != '1') {
      fail(ParseError::kBadStatusLine);
      return false;
    }
  } else if (line_len_ == 8) {
    if (c != ' ') {
      fail(ParseError::kBadStatusLine);
      return false;
    }
  } else if (line_len_ < 12) {
    if (c < '0' || c > '9') {
      fail(ParseError::kBadStatusLine);
      return false;
    }
  } else {
    if (c != ' ') {
      fail(ParseError::kBadStatusLine);
      return false;
    }
    in_reason_ = true;
    return true;
  }
  line_[line_len_++] = static_cast<char>(c);
  return true;
}

void ResponseParser::headerDone() {
  // The end of one header line: close out what it was.
  if (wanted_index_ >= 0) {
    interfaces::HttpWantedHeader& w = wanted_[static_cast<std::size_t>(wanted_index_)];
    // Trailing whitespace is not part of the value (RFC 7230 §3.2.4).
    while (wanted_written_ > 0 && isWhitespace(static_cast<std::uint8_t>(w.value[wanted_written_ - 1]))) {
      --wanted_written_;
    }
    if (w.value != nullptr && w.capacity > 0) w.value[wanted_written_] = 0;
    w.present = true;
  }
  if (own_ != Own::kNone) {
    while (own_len_ > 0 && isWhitespace(static_cast<std::uint8_t>(own_value_[own_len_ - 1]))) --own_len_;
    own_value_[own_len_] = 0;
    if (own_ == Own::kContentLength) {
      // Digits only, at most ten, inside 32 bits (§3.3.2). A second one must agree.
      if (own_overflow_ || own_len_ == 0 || own_len_ > 10) {
        fail(ParseError::kBadContentLength);
        return;
      }
      std::uint64_t value = 0;
      for (std::size_t i = 0; i < own_len_; ++i) {
        if (own_value_[i] < '0' || own_value_[i] > '9') {
          fail(ParseError::kBadContentLength);
          return;
        }
        value = value * 10 + static_cast<std::uint64_t>(own_value_[i] - '0');
      }
      if (value > 0xFFFFFFFFull) {
        fail(ParseError::kBadContentLength);
        return;
      }
      if (has_content_length_ && content_length_ != static_cast<std::uint32_t>(value)) {
        fail(ParseError::kBadContentLength);
        return;
      }
      has_content_length_ = true;
      content_length_ = static_cast<std::uint32_t>(value);
    } else {
      // `chunked`, alone, case-insensitively (§3.3.1: a sender may use `Chunked`).
      if (own_overflow_ || !sameName(own_value_, "chunked")) {
        fail(ParseError::kBadTransferEncoding);
        return;
      }
      chunked_ = true;
    }
  }
  name_len_ = 0;
  value_len_ = 0;
  value_started_ = false;
  wanted_index_ = -1;
  wanted_written_ = 0;
  own_ = Own::kNone;
  own_len_ = 0;
  own_overflow_ = false;
  state_ = ParseState::kHeaderStart;
}

void ResponseParser::endOfHeaders() {
  if (chunked_ && has_content_length_) {
    fail(ParseError::kBadTransferEncoding);
    return;
  }
  // §3.3.3: a HEAD response, 204 and 304 carry no body whatever the headers say.
  if (!expect_body_ || status_ == 204 || status_ == 304) {
    state_ = ParseState::kDone;
    return;
  }
  if (chunked_) {
    state_ = ParseState::kChunkSize;
    chunk_size_ = 0;
    chunk_digits_ = 0;
    in_chunk_ext_ = false;
    return;
  }
  if (has_content_length_) {
    body_left_ = content_length_;
    state_ = body_left_ == 0 ? ParseState::kDone : ParseState::kBodyLength;
    return;
  }
  until_close_ = true;
  state_ = ParseState::kBodyUntilClose;
}

bool ResponseParser::headerByte(std::uint8_t c) {
  if (++header_bytes_ > kMaxHeaderBytes) {
    fail(ParseError::kTooLong);
    return false;
  }
  if (c == '\r') {
    if (saw_cr_) {
      fail(ParseError::kBadLineEnding);
      return false;
    }
    saw_cr_ = true;
    return true;
  }
  if (c == '\n') {
    saw_cr_ = false;
    if (state_ == ParseState::kHeaderStart) {
      endOfHeaders();  // the empty line
      return state_ != ParseState::kError;
    }
    if (state_ == ParseState::kHeaderName) {
      fail(ParseError::kNoColon);
      return false;
    }
    headerDone();
    return state_ != ParseState::kError;
  }
  if (saw_cr_) {
    fail(ParseError::kBadLineEnding);
    return false;
  }
  if (state_ == ParseState::kHeaderStart) {
    if (isWhitespace(c)) {
      fail(ParseError::kObsoleteFold);
      return false;
    }
    state_ = ParseState::kHeaderName;
  }
  if (state_ == ParseState::kHeaderName) {
    if (c == ':') {
      name_[name_len_] = 0;
      if (name_len_ == 0) {
        fail(ParseError::kBadHeaderName);
        return false;
      }
      for (std::size_t i = 0; i < wanted_count_; ++i) {
        if (wanted_[i].name != nullptr && sameName(name_, wanted_[i].name)) {
          wanted_index_ = static_cast<int>(i);
          break;
        }
      }
      if (sameName(name_, "Content-Length")) own_ = Own::kContentLength;
      if (sameName(name_, "Transfer-Encoding")) own_ = Own::kTransferEncoding;
      state_ = ParseState::kHeaderValue;
      return true;
    }
    if (!isTokenChar(c) || name_len_ >= kMaxHeaderName) {
      fail(ParseError::kBadHeaderName);
      return false;
    }
    name_[name_len_++] = static_cast<char>(c);
    return true;
  }
  // The value.
  if (!value_started_) {
    if (isWhitespace(c)) return true;  // leading whitespace
    value_started_ = true;
  }
  if (++value_len_ > kMaxHeaderValue) {
    fail(ParseError::kTooLong);
    return false;
  }
  if (wanted_index_ >= 0) {
    interfaces::HttpWantedHeader& w = wanted_[static_cast<std::size_t>(wanted_index_)];
    if (w.value != nullptr && wanted_written_ + 1 < w.capacity) {
      w.value[wanted_written_++] = static_cast<char>(c);
    } else {
      w.truncated = true;
    }
  }
  if (own_ != Own::kNone) {
    if (own_len_ + 1 < sizeof own_value_) {
      own_value_[own_len_++] = static_cast<char>(c);
    } else {
      own_overflow_ = true;
    }
  }
  return true;
}

bool ResponseParser::chunkSizeByte(std::uint8_t c) {
  if (c == '\r') {
    if (saw_cr_) {
      fail(ParseError::kBadChunk);
      return false;
    }
    saw_cr_ = true;
    return true;
  }
  if (c == '\n') {
    saw_cr_ = false;
    if (chunk_digits_ == 0) {
      fail(ParseError::kBadChunk);
      return false;
    }
    if (chunk_size_ == 0) {
      state_ = ParseState::kTrailer;
      trailer_line_empty_ = true;
      return true;
    }
    body_left_ = chunk_size_;
    state_ = ParseState::kChunkData;
    return true;
  }
  if (saw_cr_) {
    fail(ParseError::kBadChunk);
    return false;
  }
  if (in_chunk_ext_) return true;  // `;name=value`, ignored (§4.1.1)
  if (c == ';') {
    if (chunk_digits_ == 0) {
      fail(ParseError::kBadChunk);
      return false;
    }
    in_chunk_ext_ = true;
    return true;
  }
  std::uint32_t digit;
  if (c >= '0' && c <= '9') {
    digit = static_cast<std::uint32_t>(c - '0');
  } else if (c >= 'a' && c <= 'f') {
    digit = static_cast<std::uint32_t>(c - 'a' + 10);
  } else if (c >= 'A' && c <= 'F') {
    digit = static_cast<std::uint32_t>(c - 'A' + 10);
  } else {
    fail(ParseError::kBadChunk);
    return false;
  }
  if (++chunk_digits_ > 8) {
    fail(ParseError::kBadChunk);
    return false;
  }
  chunk_size_ = (chunk_size_ << 4) | digit;
  return true;
}

bool ResponseParser::trailerByte(std::uint8_t c) {
  if (++header_bytes_ > kMaxHeaderBytes) {
    fail(ParseError::kTooLong);
    return false;
  }
  if (c == '\r') {
    if (saw_cr_) {
      fail(ParseError::kBadLineEnding);
      return false;
    }
    saw_cr_ = true;
    return true;
  }
  if (c == '\n') {
    saw_cr_ = false;
    if (trailer_line_empty_) {
      state_ = ParseState::kDone;
    }
    trailer_line_empty_ = true;
    return true;
  }
  if (saw_cr_) {
    fail(ParseError::kBadLineEnding);
    return false;
  }
  trailer_line_empty_ = false;
  return true;
}

std::size_t ResponseParser::feed(const std::uint8_t* data, std::size_t size,
                                 interfaces::HttpBodySink& sink) {
  if (data == nullptr) return 0;
  std::size_t at = 0;
  while (at < size) {
    switch (state_) {
      case ParseState::kStatusLine:
        if (!statusByte(data[at])) return at + 1;
        ++at;
        break;
      case ParseState::kHeaderStart:
      case ParseState::kHeaderName:
      case ParseState::kHeaderValue:
        if (!headerByte(data[at])) return at + 1;
        ++at;
        break;
      case ParseState::kBodyLength: {
        const std::size_t left = size - at;
        const std::size_t n = body_left_ < left ? static_cast<std::size_t>(body_left_) : left;
        if (!deliver(data + at, n, sink)) return at + n;
        at += n;
        body_left_ -= n;
        if (body_left_ == 0) state_ = ParseState::kDone;
        break;
      }
      case ParseState::kBodyUntilClose: {
        const std::size_t n = size - at;
        if (!deliver(data + at, n, sink)) return size;
        at += n;
        break;
      }
      case ParseState::kChunkSize:
        if (!chunkSizeByte(data[at])) return at + 1;
        ++at;
        break;
      case ParseState::kChunkData: {
        const std::size_t left = size - at;
        const std::size_t n = body_left_ < left ? static_cast<std::size_t>(body_left_) : left;
        if (!deliver(data + at, n, sink)) return at + n;
        at += n;
        body_left_ -= n;
        if (body_left_ == 0) {
          state_ = ParseState::kChunkDataEnd;
          saw_cr_ = false;
        }
        break;
      }
      case ParseState::kChunkDataEnd: {
        const std::uint8_t c = data[at++];
        if (c == '\r' && !saw_cr_) {
          saw_cr_ = true;
        } else if (c == '\n') {
          saw_cr_ = false;
          state_ = ParseState::kChunkSize;
          chunk_size_ = 0;
          chunk_digits_ = 0;
          in_chunk_ext_ = false;
        } else {
          fail(ParseError::kBadChunk);
          return at;
        }
        break;
      }
      case ParseState::kTrailer:
        if (!trailerByte(data[at])) return at + 1;
        ++at;
        break;
      case ParseState::kDone:
      case ParseState::kError:
        return at;
    }
  }
  return at;
}

bool ResponseParser::closed() {
  if (state_ == ParseState::kDone) return true;
  if (state_ == ParseState::kBodyUntilClose) {
    state_ = ParseState::kDone;
    return true;
  }
  if (state_ != ParseState::kError) state_ = ParseState::kError;
  return false;
}

}  // namespace net
}  // namespace core
