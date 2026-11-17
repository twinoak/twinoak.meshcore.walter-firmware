#include "core/net/call_record.h"

#include <cstdio>
#include <cstring>

namespace core {
namespace net {

namespace {

// Past this, offered_ is left alone: the count of the ones there was no room for is capped at
// fifteen anyway, so there is nothing above it to learn and nothing to wrap.
constexpr std::uint16_t kOfferedCeiling = 0xFFF0u;

void put16(std::uint8_t* at, std::uint16_t v) {
  at[0] = static_cast<std::uint8_t>(v & 0xFFu);
  at[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

std::uint16_t get16(const std::uint8_t* at) {
  return static_cast<std::uint16_t>(at[0] | (static_cast<std::uint16_t>(at[1]) << 8));
}

std::uint16_t cap16(std::uint64_t v) {
  return v > 0xFFFFu ? static_cast<std::uint16_t>(0xFFFFu) : static_cast<std::uint16_t>(v);
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// " or more" when a figure sat on its cap, so that nobody reads a capped number as exact.
const char* orMore(std::uint16_t value, std::uint16_t cap) { return value >= cap ? " or more" : ""; }

// When a receive was asked, as a clause to put after the moment it came back: `at_ms` less how
// long its command took (task p15-07, deliverable 2 - *when each receive was sent, and when it
// came back*, so that a read can be placed against the rings). Empty when the duration was not
// written down, which is every kData moment of a record kept before that task.
void askedAt(const CallStep& step, char* out, std::size_t capacity) {
  out[0] = 0;
  if (!step.took_known) return;
  const std::uint16_t asked = step.at_ms > step.took_ms
                                  ? static_cast<std::uint16_t>(step.at_ms - step.took_ms)
                                  : 0;
  std::snprintf(out, capacity, " (asked at +%u ms%s)", static_cast<unsigned>(asked),
                step.took_or_more ? " or earlier" : "");
}

}  // namespace

// ------------------------------------------------------------------------------- as the call runs

void CallRecord::clear() {
  recording_.store(false, std::memory_order_release);
  for (std::size_t i = 0; i < sizeof bytes_; ++i) bytes_[i] = 0;
  bytes_[0] = kCallRecordVersion;
  offered_.store(0, std::memory_order_release);
  not_yet_.store(0, std::memory_order_release);
  origin_ms_ = 0;
  held_ = false;
  answer_bytes_ = 0;
  answer_status_ = 0;
  answer_known_ = false;
  answer_has_length_ = false;
  answer_chunked_ = false;
  answer_close_ = false;
}

void CallRecord::begin() {
  clear();
  held_ = true;
}

void CallRecord::onSocket(int id) {
  const std::uint8_t kept = static_cast<std::uint8_t>(bytes_[2] & 0xF0u);
  const std::uint8_t narrow = static_cast<std::uint8_t>(id > 0 && id < 16 ? id : 0);
  bytes_[2] = static_cast<std::uint8_t>(kept | narrow);
}

void CallRecord::wentOut(std::uint64_t now_ms, std::size_t bytes) {
  origin_ms_ = now_ms;
  put16(bytes_ + 3, cap16(bytes));
  recording_.store(true, std::memory_order_release);
}

std::uint16_t CallRecord::since(std::uint64_t now_ms) const {
  return now_ms > origin_ms_ ? cap16(now_ms - origin_ms_) : 0;
}

std::size_t CallRecord::take() {
  if (offered_.load(std::memory_order_acquire) >= kOfferedCeiling) return kCallMoments;
  return offered_.fetch_add(1, std::memory_order_acq_rel);
}

void CallRecord::put(std::size_t index, CallMoment what, std::uint64_t now_ms, std::uint16_t value,
                     std::uint8_t state, int cme) {
  if (index >= kCallMoments) return;
  std::uint8_t* at = bytes_ + kCallRecordHeadBytes + index * kCallMomentBytes;
  put16(at, since(now_ms));
  const std::uint8_t narrow_state = static_cast<std::uint8_t>(state < 16 ? state : 15);
  at[2] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(what) & 0x0Fu) |
                                    static_cast<std::uint8_t>(narrow_state << 4));
  put16(at + 3, value);
  // The bias: `+CME ERROR: 0` is a code of its own, so 0 has to mean *no code at all*.
  at[5] = cme < 0 ? 0 : static_cast<std::uint8_t>(cme < 255 ? cme + 1 : 255);
}

void CallRecord::gotData(std::uint64_t now_ms, std::size_t bytes, std::uint32_t took_ms) {
  if (!recording()) return;
  // The sixth byte carries how long the command took, through put()'s plus-one bias (task
  // p15-07): a 0 there is *not written down*, which is what a record kept before that task has,
  // and not *it came back in the same millisecond*. A receive that returns bytes is a UART round
  // trip; the one that takes thirty seconds is the one that fails, and that one's duration is a
  // whole u16.
  const int took = took_ms > kCallTookAtMost ? static_cast<int>(kCallTookAtMost)
                                             : static_cast<int>(took_ms);
  put(take(), CallMoment::kData, now_ms, cap16(bytes), 0, took);
}

void CallRecord::notYet() {
  if (!recording()) return;
  // Counted and not listed: a stalled call asks hundreds of times (every kReceivePollMs of
  // components/modem/walter_tls.h) and each ask says the same thing.
  std::uint16_t seen = not_yet_.load(std::memory_order_acquire);
  while (seen < kCallNotYetAtMost &&
         !not_yet_.compare_exchange_weak(seen, static_cast<std::uint16_t>(seen + 1),
                                         std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
}

void CallRecord::receiveFailed(std::uint64_t now_ms, std::uint32_t took_ms, std::uint8_t state,
                               int cme) {
  if (!recording()) return;
  put(take(), CallMoment::kReceiveFailed, now_ms, cap16(took_ms), state, cme);
}

void CallRecord::rang(std::uint64_t now_ms, std::uint32_t announced) {
  if (!recording()) return;
  put(take(), CallMoment::kRing, now_ms, cap16(announced), 0, -1);
}

void CallRecord::wasClosed(std::uint64_t now_ms) {
  if (!recording()) return;
  put(take(), CallMoment::kClose, now_ms, 0, 0, -1);
}

void CallRecord::answerWas(const interfaces::HttpResponse& response) {
  // A status of 0 is *nothing answered*: there is no answer for this to be about, and the lines
  // of deliverable 3 are then not printed at all.
  answer_known_ = response.status != 0;
  if (!answer_known_) return;
  answer_status_ = response.status;
  answer_has_length_ = response.has_content_length;
  answer_chunked_ = response.chunked;
  answer_close_ = response.connection_close;
  answer_bytes_ = response.body_bytes > 0xFFFFFFFFull
                      ? 0xFFFFFFFFu
                      : static_cast<std::uint32_t>(response.body_bytes);
}

void CallRecord::copyFrom(const CallRecord& other) {
  recording_.store(false, std::memory_order_release);
  std::memcpy(bytes_, other.bytes_, sizeof bytes_);
  offered_.store(other.offered_.load(std::memory_order_acquire), std::memory_order_release);
  not_yet_.store(other.not_yet_.load(std::memory_order_acquire), std::memory_order_release);
  origin_ms_ = other.origin_ms_;
  held_ = other.held_;
  answer_bytes_ = other.answer_bytes_;
  answer_status_ = other.answer_status_;
  answer_known_ = other.answer_known_;
  answer_has_length_ = other.answer_has_length_;
  answer_chunked_ = other.answer_chunked_;
  answer_close_ = other.answer_close_;
}

void CallRecord::ended(interfaces::HttpOutcome outcome) {
  recording_.store(false, std::memory_order_release);
  if (!held_) return;
  const std::uint8_t kept = static_cast<std::uint8_t>(bytes_[1] & 0x0Fu);
  bytes_[1] = static_cast<std::uint8_t>(
      kept | static_cast<std::uint8_t>((static_cast<std::uint8_t>(outcome) & 0x0Fu) << 4));
  put16(bytes_ + 5, not_yet_.load(std::memory_order_acquire));
  const std::uint16_t offered = offered_.load(std::memory_order_acquire);
  const std::uint16_t over = offered > kCallMoments ? static_cast<std::uint16_t>(offered - kCallMoments) : 0;
  const std::uint8_t dropped = over > kCallDroppedAtMost ? kCallDroppedAtMost
                                                         : static_cast<std::uint8_t>(over);
  bytes_[2] = static_cast<std::uint8_t>((bytes_[2] & 0x0Fu) | static_cast<std::uint8_t>(dropped << 4));
}

// ------------------------------------------------------------------------------- what is in it

std::uint8_t CallRecord::callWas() const { return static_cast<std::uint8_t>(bytes_[1] & 0x0Fu); }

void CallRecord::callWas(std::uint8_t what) {
  bytes_[1] = static_cast<std::uint8_t>((bytes_[1] & 0xF0u) | (what & 0x0Fu));
}

int CallRecord::socketId() const { return static_cast<int>(bytes_[2] & 0x0Fu); }
std::uint16_t CallRecord::bytesSent() const { return get16(bytes_ + 3); }
std::uint16_t CallRecord::notYets() const { return get16(bytes_ + 5); }
std::uint8_t CallRecord::dropped() const { return static_cast<std::uint8_t>((bytes_[2] >> 4) & 0x0Fu); }

interfaces::HttpOutcome CallRecord::outcome() const {
  const std::uint8_t value = static_cast<std::uint8_t>((bytes_[1] >> 4) & 0x0Fu);
  return value <= static_cast<std::uint8_t>(interfaces::HttpOutcome::kSocketFailed)
             ? static_cast<interfaces::HttpOutcome>(value)
             : interfaces::HttpOutcome::kOk;
}

std::size_t CallRecord::steps() const {
  std::size_t count = 0;
  while (count < kCallMoments) {
    const std::uint8_t what =
        static_cast<std::uint8_t>(bytes_[kCallRecordHeadBytes + count * kCallMomentBytes + 2] & 0x0Fu);
    if (what == 0 || what >= static_cast<std::uint8_t>(CallMoment::kCount)) break;
    ++count;
  }
  return count;
}

CallStep CallRecord::step(std::size_t index) const {
  CallStep out;
  if (index >= kCallMoments) return out;
  const std::uint8_t* at = bytes_ + kCallRecordHeadBytes + index * kCallMomentBytes;
  const std::uint8_t what = static_cast<std::uint8_t>(at[2] & 0x0Fu);
  if (what == 0 || what >= static_cast<std::uint8_t>(CallMoment::kCount)) return out;
  out.what = static_cast<CallMoment>(what);
  out.at_ms = get16(at);
  out.value = get16(at + 3);
  out.state = static_cast<std::uint8_t>((at[2] >> 4) & 0x0Fu);
  out.had_cme = at[5] != 0;
  out.cme = out.had_cme ? static_cast<std::uint16_t>(at[5] - 1) : 0;
  // How long the receive's command took (task p15-07). kReceiveFailed has carried it in `value`
  // since task p15-02; kData carries it in the sixth byte with the same plus-one bias the CME
  // number has, so a 0 there is a record written before that task and says *not known*.
  if (out.what == CallMoment::kData) {
    out.took_known = at[5] != 0;
    out.took_ms = out.took_known ? static_cast<std::uint16_t>(at[5] - 1) : 0;
    out.took_or_more = at[5] == 0xFFu;
    // The sixth byte is a duration here and not a CME number: nothing is to read it as one.
    out.had_cme = false;
    out.cme = 0;
  } else if (out.what == CallMoment::kReceiveFailed) {
    out.took_known = true;
    out.took_ms = out.value;
    out.took_or_more = out.value >= kCallMomentAtMost;
  }
  return out;
}

// ------------------------------------------------------------------------------- kept over a sleep

bool CallRecord::keep(std::uint8_t what, interfaces::KeyValue* store) {
  if (!held_ || store == nullptr || !store->ready()) return false;
  callWas(what);
  // The hex is a local and not a member: it is 159 bytes, it is wanted once a failed call, and
  // every byte of this object is .bss the main task's stack has to be found around afterwards
  // (docs/ARCHITECTURE.md §14, rail R22).
  char hex[kCallRecordHexBytes] = {};
  const char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < sizeof bytes_; ++i) {
    hex[2 * i] = digits[bytes_[i] >> 4];
    hex[2 * i + 1] = digits[bytes_[i] & 0x0Fu];
  }
  hex[2 * sizeof bytes_] = 0;
  return store->write(kCallRecordKey, hex);
}

bool CallRecord::decode(const char* hex) {
  if (std::strlen(hex) != kCallRecordHexBytes - 1) return false;
  std::uint8_t bytes[kCallRecordBytes] = {};
  for (std::size_t i = 0; i < sizeof bytes; ++i) {
    const int hi = hexValue(hex[2 * i]);
    const int lo = hexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  if (bytes[0] != kCallRecordVersion) return false;
  std::memcpy(bytes_, bytes, sizeof bytes_);
  return true;
}

bool CallRecord::load(interfaces::KeyValue* store) {
  clear();
  if (store == nullptr || !store->ready()) return false;
  char hex[kCallRecordHexBytes] = {};
  if (!store->read(kCallRecordKey, hex, sizeof hex)) return false;
  if (!decode(hex)) {
    clear();
    return false;
  }
  not_yet_.store(get16(bytes_ + 5), std::memory_order_release);
  held_ = true;
  return true;
}

bool CallRecord::forget(interfaces::KeyValue* store) {
  if (store == nullptr || !store->ready()) return false;
  return store->erase(kCallRecordKey);
}

// ------------------------------------------------------------------------------- in words

void CallRecord::describe(interfaces::TextOut& out, const char* what, const char* lead) const {
  char line[200];
  auto say = [&out, &line](int n) {
    if (n <= 0) return;
    const std::size_t length = static_cast<std::size_t>(n) < sizeof line
                                   ? static_cast<std::size_t>(n)
                                   : sizeof line - 1;
    out.write(line, length);
  };
  if (!held_) {
    say(std::snprintf(line, sizeof line,
                      "  what the modem said: no call has been written down yet\n"));
    return;
  }
  const std::uint16_t not_yet = notYets();
  // A call that was never dialled has no socket, and `socket 0` would read as one.
  char where[24];
  if (socketId() != 0) {
    std::snprintf(where, sizeof where, "on socket %d", socketId());
  } else {
    std::snprintf(where, sizeof where, "on no socket");
  }
  say(std::snprintf(line, sizeof line,
                    "  what the modem said %s (%s), %s: %u%s bytes sent, "
                    "%u%s receives answered \"nothing buffered yet\"\n",
                    lead != nullptr ? lead : "in it", what != nullptr ? what : "a call", where,
                    static_cast<unsigned>(bytesSent()), orMore(bytesSent(), kCallBytesAtMost),
                    static_cast<unsigned>(not_yet), orMore(not_yet, kCallNotYetAtMost)));
  say(std::snprintf(line, sizeof line, "    it ended as: %s\n", interfaces::describe(outcome())));
  const std::size_t count = steps();
  if (count == 0) {
    say(std::snprintf(line, sizeof line, "    and the modem said nothing at all after that\n"));
  }
  for (std::size_t i = 0; i < count; ++i) {
    const CallStep s = step(i);
    const char* later = orMore(s.at_ms, kCallMomentAtMost);
    // When this receive was asked, for the two kinds that are one (task p15-07): the clause is
    // empty for a ring, a close and the retired ask, which are not commands this end sent.
    char asked[40] = {};
    askedAt(s, asked, sizeof asked);
    switch (s.what) {
      case CallMoment::kData:
        say(std::snprintf(line, sizeof line, "    +%u ms%s%s: a receive returned %u%s bytes\n",
                          static_cast<unsigned>(s.at_ms), later, asked,
                          static_cast<unsigned>(s.value), orMore(s.value, kCallBytesAtMost)));
        break;
      case CallMoment::kReceiveFailed:
        if (s.had_cme) {
          say(std::snprintf(line, sizeof line,
                            "    +%u ms%s%s: a receive failed after %u%s ms: +CME ERROR: %u\n",
                            static_cast<unsigned>(s.at_ms), later, asked,
                            static_cast<unsigned>(s.value),
                            orMore(s.value, kCallMomentAtMost), static_cast<unsigned>(s.cme)));
        } else if (s.state == kCallStateNoAnswer) {
          say(std::snprintf(line, sizeof line,
                            "    +%u ms%s%s: a receive failed after %u%s ms: nothing answered, the "
                            "library gave up on the command\n",
                            static_cast<unsigned>(s.at_ms), later, asked,
                            static_cast<unsigned>(s.value),
                            orMore(s.value, kCallMomentAtMost)));
        } else {
          say(std::snprintf(line, sizeof line,
                            "    +%u ms%s%s: a receive failed after %u%s ms: no code, the library's "
                            "state %u\n",
                            static_cast<unsigned>(s.at_ms), later, asked,
                            static_cast<unsigned>(s.value),
                            orMore(s.value, kCallMomentAtMost), static_cast<unsigned>(s.state)));
        }
        break;
      case CallMoment::kRing:
        say(std::snprintf(line, sizeof line, "    +%u ms%s: a ring: the modem announced %u%s bytes\n",
                          static_cast<unsigned>(s.at_ms), later, static_cast<unsigned>(s.value),
                          orMore(s.value, kCallBytesAtMost)));
        break;
      case CallMoment::kClose:
        say(std::snprintf(line, sizeof line, "    +%u ms%s: the other end closed\n",
                          static_cast<unsigned>(s.at_ms), later));
        break;
      case CallMoment::kReadAgain:
        // The one more ask, and what came of it. **Read and printed, never written, since task
        // p15-05**: the firmware of task p15-03 made that ask, and a record it kept is in the
        // store of every unit that ran it - so these four lines stay exactly as they were, and
        // the first thing a unit says after taking this release is what the last one found.
        if (s.state == static_cast<std::uint8_t>(ReadAgainFound::kBytes)) {
          say(std::snprintf(line, sizeof line,
                            "    +%u ms%s: asked again after that: the modem still held %u%s bytes\n",
                            static_cast<unsigned>(s.at_ms), later, static_cast<unsigned>(s.value),
                            orMore(s.value, kCallBytesAtMost)));
        } else if (s.state != static_cast<std::uint8_t>(ReadAgainFound::kFailed)) {
          say(std::snprintf(line, sizeof line,
                            "    +%u ms%s: asked again after that: the modem held nothing\n",
                            static_cast<unsigned>(s.at_ms), later));
        } else if (s.had_cme) {
          say(std::snprintf(line, sizeof line,
                            "    +%u ms%s: asked again after that: it failed too, +CME ERROR: %u\n",
                            static_cast<unsigned>(s.at_ms), later, static_cast<unsigned>(s.cme)));
        } else {
          say(std::snprintf(line, sizeof line,
                            "    +%u ms%s: asked again after that: it failed too, nothing answered\n",
                            static_cast<unsigned>(s.at_ms), later));
        }
        break;
      case CallMoment::kNothing:
      case CallMoment::kCount:
        break;
    }
  }
  if (dropped() != 0) {
    say(std::snprintf(line, sizeof line, "    and %u%s more there was no room for\n",
                      static_cast<unsigned>(dropped()),
                      dropped() >= kCallDroppedAtMost ? " or more" : ""));
  }
  // **What the answer said of itself** (task p15-07, deliverable 3), for a call something
  // answered: the status, how the body was framed, how many bytes of it were read, whether the
  // server asked for the close - and where that close fell against the last byte, which is the
  // one thing a stalled call and a call that worked differ in that no number above says.
  //
  // Numbers and those words only (spec/SYSTEM.md S-12): no header's value, no byte of the body.
  // Not printed at all for a call nothing answered, nor for a record read back out of the store -
  // these four are not in the stored bytes, so a record kept by any earlier firmware says
  // exactly what it always said.
  if (!answer_known_) return;
  const char* framing = answer_has_length_ ? "framed by a length"
                        : answer_chunked_  ? "chunked"
                                           : "framed by nothing, so it ran to the close";
  say(std::snprintf(line, sizeof line, "    the answer: http %u, %s, %u bytes of body read, %s\n",
                    static_cast<unsigned>(answer_status_), framing,
                    static_cast<unsigned>(answer_bytes_),
                    answer_close_ ? "and it said Connection: close" : "and it said no Connection: close"));
  std::size_t close_at = count;
  std::size_t last_data = count;
  for (std::size_t i = 0; i < count; ++i) {
    const CallMoment kind = step(i).what;
    if (kind == CallMoment::kClose && close_at == count) close_at = i;
    if (kind == CallMoment::kData) last_data = i;
  }
  if (close_at == count) {
    // Not a fault, and worth saying plainly: recording stops the moment the call ends, and a
    // response framed by a Content-Length ends on its last byte - so the `+SQNSH` the server
    // sends behind it often arrives after this record was closed. The call that fails is the
    // other way about: its close is handed over long before it gives up.
    say(std::snprintf(line, sizeof line,
                      "    the other end's close was not handed over before the call ended\n"));
  } else if (last_data == count) {
    say(std::snprintf(line, sizeof line,
                      "    the other end's close was handed over, and no receive returned a byte\n"));
  } else {
    say(std::snprintf(line, sizeof line, "    the other end's close was handed over %s the last "
                      "byte was read\n", close_at < last_data ? "before" : "after"));
  }
}

}  // namespace net
}  // namespace core
