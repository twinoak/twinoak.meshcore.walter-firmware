// HTTP/1.1 as bytes (task p3-02): the request head the writer produces, byte for byte; the
// response parser against split headers, folded and oversized headers, chunked bodies, bodies
// with no length, the bare-LF tolerance, the Content-Length rules - and ten thousand random
// inputs without a fault under the sanitizers.
#include <doctest.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "core/net/http1.h"

using core::net::ParseError;
using core::net::ParseState;
using core::net::ResponseParser;
using core::net::writeRequestHead;
using interfaces::HttpBodySink;
using interfaces::HttpHeader;
using interfaces::HttpMethod;
using interfaces::HttpRequest;
using interfaces::HttpWantedHeader;

namespace {

class VectorSink final : public HttpBodySink {
 public:
  std::vector<std::uint8_t> bytes;
  std::size_t stop_after = static_cast<std::size_t>(-1);
  bool piece(const std::uint8_t* data, std::size_t size) override {
    bytes.insert(bytes.end(), data, data + size);
    return bytes.size() <= stop_after;
  }
  std::string text() const { return std::string(bytes.begin(), bytes.end()); }
};

// Feeds `text` in pieces of `piece` bytes and returns the parser's final state.
ParseState feedIn(ResponseParser& p, const std::string& text, std::size_t piece, HttpBodySink& sink) {
  for (std::size_t at = 0; at < text.size() && !p.done() && !p.failed();) {
    const std::size_t n = text.size() - at < piece ? text.size() - at : piece;
    const std::size_t used = p.feed(reinterpret_cast<const std::uint8_t*>(text.data()) + at, n, sink);
    at += used;
    if (used == 0) break;
  }
  return p.state();
}

}  // namespace

TEST_CASE("the request head: method, path, Host, User-Agent, Accept, Connection: close, the caller's headers, Content-Length") {
  HttpRequest r;
  r.method = HttpMethod::kGet;
  r.host = "meshmaster.dk";
  r.path = "/api/thing?x=1";
  r.headers[0] = HttpHeader{"Range", "bytes=0-65535"};
  r.headers[1] = HttpHeader{"If-Range", "\"etag\""};
  r.header_count = 2;
  char out[2560];
  const std::size_t n = writeRequestHead(r, "twinoak-walter 0.0.0-dev", out, sizeof out);
  REQUIRE(n > 0);
  CHECK(std::string(out, n) ==
        "GET /api/thing?x=1 HTTP/1.1\r\n"
        "Host: meshmaster.dk\r\n"
        "User-Agent: twinoak-walter 0.0.0-dev\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "Range: bytes=0-65535\r\n"
        "If-Range: \"etag\"\r\n"
        "\r\n");

  r.method = HttpMethod::kPost;
  r.port = 8443;
  r.header_count = 1;
  r.headers[0] = HttpHeader{"Content-Type", "application/json"};
  r.body_size = 17;
  const std::size_t m = writeRequestHead(r, "ua", out, sizeof out);
  REQUIRE(m > 0);
  const std::string head(out, m);
  CHECK(head.rfind("POST /api/thing?x=1 HTTP/1.1\r\nHost: meshmaster.dk:8443\r\n", 0) == 0);
  CHECK(head.find("Content-Type: application/json\r\n") != std::string::npos);
  CHECK(head.find("Content-Length: 17\r\n\r\n") == head.size() - 22);
}

TEST_CASE("the request head refuses what cannot go on the wire: no host, a bad path, a header with CR LF, too long") {
  char out[2560];
  HttpRequest r;
  r.host = "";
  r.path = "/";
  CHECK(writeRequestHead(r, "ua", out, sizeof out) == 0);
  r.host = "h.example";
  r.path = "no-leading-slash";
  CHECK(writeRequestHead(r, "ua", out, sizeof out) == 0);
  r.path = "/with space";
  CHECK(writeRequestHead(r, "ua", out, sizeof out) == 0);
  r.path = "/";
  r.headers[0] = HttpHeader{"X-Bad", "value\r\nInjected: yes"};
  r.header_count = 1;
  CHECK(writeRequestHead(r, "ua", out, sizeof out) == 0);
  r.headers[0] = HttpHeader{"Bad Name", "v"};
  CHECK(writeRequestHead(r, "ua", out, sizeof out) == 0);
  r.header_count = 0;
  char small[40];
  CHECK(writeRequestHead(r, "ua", small, sizeof small) == 0);  // does not fit: refused, not cut
  std::string path = "/";
  path += std::string(3000, 'p');
  r.path = path.c_str();
  CHECK(writeRequestHead(r, "ua", out, sizeof out) == 0);
}

