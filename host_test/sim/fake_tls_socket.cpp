#include "sim/fake_tls_socket.h"

#include <cstring>

namespace sim {

// Through delayMs() and not advance(): a connect and a receive are waits, and a clock another
// clock is tied to (host_test/shim/support/FakePlatform.h, task p4-03: the agent library's
// platform clock moves with the link's) sees a wait and not a jump.
bool FakeTlsSocket::spend(std::uint32_t cost, std::uint64_t deadline) {
  const std::uint64_t now = clock_.nowMs();
  if (now >= deadline) return false;
  if (now + cost > deadline) {
    clock_.delayMs(static_cast<std::uint32_t>(deadline - now));
    return false;
  }
  if (cost != 0) clock_.delayMs(cost);
  return true;
}

// -- the socket layer (task p8-03) -------------------------------------------------------------
// The fake's half of core/net/socket_rotation.h: a connection is on an id, a close is an event on
// that id, and the event can be late. The refusals in open() happen before any of it, so a host
// that cannot be reached costs no connection and no close - which is also why every test written
// before this task behaves exactly as it did.

int FakeTlsSocket::beginConnection(std::uint64_t deadline_ms) {
  ++connections_;
  if (as_v0_1_0) {
    // v0.1.0: one socket, and the one close flag cleared before the dial.
    v010_closed_ = false;
    return kFirstSocketId;
  }
  return rotation_.begin(rotation_.nextId(), deadline_ms);
}

bool FakeTlsSocket::closedNow() const {
  return as_v0_1_0 ? v010_closed_ : rotation_.closedSeen();
}

void FakeTlsSocket::deliverClose(int id) {
  if (as_v0_1_0) {
    v010_closed_ = true;  // one flag, whatever the id named
    return;
  }
  rotation_.onClose(id);
}

void FakeTlsSocket::theServerClosed() {
  // A drop in the middle of a body is the close arriving while the body is being read, not the
  // race between a Content-Length and a FIN: it is never held back, and never lost.
  if (close_event_lost && !dropped_) return;
  const int late = dropped_ ? 0 : close_event_lateness;
  if (late <= 0) {
    deliverClose(socket_id_);
    return;
  }
  if (kept_count_ >= kKeptCloses) return;
  kept_[kept_count_].id = socket_id_;
  kept_[kept_count_].due = connections_ + static_cast<std::uint32_t>(late);
  ++kept_count_;
}

void FakeTlsSocket::deliverDueCloses() {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < kept_count_; ++i) {
    if (kept_[i].due <= connections_) {
      deliverClose(kept_[i].id);
      ++late_closes;
    } else {
      kept_[kept++] = kept_[i];
    }
  }
  kept_count_ = kept;
}

void FakeTlsSocket::endConnection() {
  if (as_v0_1_0) return;
  // The scripted server closes *after* it answers. A connection it never answered - a handshake
  // on its own (core/update/tls_check.h), a stall, a request that went nowhere - has no close on
  // its way, so nothing is owed. The board tells the two apart by whether receive() ever
  // returned a byte and calls end() or cancel() the same way
  // (components/modem/walter_tls.cpp, close()); what a debt that is never paid costs, and how
  // long it lives, is in socket_rotation.h.
  if (!answered_) {
    rotation_.cancel();
    return;
  }
  rotation_.end();
}

interfaces::TlsOpen FakeTlsSocket::open(const char* host, std::uint16_t, std::uint8_t root,
                                        std::uint64_t deadline_ms) {
  ++opens;
  last_root = root;
  last_host = host != nullptr ? host : "";
  if (open_) return interfaces::TlsOpen::kBusy;
  if (no_uplink) return interfaces::TlsOpen::kNoUplink;
  if (root >= core::net::tlsRootCount()) return interfaces::TlsOpen::kNoSuchRoot;
  if (!spend(connect_ms, deadline_ms)) return interfaces::TlsOpen::kTimeout;
  std::uint8_t chain_root = 0;
  if (!server_.rootOf(last_host, &chain_root)) {
    ++connect_failures;
    return dial_opaque ? interfaces::TlsOpen::kDialRefused : interfaces::TlsOpen::kConnectFailed;
  }
  // The rail: a chain that does not end at the root asked for is a failed handshake, and so is
  // a server scripted to break every handshake.
  if (refuse_tls || chain_root != root) {
    ++tls_failures;
    return dial_opaque ? interfaces::TlsOpen::kDialRefused : interfaces::TlsOpen::kTlsFailed;
  }
  socket_id_ = beginConnection(deadline_ms);
  socket_ids.push_back(socket_id_);
  // And now the events the library had not got round to: *after* the flags were cleared, which
  // is the board's race and the harshest moment for them to land.
  deliverDueCloses();
  open_ = true;
  request_.clear();
  response_.clear();
  read_ = 0;
  cut_at_ = 0;
  answered_ = false;
  dropped_ = false;
  return interfaces::TlsOpen::kOpen;
}

