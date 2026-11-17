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
  // What the modem said in this one, from the beginning (task p15-02): the record belongs to the
  // request now starting whatever becomes of it, so that a call that never got its bytes out is
  // still the record a person reads beside its entry in `agent calls`.
  if (record_ != nullptr) record_->begin();
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
    response->connection_close = parser_.connectionClose();
    response->body_bytes = parser_.bodyBytes();
    response->elapsed_ms = static_cast<std::uint32_t>(clock_.nowMs() - started);
    // Before the close this end asks for: `+SQNSH` for our own AT+SQNSH is not the other end
    // hanging up, and the record is the receiving side of the call and not of the close.
    //
    // What the answer said of itself goes in first (task p15-07): the status, the framing, the
    // bytes and the server's `Connection: close`, which are what a call that *worked* is read by
    // when it is laid beside one that did not. A call nothing answered has a status of 0 and the
    // record says nothing about an answer.
    if (record_ != nullptr) {
      record_->answerWas(*response);
      record_->ended(outcome);
    }
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

  // The root this request is verified against, once per boot in practice (core/net/trust.h,
  // ensureRoot: another host's root that will not load is no reason to refuse this one).
  if (trust_ != nullptr) {
    const TrustOutcome t = trust_->ensureRoot(request.root, deadline);
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
  // The request is whole on the wire. Everything the modem says from here is counted from this
  // moment, which is the moment the 8 November stall begins (core/net/call_record.h).
  if (record_ != nullptr) {
    record_->wentOut(clock_.nowMs(),
                     head_len + (request.method == interfaces::HttpMethod::kPost ? request.body_size : 0));
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
        // Not kClosed: the other end did not hang up, the socket did (interfaces/http.h,
        // kSocketFailed). On the board this is the modem answering an error to a receive, or not
        // answering it at all inside the library's 30 s command timeout.
        finish(interfaces::HttpOutcome::kSocketFailed);
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
