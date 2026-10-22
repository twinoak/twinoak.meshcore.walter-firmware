#include "core/net/http_over_tls.h"

#include <cstring>

#include "core/version.h"

namespace core {
namespace net {

namespace {

std::uint64_t sooner(std::uint64_t a, std::uint64_t b) { return a < b ? a : b; }

}  // namespace

HttpOverTls::HttpOverTls(interfaces::TlsSocket& socket, TrustLoader* trust, interfaces::Clock& clock)
    : socket_(socket), trust_(trust), clock_(clock) {}

void HttpOverTls::wipeHead() {
  volatile char* p = head_;
  for (std::size_t i = 0; i < sizeof head_; ++i) p[i] = 0;
}

void HttpOverTls::request(const interfaces::HttpRequest& request, interfaces::HttpWantedHeader* wanted,
                          std::size_t wanted_count, interfaces::HttpResponse* response,
                          interfaces::HttpBodySink& sink) {
  if (response == nullptr) return;
  *response = interfaces::HttpResponse{};
  ++requests_;
  const std::uint64_t started = clock_.nowMs();
  const std::uint32_t total_ms = request.total_ms != 0 ? request.total_ms : kDefaultTotalMs;
  const std::uint32_t piece_ms = request.piece_ms != 0 ? request.piece_ms : kDefaultPieceMs;
  const std::uint64_t deadline = started + total_ms;

  auto finish = [&](interfaces::HttpOutcome outcome) {
    // Every way out: a sensitive head is gone whether it was sent, refused or half-written.
    if (request.sensitive) wipeHead();
    response->outcome = outcome;
    response->status = parser_.status();
    response->has_content_length = parser_.hasContentLength();
    response->content_length = parser_.contentLength();
    response->chunked = parser_.chunked();
    response->body_bytes = parser_.bodyBytes();
    response->elapsed_ms = static_cast<std::uint32_t>(clock_.nowMs() - started);
    socket_.close();
  };

  // The head first, so that a request that cannot be written costs no connection.
  parser_.begin(true, wanted, wanted_count);
  const std::size_t head_len = writeRequestHead(request, core::banner(), head_, sizeof head_);
  if (head_len == 0) {
    finish(interfaces::HttpOutcome::kBadRequest);
    return;
  }
  if (request.root >= tlsRootCount()) {
    finish(interfaces::HttpOutcome::kTlsFailed);
    return;
  }

  // The roots, once per boot in practice (core/net/trust.h).
  if (trust_ != nullptr) {
    const TrustOutcome t = trust_->ensure(deadline);
    if (t == TrustOutcome::kFailed || t == TrustOutcome::kNotBegun) {
      finish(interfaces::HttpOutcome::kTrustFailed);
      return;
    }
  }

  ++connects_;
  const interfaces::TlsOpen opened = socket_.open(request.host, request.port, request.root, deadline);
  switch (opened) {
    case interfaces::TlsOpen::kOpen:
      break;
    case interfaces::TlsOpen::kNoUplink:
      finish(interfaces::HttpOutcome::kNoUplink);
      return;
    case interfaces::TlsOpen::kTlsFailed:
    case interfaces::TlsOpen::kNoSuchRoot:
      finish(interfaces::HttpOutcome::kTlsFailed);
      return;
    case interfaces::TlsOpen::kTimeout:
      finish(interfaces::HttpOutcome::kTimeout);
      return;
    case interfaces::TlsOpen::kBusy:
    case interfaces::TlsOpen::kConnectFailed:
      finish(interfaces::HttpOutcome::kConnectFailed);
      return;
    case interfaces::TlsOpen::kDialRefused:
      finish(interfaces::HttpOutcome::kDialRefused);
      return;
  }

  const bool sent = socket_.send(reinterpret_cast<const std::uint8_t*>(head_), head_len, deadline);
  // The head is on the wire (or refused): a credential in it is wiped now, before the body goes
  // and before the seconds a response takes over LTE-M, and not only at the end.
  if (request.sensitive) wipeHead();
  if (!sent) {
    finish(interfaces::HttpOutcome::kSendFailed);
    return;
  }
  if (request.method == interfaces::HttpMethod::kPost && request.body_size > 0) {
    if (request.body == nullptr || !socket_.send(request.body, request.body_size, deadline)) {
      finish(interfaces::HttpOutcome::kSendFailed);
      return;
    }
  }

  // The response, piece by piece. The status and the framing headers are written into the
  // response **before the first body piece** reaches the sink (interfaces/http.h's promise: a sink
  // may decide from the status), through this relay; finish() writes them again at the end.
  class Early final : public interfaces::HttpBodySink {
   public:
    Early(const ResponseParser& parser, interfaces::HttpResponse* response, interfaces::HttpBodySink& inner)
        : parser_(parser), response_(response), inner_(inner) {}
    bool piece(const std::uint8_t* data, std::size_t size) override {
      if (!filled_) {
        filled_ = true;
        response_->status = parser_.status();
        response_->has_content_length = parser_.hasContentLength();
        response_->content_length = parser_.contentLength();
        response_->chunked = parser_.chunked();
      }
      return inner_.piece(data, size);
    }

   private:
    const ResponseParser& parser_;
    interfaces::HttpResponse* response_;
    interfaces::HttpBodySink& inner_;
    bool filled_ = false;
  } early(parser_, response, sink);
  while (!parser_.done() && !parser_.failed()) {
    const std::uint64_t now = clock_.nowMs();
    if (now >= deadline) {
      finish(interfaces::HttpOutcome::kTimeout);
      return;
    }
    std::size_t got = 0;
    const interfaces::TlsRecv r = socket_.receive(rx_, sizeof rx_, &got, sooner(deadline, now + piece_ms));
    switch (r) {
      case interfaces::TlsRecv::kData:
        parser_.feed(rx_, got, early);
        break;
      case interfaces::TlsRecv::kClosed:
        if (parser_.closed()) {
          finish(interfaces::HttpOutcome::kOk);
        } else {
          finish(interfaces::HttpOutcome::kClosed);
        }
        return;
      case interfaces::TlsRecv::kTimeout:
        finish(interfaces::HttpOutcome::kTimeout);
        return;
      case interfaces::TlsRecv::kError:
        finish(interfaces::HttpOutcome::kClosed);
        return;
    }
  }
  if (parser_.failed()) {
    switch (parser_.error()) {
      case ParseError::kSinkStopped: finish(interfaces::HttpOutcome::kSinkStopped); return;
      case ParseError::kTooLong: finish(interfaces::HttpOutcome::kTooLong); return;
      default: finish(interfaces::HttpOutcome::kMalformed); return;
    }
  }
  finish(interfaces::HttpOutcome::kOk);
}

}  // namespace net
}  // namespace core
