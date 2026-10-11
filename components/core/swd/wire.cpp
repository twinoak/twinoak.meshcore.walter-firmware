#include "core/swd/wire.h"

namespace core {
namespace swd {

Wire::Wire(board::SwdLines& lines, interfaces::Clock& clock, std::uint32_t half_period_us)
    : lines_(lines), clock_(clock), half_us_(kDefaultHalfPeriodUs) {
  setHalfPeriodUs(half_period_us);
}

std::uint32_t Wire::setHalfPeriodUs(std::uint32_t us) {
  if (us < kMinHalfPeriodUs) us = kMinHalfPeriodUs;
  if (us > kMaxHalfPeriodUs) us = kMaxHalfPeriodUs;
  half_us_ = us;
  return half_us_;
}

// Where in the cycle a bit of the target's is read. Each line has 470 ohm in series and about
// 2.2 nF to ground (BENCH-BREADBOARD.md section 4; docs/ARCHITECTURE.md section 2): a time
// constant tau of about 1 us, on SWCLK as the target sees it and on SWDIO as the target drives
// it. The target changes SWDIO at the rising edge of SWCLK (B4.3.1) - the edge *it* sees, about
// 0.7 tau after we drive it - and then has to pull 2.2 nF to the new level; at a turnaround it
// starts from whatever the line was left at (the park bit's high), which is the tight spot
// BENCH-BREADBOARD.md section 4 names: "SWDIO turnaround (target ACK bits) is the tight spot,
// not SWCLK". Reading at the end of the low half, just before we raise the clock again, gives
// the bit a whole period less the clock's own delay: at the default 125 kHz (an 8 us period)
// about 7 us, seven time constants; at the predicted ceiling of 300 kHz about 2.6 tau, 93 %
// settled. Reading at the middle of the cycle - the end of the high half - would give half of
// that, under one tau at the ceiling, for nothing in return: the bit cannot change before the
// next rising edge, and we have not produced it yet.
bool Wire::cycle(bool* sample) {
  clock_.delayUs(half_us_);
  if (sample != nullptr) *sample = lines_.readIo();
  if (!lines_.clock(true)) return false;
  clock_.delayUs(half_us_);
  if (!lines_.clock(false)) return false;
  clocks_++;
  return true;
}

bool Wire::driveBit(bool high) { return lines_.driveIo(high); }

bool Wire::lineReset(unsigned clocks) {
  if (!driveBit(true)) return false;
  for (unsigned i = 0; i < clocks; ++i) {
    if (!cycle()) return false;
  }
  return true;
}

bool Wire::jtagToSwd() { return writeBits(kJtagToSwd, kJtagToSwdBits); }

bool Wire::idle(unsigned clocks) {
  if (!driveBit(false)) return false;
  for (unsigned i = 0; i < clocks; ++i) {
    if (!cycle()) return false;
  }
  return true;
}

bool Wire::writeBits(std::uint32_t bits, unsigned count) {
  if (count > 32) count = 32;
  for (unsigned i = 0; i < count; ++i) {
    if (!driveBit(((bits >> i) & 1u) != 0)) return false;
    if (!cycle()) return false;
  }
  return true;
}

bool Wire::readBits(unsigned count, std::uint32_t* out) {
  if (count > 32) count = 32;
  if (!lines_.releaseIo()) return false;
  std::uint32_t value = 0;
  for (unsigned i = 0; i < count; ++i) {
    bool bit = false;
    if (!cycle(&bit)) return false;
    if (bit) value |= 1u << i;
  }
  *out = value;
  return true;
}

bool Wire::turnaround() {
  if (!lines_.releaseIo()) return false;
  return cycle();
}

bool Wire::release() { return lines_.releaseIo(); }

bool Wire::backOff() {
  if (!lines_.releaseIo()) return false;
  for (unsigned i = 0; i < kBackOffClocks; ++i) {
    if (!cycle()) return false;
  }
  return true;
}

std::uint8_t Wire::request(bool ap, bool read, std::uint8_t addr) {
  const unsigned a2 = (addr >> 2) & 1u;
  const unsigned a3 = (addr >> 3) & 1u;
  const unsigned apndp = ap ? 1u : 0u;
  const unsigned rnw = read ? 1u : 0u;
  const unsigned parity = (apndp + rnw + a2 + a3) & 1u;
  // Bit 0 goes first on the wire (B4.1.5): start, APnDP, RnW, A[2], A[3], parity, stop, park.
  return static_cast<std::uint8_t>(1u | (apndp << 1) | (rnw << 2) | (a2 << 3) | (a3 << 4) |
                                   (parity << 5) | (0u << 6) | (1u << 7));
}

bool Wire::parity32(std::uint32_t value) {
  value ^= value >> 16;
  value ^= value >> 8;
  value ^= value >> 4;
  value ^= value >> 2;
  value ^= value >> 1;
  return (value & 1u) != 0;
}

// The sequence, in rising edges counted from the one that samples the park bit (P), as B4.2's
// figures lay it out and as the simulator checks it:
//   P+1        the target drives ACK[0] (it changes the line on the rising edge, B4.3.1): the
//              turnaround between the request and the acknowledge is the cycle ending here, and
//              SWDIO was released right after P's falling edge;
//   P+2..P+4   ACK[0..2] are read just before each of these edges;
//   read, OK   P+4..P+36 the target drives RDATA[0..31] and the parity, read before P+5..P+37;
//              it stops driving at P+37; P+38 is the turnaround; the host drives again after it;
//   write, OK  the target stops driving at P+4; P+5 is the turnaround; WDATA[0..31] and the
//              parity are sampled by the target at P+6..P+38;
//   WAIT/FAULT the target stops driving at P+4; P+5 is the turnaround; no data phase (B4.2.3,
//              B4.2.4, overrun detection off).
Transfer Wire::transfer(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata) {
  Transfer t;
  t.lines = false;
  if (!writeBits(request(ap, read, addr), 8)) return t;
  if (!turnaround()) return t;
  std::uint32_t ack = 0;
  if (!readBits(3, &ack)) return t;
  t.ack = static_cast<std::uint8_t>(ack);
  if (t.ack == ack::kOk) {
    if (read) {
      std::uint32_t parity = 0;
      if (!readBits(32, &t.data)) return t;
      if (!readBits(1, &parity)) return t;
      t.parity_ok = parity32(t.data) == (parity != 0);
      if (!turnaround()) return t;
    } else {
      if (!turnaround()) return t;
      if (!writeBits(wdata, 32)) return t;
      if (!writeBits(parity32(wdata) ? 1u : 0u, 1)) return t;
    }
  } else if (t.ack == ack::kWait || t.ack == ack::kFault) {
    if (!turnaround()) return t;
  } else {
    // Nothing recognisable answered. Back off, release, and leave the idle cycles out: whatever
    // is over there is not in a transfer that needs clocking through, and what comes next is a
    // line reset or nothing (B4.2.5).
    if (!backOff()) return t;
    t.lines = release();
    return t;
  }
  if (!idle(kIdleAfterTransfer)) return t;
  t.lines = release();
  return t;
}

}  // namespace swd
}  // namespace core