bool FakeTlsSocket::send(const std::uint8_t* data, std::size_t size, std::uint64_t deadline_ms) {
  if (!open_ || data == nullptr) return false;
  // The board, when a late close had set the flag: `send` stops and the request never goes out.
  if (closedNow()) return false;
  if (!spend(piece_ms, deadline_ms)) return false;
  request_.append(reinterpret_cast<const char*>(data), size);
  if (!answered_ && requestComplete()) answer();
  return true;
}

// The head is in and, for a body, Content-Length bytes after it.
bool FakeTlsSocket::requestComplete() const {
  const std::size_t end = request_.find("\r\n\r\n");
  if (end == std::string::npos) return false;
  const std::string head = request_.substr(0, end);
  std::size_t length = 0;
  std::size_t at = head.find("\r\nContent-Length:");
  if (at == std::string::npos) at = head.find("\r\ncontent-length:");
  if (at != std::string::npos) {
    at = head.find(':', at) + 1;
    while (at < head.size() && head[at] == ' ') ++at;
    while (at < head.size() && head[at] >= '0' && head[at] <= '9') {
      length = length * 10 + static_cast<std::size_t>(head[at] - '0');
      ++at;
    }
  }
  return request_.size() >= end + 4 + length;
}

void FakeTlsSocket::answer() {
  answered_ = true;
  const int number = requests++;
  response_ = server_.respond(request_);
  cut_at_ = response_.size();
  if (drop_after_response_bytes >= 0 && static_cast<std::size_t>(drop_after_response_bytes) < cut_at_) {
    cut_at_ = static_cast<std::size_t>(drop_after_response_bytes);
    dropped_ = true;
  }
  if (number == drop_at_request) {
    // After N bytes of the body: the body begins after the head's empty line.
    const std::string text(response_.begin(), response_.end());
    const std::size_t body_at = text.find("\r\n\r\n");
    if (body_at != std::string::npos) {
      const std::size_t cut = body_at + 4 + static_cast<std::size_t>(drop_after_body_bytes);
      if (cut < cut_at_) {
        cut_at_ = cut;
        dropped_ = true;
      }
    }
  }
  // The scripted server answers and closes (`Connection: close`), so its close is on its way the
  // moment the response is: delivered now, or at a later connection (close_event_lateness).
  theServerClosed();
}

interfaces::TlsRecv FakeTlsSocket::receive(std::uint8_t* out, std::size_t max, std::size_t* got,
                                           std::uint64_t deadline_ms) {
  if (got != nullptr) *got = 0;
  if (!open_ || out == nullptr || got == nullptr) return interfaces::TlsRecv::kError;
  if (stall || server_.no_response || !answered_) {
    // Nothing will come: the deadline.
    spend(0xFFFFFFFFu, deadline_ms);
    return interfaces::TlsRecv::kTimeout;
  }
  if (read_ >= cut_at_) {
    // Everything there was: the server closes after its response (Connection: close), and a
    // dropped connection looks exactly the same from here - *once the close has been delivered*.
    if (closedNow()) return interfaces::TlsRecv::kClosed;
    // It has not been (close_event_lateness). To the modem this is a socket with nothing
    // buffered and no close seen, and the board's receive loop waits for one or the other. A
    // request that ends on its Content-Length never gets here, which is why the first request of
    // every wake worked and the second did not.
    spend(0xFFFFFFFFu, deadline_ms);
    return interfaces::TlsRecv::kTimeout;
  }
  if (!spend(piece_ms, deadline_ms)) return interfaces::TlsRecv::kTimeout;
  std::size_t n = cut_at_ - read_;
  if (n > max) n = max;
  if (n > piece_bytes) n = piece_bytes;
  std::memcpy(out, response_.data() + read_, n);
  read_ += n;
  bytes_out += n;
  *got = n;
  return interfaces::TlsRecv::kData;
}

void FakeTlsSocket::close() {
  if (!open_) return;  // a refused open costs no connection and settles nothing
  ++closes;
  open_ = false;
  endConnection();
}

}  // namespace sim