TEST_CASE("a plain response with Content-Length, in every chunking from one byte up") {
  const std::string text =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\nETag: \"abc\"  \r\n\r\nhello world";
  for (std::size_t piece : {1u, 2u, 3u, 7u, 16u, 100u, 1500u}) {
    ResponseParser p;
    char etag[32] = {};
    HttpWantedHeader wanted[1] = {};
    wanted[0].name = "etag";  // case-insensitive
    wanted[0].value = etag;
    wanted[0].capacity = sizeof etag;
    p.begin(true, wanted, 1);
    VectorSink sink;
    CHECK(feedIn(p, text, piece, sink) == ParseState::kDone);
    CHECK(p.status() == 200);
    CHECK(p.hasContentLength());
    CHECK(p.contentLength() == 11);
    CHECK(sink.text() == "hello world");
    CHECK(wanted[0].present);
    CHECK(std::string(etag) == "\"abc\"");  // trailing whitespace trimmed
    CHECK_FALSE(wanted[0].truncated);
  }
}

TEST_CASE("the status line: HTTP/1.0 too, a reason phrase or none, and what is not a status line") {
  VectorSink sink;
  {
    ResponseParser p;
    CHECK(feedIn(p, "HTTP/1.0 204\r\n\r\n", 5, sink) == ParseState::kDone);
    CHECK(p.status() == 204);
  }
  for (const char* bad : {"HTTP/2 200 OK\r\n\r\n", "HTTP/1.1 20 OK\r\n\r\n", "HTTP/1.1 2000 OK\r\n\r\n",
                          "HTTP/1.1  200 OK\r\n\r\n", "HTP/1.1 200 OK\r\n\r\n", "200 OK\r\n\r\n",
                          "HTTP/1.1 abc OK\r\n\r\n", "HTTP/1.1 099 OK\r\n\r\n", "\r\n"}) {
    ResponseParser p;
    CHECK_MESSAGE(feedIn(p, bad, 1, sink) == ParseState::kError, bad);
    CHECK(p.error() == ParseError::kBadStatusLine);
  }
  ResponseParser p;
  CHECK(feedIn(p, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n\r\n", 1, sink) == ParseState::kError);
  CHECK(p.error() == ParseError::kInformational);
}

TEST_CASE("a bare LF ends a line; a bare CR does not") {
  VectorSink sink;
  ResponseParser p;
  CHECK(feedIn(p, "HTTP/1.1 200 OK\nContent-Length: 2\n\nok", 1, sink) == ParseState::kDone);
  CHECK(sink.text() == "ok");
  ResponseParser q;
  CHECK(feedIn(q, "HTTP/1.1 200 OK\rContent-Length: 2\r\n\r\nok", 1, sink) == ParseState::kError);
  CHECK(q.error() == ParseError::kBadLineEnding);
}

TEST_CASE("headers: folded lines refused, a line without a colon refused, a bad name refused, a long name refused") {
  VectorSink sink;
  {
    ResponseParser p;
    CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nX-A: one\r\n two\r\nContent-Length: 0\r\n\r\n", 3, sink) == ParseState::kError);
    CHECK(p.error() == ParseError::kObsoleteFold);
  }
  {
    ResponseParser p;
    CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nnocolon\r\n\r\n", 3, sink) == ParseState::kError);
    CHECK(p.error() == ParseError::kNoColon);
  }
  {
    ResponseParser p;
    CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n", 3, sink) == ParseState::kError);
    CHECK(p.error() == ParseError::kBadHeaderName);
  }
  {
    ResponseParser p;
    const std::string name(core::net::kMaxHeaderName + 1, 'N');
    CHECK(feedIn(p, "HTTP/1.1 200 OK\r\n" + name + ": x\r\n\r\n", 3, sink) == ParseState::kError);
    CHECK(p.error() == ParseError::kBadHeaderName);
  }
}

TEST_CASE("oversized headers: a wanted value is cut and says so, a value past the bound is refused, a header section past the bound is refused") {
  VectorSink sink;
  {
    ResponseParser p;
    char small[8] = {};
    HttpWantedHeader wanted[1] = {};
    wanted[0].name = "Location";
    wanted[0].value = small;
    wanted[0].capacity = sizeof small;
    p.begin(true, wanted, 1);
    CHECK(feedIn(p, "HTTP/1.1 302 Found\r\nLocation: https://very.long.example/path\r\nContent-Length: 0\r\n\r\n", 5, sink) == ParseState::kDone);
    CHECK(wanted[0].present);
    CHECK(wanted[0].truncated);
    CHECK(std::string(small) == "https:/");
  }
  {
    // A 3 KB Content-Security-Policy (what github.com sends) is fine; it is not wanted and is dropped.
    ResponseParser p;
    const std::string csp(3000, 'c');
    CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nContent-Security-Policy: " + csp + "\r\nContent-Length: 0\r\n\r\n", 64, sink) == ParseState::kDone);
    CHECK(p.headerBytes() > 3000);
  }
  {
    ResponseParser p;
    const std::string huge(core::net::kMaxHeaderValue + 1, 'v');
    CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nX-Huge: " + huge + "\r\n\r\n", 1024, sink) == ParseState::kError);
    CHECK(p.error() == ParseError::kTooLong);
  }
  {
    ResponseParser p;
    std::string many = "HTTP/1.1 200 OK\r\n";
    for (int i = 0; i < 600; ++i) many += "X-" + std::to_string(i) + ": " + std::string(60, 'x') + "\r\n";
    CHECK(feedIn(p, many + "\r\n", 1024, sink) == ParseState::kError);
    CHECK(p.error() == ParseError::kTooLong);
  }
}

TEST_CASE("Content-Length: digits only, inside 32 bits, two that agree are one, two that disagree are refused") {
  VectorSink sink;
  for (const char* bad : {"Content-Length: 1x\r\n", "Content-Length: -1\r\n", "Content-Length: 99999999999\r\n",
                          "Content-Length: 4294967296\r\n", "Content-Length:\r\n",
                          "Content-Length: 5\r\nContent-Length: 6\r\n"}) {
    ResponseParser p;
    CHECK_MESSAGE(feedIn(p, std::string("HTTP/1.1 200 OK\r\n") + bad + "\r\nhello", 4, sink) == ParseState::kError, bad);
    CHECK(p.error() == ParseError::kBadContentLength);
  }
  ResponseParser p;
  CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello", 4, sink) == ParseState::kDone);
  CHECK(sink.text() == "hello");
}

TEST_CASE("chunked bodies are handled: sizes in hex, extensions skipped, trailers skipped, every chunking") {
  const std::string text =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n"
      "5;ext=1\r\nhello\r\n"
      "6\r\n world\r\n"
      "A\r\n0123456789\r\n"
      "0\r\n"
      "X-Trailer: yes\r\n"
      "\r\n";
  for (std::size_t piece : {1u, 2u, 5u, 13u, 1500u}) {
    ResponseParser p;
    VectorSink sink;
    CHECK(feedIn(p, text, piece, sink) == ParseState::kDone);
    CHECK(p.chunked());
    CHECK(sink.text() == "hello world0123456789");
    CHECK(p.bodyBytes() == 21);
  }
  VectorSink sink;
  for (const char* bad : {"\r\nhello\r\n0\r\n\r\n",       // no size
                          "zz\r\nhello\r\n0\r\n\r\n",     // not hex
                          "5\r\nhelloX\r\n0\r\n\r\n",     // the data not followed by CR LF
                          "123456789\r\n", ";ext\r\n"}) {  // too long; an extension with no size
    ResponseParser p;
    CHECK_MESSAGE(feedIn(p, std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") + bad, 3, sink) == ParseState::kError, bad);
    CHECK(p.error() == ParseError::kBadChunk);
  }
  {
    ResponseParser p;
    CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", 3, sink) == ParseState::kError);
    CHECK(p.error() == ParseError::kBadTransferEncoding);
  }
  {
    ResponseParser p;
    CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n", 3, sink) == ParseState::kError);
    CHECK(p.error() == ParseError::kBadTransferEncoding);
  }
}

TEST_CASE("a body with no length is read until the connection closes, and only that is completed by a close") {
  VectorSink sink;
  ResponseParser p;
  CHECK(feedIn(p, "HTTP/1.1 200 OK\r\n\r\nsome bytes", 4, sink) == ParseState::kBodyUntilClose);
  CHECK(p.untilClose());
  CHECK(p.closed());
  CHECK(p.done());
  CHECK(sink.text() == "some bytes");

  ResponseParser q;
  CHECK(feedIn(q, "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort", 4, sink) == ParseState::kBodyLength);
  CHECK_FALSE(q.closed());
  CHECK(q.failed());

  ResponseParser r;
  CHECK(feedIn(r, "HTTP/1.1 200 OK\r\nContent-Len", 4, sink) == ParseState::kHeaderName);
  CHECK_FALSE(r.closed());
}

TEST_CASE("204, 304 and a HEAD response have no body whatever the headers say") {
  VectorSink sink;
  for (const char* status : {"204 No Content", "304 Not Modified"}) {
    ResponseParser p;
    CHECK(feedIn(p, std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 5\r\n\r\nextra", 3, sink) == ParseState::kDone);
    CHECK(p.bodyBytes() == 0);
  }
  ResponseParser p;
  p.begin(false, nullptr, 0);
  CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", 3, sink) == ParseState::kDone);
  CHECK(sink.text().empty());
}

TEST_CASE("the sink can stop the body, and the parser says so") {
  VectorSink sink;
  sink.stop_after = 3;
  ResponseParser p;
  CHECK(feedIn(p, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789", 2, sink) == ParseState::kError);
  CHECK(p.error() == ParseError::kSinkStopped);
  // The piece that passed the bound was handed over whole (two bytes at a time here, the first
  // body piece sharing its two with the end of the headers), and nothing after it.
  CHECK(sink.bytes.size() > 3);
  CHECK(sink.bytes.size() <= 5);
}

TEST_CASE("after done or error nothing more is consumed") {
  VectorSink sink;
  ResponseParser p;
  const std::string text = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokTRAILING GARBAGE";
  const std::size_t used = p.feed(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), sink);
  CHECK(p.done());
  CHECK(used == text.size() - std::strlen("TRAILING GARBAGE"));
  CHECK(p.feed(reinterpret_cast<const std::uint8_t*>("x"), 1, sink) == 0);
}

// Ten thousand inputs: random bytes, and valid responses with random mutations, fed in random
// pieces. The parser must end in done or error (or be waiting for more) without a fault - the
// sanitizers are what judges that - and must never report a body longer than it was given.
TEST_CASE("ten thousand random inputs without a fault, and the parser always ends") {
  std::mt19937 rng(20261014u);
  std::uniform_int_distribution<int> kind(0, 3);
  std::uniform_int_distribution<int> length(0, 400);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<int> piece(1, 64);
  const std::vector<std::string> seeds = {
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\nETag: \"abc\"\r\n\r\nhello world",
      "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-4/10\r\nContent-Length: 5\r\n\r\nhello",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n",
      "HTTP/1.1 302 Found\r\nLocation: https://elsewhere.example/x\r\nContent-Length: 0\r\n\r\n",
      "HTTP/1.1 200 OK\r\n\r\nuntil close",
  };
  int done = 0, errors = 0, waiting = 0;
  for (int i = 0; i < 10000; ++i) {
    std::string input;
    const int k = kind(rng);
    if (k == 0) {
      const int n = length(rng);
      for (int j = 0; j < n; ++j) input.push_back(static_cast<char>(byte(rng)));
    } else {
      input = seeds[static_cast<std::size_t>(i) % seeds.size()];
      const int mutations = k == 1 ? 1 : (k == 2 ? 5 : 20);
      for (int m = 0; m < mutations && !input.empty(); ++m) {
        std::uniform_int_distribution<std::size_t> at(0, input.size() - 1);
        const int what = byte(rng) % 3;
        if (what == 0) input[at(rng)] = static_cast<char>(byte(rng));
        if (what == 1) input.erase(at(rng), 1);
        if (what == 2) input.insert(at(rng), 1, static_cast<char>(byte(rng)));
      }
    }
    ResponseParser p;
    char small[16] = {};
    HttpWantedHeader wanted[2] = {};
    wanted[0].name = "ETag";
    wanted[0].value = small;
    wanted[0].capacity = sizeof small;
    wanted[1].name = "Location";
    wanted[1].value = nullptr;  // wanted with no buffer: must be harmless
    wanted[1].capacity = 0;
    p.begin(true, wanted, 2);
    VectorSink sink;
    std::uint64_t fed = 0;
    for (std::size_t at = 0; at < input.size() && !p.done() && !p.failed();) {
      const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(piece(rng)), input.size() - at);
      const std::size_t used = p.feed(reinterpret_cast<const std::uint8_t*>(input.data()) + at, n, sink);
      REQUIRE(used <= n);
      fed += used;
      at += used;
      if (used == 0) break;
    }
    REQUIRE(p.bodyBytes() <= fed);
    REQUIRE(sink.bytes.size() == p.bodyBytes());
    if (p.done()) {
      ++done;
    } else if (p.failed()) {
      ++errors;
    } else {
      ++waiting;
      p.closed();  // a close must also leave it done or failed
      REQUIRE((p.done() || p.failed()));
    }
  }
  MESSAGE("random inputs: " << done << " parsed to the end, " << errors << " refused, " << waiting << " still waiting at the end of the input");
  CHECK(done > 0);
  CHECK(errors > 0);
}
