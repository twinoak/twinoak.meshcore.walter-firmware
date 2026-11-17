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
  // Into the record too, as the board does from the library's event task
  // (components/modem/walter_tls.cpp, noteClose): only for the id this connection is on, and only
  // once the request has gone out, which the record itself decides.
  if (record_ != nullptr && id == socket_id_) record_->wasClosed(clock_.nowMs());
  if (as_v0_1_0) {
    v010_closed_ = true;  // one flag, whatever the id named
    return;
  }
  rotation_.onClose(id);
}

void FakeTlsSocket::theServerClosed() {
  // The script may say when it is handed over instead (task p15-02): in the 8 November stall the
  // server answered and its close arrived while the receive was hanging, so it cannot be
  // delivered the moment the response was built.
  if (close_after_ms != 0) {
    close_scripted_ = true;
    return;
  }
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

// -- the scripted modem (task p15-02) ----------------------------------------------------------
// What the library's event task would bring in while a receive is running: the ring with the
// length the modem announced, and the close. Both are counted from the moment the request was
// whole on the wire, which is what the record counts from too.

void FakeTlsSocket::deliverScripted() {
  if (went_out_ms_ == 0) return;
  const std::uint64_t since = clock_.nowMs() - went_out_ms_;
  if (!ring_done_ && since >= ring_after_ms) {
    ring_done_ = true;
    rotation_.onRing(socket_id_);
    if (record_ != nullptr) record_->rang(clock_.nowMs(), ring_bytes);
  }
  if (!close_done_ && close_scripted_ && since >= close_after_ms) {
    close_done_ = true;
    deliverClose(socket_id_);
  }
}

void FakeTlsSocket::hang(std::uint32_t cost) {
  // In steps, so that the ring and the close land inside the hang and not after it. Once there is
  // nothing left to deliver the rest of it is one wait: a 30-second stall is 3 000 steps
  // otherwise, and the host suite pays for every one of them.
  constexpr std::uint32_t kStepMs = 10;
  std::uint32_t spent = 0;
  while (spent < cost) {
    std::uint32_t one = cost - spent;
    const bool nothing_left = ring_done_ && (close_done_ || !close_scripted_);
    if (!nothing_left && one > kStepMs) one = kStepMs;
    clock_.delayMs(one);
    spent += one;
    deliverScripted();
  }
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
  // Which socket this call is on, into the record (task p15-02), and the script for this
  // connection back to its beginning.
  if (record_ != nullptr) record_->onSocket(socket_id_);
  went_out_ms_ = 0;
  not_yet_left_ = not_yet_receives;
  receive_hung_ = false;
  ring_done_ = ring_bytes == 0;
  close_scripted_ = false;
  close_done_ = false;
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
  if (!answered_ && requestComplete()) {
    // The moment the request is whole on the wire, which is what the script's own times and the
    // record's `at_ms` are both counted from (task p15-02).
    went_out_ms_ = clock_.nowMs();
    answer();
  }
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
  // -- the scripted modem first (task p15-02) ------------------------------------------------
  // The asks the board's own receive loop makes and answers before a byte of the response is
  // handed over: the polls that say *nothing buffered yet*, and then the one command the modem
  // leaves unanswered. Nothing of this can be reached through this fake's loop - the loop is the
  // board's - so the fake says the same things to the same record (fake_tls_socket.h).
  deliverScripted();
  while (not_yet_left_ > 0) {
    --not_yet_left_;
    if (record_ != nullptr) record_->notYet();
    if (!spend(not_yet_ms, deadline_ms)) return interfaces::TlsRecv::kTimeout;
    deliverScripted();
  }
  if (receive_hangs_ms != 0 && !receive_hung_) {
    receive_hung_ = true;
    const std::uint64_t asked = clock_.nowMs();
    // Past the deadline if the script says so, which is what the library's blocking call does:
    // the wall is read before a receive and not during it (interfaces/tls_socket.h), so a 30 s
    // stall happens under a 15 s piece deadline and comes out as kError and not as a timeout.
    hang(receive_hangs_ms);
    if (record_ != nullptr) {
      record_->receiveFailed(clock_.nowMs(), static_cast<std::uint32_t>(clock_.nowMs() - asked),
                             receive_state, receive_cme);
    }
    // **The board's own answer from here, and not a habit of this fake's**: a receive that
    // failed with the close already handed over is the other end hanging up (kClosed), and one
    // with no close seen is this socket failing (kError). Until task p15-03 this returned kError
    // either way, which the board has never done; that task moved the decision into a rule and
    // task p15-05 took the rule's one more ask out again - the decision itself stays, here and
    // in components/modem/walter_tls.cpp, which is the point of this socket being the board's
    // peer. **No second command is sent**: whatever the script still holds of the response is
    // not handed over, exactly as the modem's bytes are gone when the command gives up.
    return closedNow() ? interfaces::TlsRecv::kClosed : interfaces::TlsRecv::kError;
  }
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
  // What this one command costs, so that the record can say when the receive was asked as well as
  // when it came back (task p15-07): the board reads its clock either side of `socketReceive`.
  const std::uint64_t asked_at = clock_.nowMs();
  if (!spend(piece_ms, deadline_ms)) return interfaces::TlsRecv::kTimeout;
  std::size_t n = cut_at_ - read_;
  if (n > max) n = max;
  if (n > piece_bytes) n = piece_bytes;
  std::memcpy(out, response_.data() + read_, n);
  read_ += n;
  bytes_out += n;
  *got = n;
  if (record_ != nullptr) {
    record_->gotData(clock_.nowMs(), n, static_cast<std::uint32_t>(clock_.nowMs() - asked_at));
  }
  return interfaces::TlsRecv::kData;
}

void FakeTlsSocket::close() {
  if (!open_) return;  // a refused open costs no connection and settles nothing
  ++closes;
  open_ = false;
  endConnection();
}

}  // namespace sim
