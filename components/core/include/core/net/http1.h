// HTTP/1.1 as bytes (task p3-02; RFC 7230 and RFC 7231): a writer for the one kind of request this
// firmware makes and a parser for the responses it gets back. Both are pure - bytes in, bytes or
// decisions out, no socket, no clock, no heap - so that every rule below has a host test and the
// parser can be run against ten thousand random inputs under the sanitizers
// (host_test/core/net/test_http1.cpp). core/net/http_over_tls.h drives them over a TLS socket.
//
// Why this exists at all: the modem's own HTTP client gives back a status, a content type and a
// length, and nothing else of a response (tasks/phase-3/reports/02.md); following a redirect,
// checking a range or noticing a changed file needs the headers, so the protocol is read here.
//
// -- the parser's rules, each a test ------------------------------------------------------------
//
//   * a status line `HTTP/1.0` or `HTTP/1.1`, a space, three digits, then a space and a reason
//     phrase (ignored) or the end of the line. Anything else is malformed. 1xx is refused: this
//     firmware never asks for `Expect: 100-continue` and reads nothing but a final status.
//   * a line ends in CR LF; a bare LF is accepted as well (RFC 7230 §3.5 allows a recipient to),
//     a bare CR is not.
//   * header names are tokens (RFC 7230 §3.2.6) of at most kMaxHeaderName bytes; a line that
//     begins with a space or a tab (an obsolete line fold, §3.2.4) is refused; a line without a
//     colon is refused. Names are compared case-insensitively.
//   * header values are streamed, never buffered: a value the caller asked for is copied into
//     the caller's buffer (and cut, with `truncated` set, when longer), any other is counted and
//     dropped. One value longer than kMaxHeaderValue, or a header section longer than
//     kMaxHeaderBytes, is "too long": a server that streams headers for ever is refused, not
//     waited for.
//   * `Content-Length` is decimal digits only, at most ten of them, inside 32 bits; two
//     Content-Length headers that disagree are refused (§3.3.2: a message with conflicting
//     lengths is invalid).
//   * `Transfer-Encoding: chunked` **is handled** - the one transfer coding this firmware reads,
//     because a dynamic page from a Node or Go server is usually chunked and refusing it would
//     refuse the server this unit exists to talk to. Any other coding, or chunked combined with
//     Content-Length, is refused (§3.3.3 item 3 allows either answer; refusing is the safe one).
//     Chunk extensions are skipped; trailers are skipped line by line; a chunk size over 32 bits
//     or longer than 8 hex digits is refused.
//   * a body without either framing header is read until the connection closes (§3.3.3 item 7);
//     a closed connection then completes it, and completes nothing else.
//   * 204, 304 and the answer to a HEAD request have no body whatever the headers say (§3.3.3
//     items 1 and 2); the caller says which request it made.
//
// What the parser does not do: decompress (this firmware sends no Accept-Encoding, so a
// conforming server sends none), handle HTTP/2, or keep a connection alive (the writer says
// `Connection: close`, and one request is one connection).
#pragma once

#include <cstddef>
#include <cstdint>

#include "interfaces/http.h"

namespace core {
namespace net {

// The bounds, each a sentence.

// A header field name. The longest in common use (`Strict-Transport-Security`,
// `Content-Security-Policy-Report-Only`) is under forty.
constexpr std::size_t kMaxHeaderName = 64;

// One header value. github.com sends a Content-Security-Policy of some 3 KB; a signed
// `Location` from a GitHub release download is about 1 KB. Nothing legitimate is longer than 8 KB.
constexpr std::size_t kMaxHeaderValue = 8192;

// The whole header section. github.com's response headers are ~5 KB; 32 KB is a server that is
// not answering a question but filling a buffer.
constexpr std::size_t kMaxHeaderBytes = 32768;

// The request head this firmware writes: a path of at most core/net/url.h's kMaxUrlLength (1536),
// a host of 127, five fixed headers, four of the caller's. A bearer token for the server (task
// p3-03) is a few hundred bytes. 2560 holds all of it with room; a head that does not fit is a
// refused request, never a cut one.
constexpr std::size_t kRequestHeadBytes = 2560;

// -- the writer --------------------------------------------------------------------------------

// The request head - request line, Host, User-Agent, Accept, Connection: close, the caller's
// headers, Content-Length for a body, the empty line - into `out`. Returns its length, or 0 when
// it does not fit or when a header name or value carries CR or LF (a header injection, refused).
// `user_agent` is what goes after `User-Agent: ` (core::banner() with a slash: the product and
// its version; nothing that identifies a unit).
std::size_t writeRequestHead(const interfaces::HttpRequest& request, const char* user_agent,
                             char* out, std::size_t capacity);

// -- the parser --------------------------------------------------------------------------------

enum class ParseState : std::uint8_t {
  kStatusLine = 0,
  kHeaderStart,    // at the start of a header line (or the empty line)
  kHeaderName,
  kHeaderValue,
  kBodyLength,     // a Content-Length body
  kBodyUntilClose,
  kChunkSize,
  kChunkData,
  kChunkDataEnd,   // the CR LF after a chunk's data
  kTrailer,        // the lines after the last chunk, up to the empty one
  kDone,
  kError,
};

enum class ParseError : std::uint8_t {
  kNone = 0,
  kBadStatusLine,       // not `HTTP/1.x NNN`
  kInformational,       // a 1xx status
  kBadLineEnding,       // a bare CR
  kObsoleteFold,        // a header line beginning with whitespace
  kBadHeaderName,       // not a token, or too long
  kNoColon,
  kBadContentLength,    // not digits, too many, or two that disagree
  kBadTransferEncoding, // a coding other than chunked, or chunked with Content-Length
  kBadChunk,            // a chunk size that is not hex, too long, or not followed by CR LF
  kTooLong,             // a value over kMaxHeaderValue or a header section over kMaxHeaderBytes
  kSinkStopped,         // the sink refused a piece
};

const char* describe(ParseError error);

class ResponseParser {
 public:
  ResponseParser() { begin(true, nullptr, 0); }

