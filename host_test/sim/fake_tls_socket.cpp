#include "sim/fake_tls_socket.h"

#include <cstring>

namespace sim {

bool FakeTlsSocket::spend(std::uint32_t cost, std::uint64_t deadline) {
  const std::uint64_t now = clock_.nowMs();
  if (now >= deadline) return false;
  if (now + cost > deadline) {
    clock_.advance(deadline - now);
    return false;
  }
  clock_.advance(cost);
  return true;
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
    // dropped connection looks exactly the same from here.
    return interfaces::TlsRecv::kClosed;
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
  if (open_) ++closes;
  open_ = false;
}

}  // namespace sim