  // A new response. `expect_body` is false for a HEAD request. The wanted headers are the
  // caller's, filled in as they stream past; `present` and `truncated` are cleared here.
  void begin(bool expect_body, interfaces::HttpWantedHeader* wanted, std::size_t wanted_count);

  // The next bytes from the connection, in any chunking. Body bytes go to `sink` as they are
  // recognised; a sink that returns false ends the parse with kSinkStopped. Returns how many of
  // `size` were consumed: all of them until the response is done or in error, after which nothing
  // more is read (a server that sends more after `Connection: close` is ignored).
  std::size_t feed(const std::uint8_t* data, std::size_t size, interfaces::HttpBodySink& sink);

  // The other end closed the connection. True when that completes the response (a body read
  // until close, after the headers); false when the response was cut short. Moves the state to
  // kDone or kError accordingly.
  bool closed();

  ParseState state() const { return state_; }
  bool headersDone() const { return state_ >= ParseState::kBodyLength; }
  bool done() const { return state_ == ParseState::kDone; }
  bool failed() const { return state_ == ParseState::kError; }
  ParseError error() const { return error_; }

  std::uint16_t status() const { return status_; }
  bool hasContentLength() const { return has_content_length_; }
  std::uint32_t contentLength() const { return content_length_; }
  bool chunked() const { return chunked_; }
  bool untilClose() const { return until_close_; }
  std::uint64_t bodyBytes() const { return body_bytes_; }
  std::size_t headerBytes() const { return header_bytes_; }

 private:
  void fail(ParseError error);
  void endOfHeaders();
  void headerDone();
  bool deliver(const std::uint8_t* data, std::size_t size, interfaces::HttpBodySink& sink);
  // One byte of the status line or of a header; returns false when the parse failed.
  bool statusByte(std::uint8_t c);
  bool headerByte(std::uint8_t c);
  bool chunkSizeByte(std::uint8_t c);
  bool trailerByte(std::uint8_t c);

  ParseState state_ = ParseState::kStatusLine;
  ParseError error_ = ParseError::kNone;
  bool expect_body_ = true;
  interfaces::HttpWantedHeader* wanted_ = nullptr;
  std::size_t wanted_count_ = 0;

  // The status line, accumulated up to the status code.
  char line_[16] = {};
  std::size_t line_len_ = 0;
  bool in_reason_ = false;
  bool saw_cr_ = false;

  // The header being read.
  char name_[kMaxHeaderName + 1] = {};
  std::size_t name_len_ = 0;
  std::size_t value_len_ = 0;      // bytes of the value seen (not stored)
  bool value_started_ = false;     // past the leading whitespace
  int wanted_index_ = -1;          // which wanted header this is, or -1
  std::size_t wanted_written_ = 0; // bytes copied into it
  // The two headers the parser itself reads.
  enum class Own : std::uint8_t { kNone, kContentLength, kTransferEncoding };
  Own own_ = Own::kNone;
  char own_value_[32] = {};
  std::size_t own_len_ = 0;
  bool own_overflow_ = false;

  std::uint16_t status_ = 0;
  bool has_content_length_ = false;
  std::uint32_t content_length_ = 0;
  bool chunked_ = false;
  bool until_close_ = false;
  std::size_t header_bytes_ = 0;
  std::uint64_t body_bytes_ = 0;
  std::uint64_t body_left_ = 0;    // of a Content-Length body or of the current chunk

  // A chunk size line.
  std::uint64_t chunk_size_ = 0;
  std::size_t chunk_digits_ = 0;
  bool in_chunk_ext_ = false;
  bool trailer_line_empty_ = true;
};

}  // namespace net
}  // namespace core
