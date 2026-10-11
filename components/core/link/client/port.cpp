#include "core/link/client/port.h"

namespace core {
namespace link {
namespace client {

board::Outcome LinkPort::open() {
  if (isOpen()) {
    return board::Outcome::kOk;
  }
  // The pads first (ARCHITECTURE §2 rule 1: TX is driven only from here on), then the peripheral.
  const board::Outcome outcome = pins_.uartEnable();
  if (outcome != board::Outcome::kOk) {
    return outcome;
  }
  if (!stream_.open()) {
    pins_.uartRelease();
    return board::Outcome::kPinRefused;
  }
  open_ = true;
  ++opens_;
  return board::Outcome::kOk;
}

void LinkPort::close() {
  if (open_) {
    stream_.close();
    open_ = false;
  }
  pins_.uartRelease();
}

bool LinkPort::isOpen() {
  if (!open_) {
    return false;
  }
  // The pins can go without this object being told: killCycle() releases everything, and so does
  // any failed pin operation (both call safe()). The stream must not keep a pad that RadioPins
  // believes is a plain input, so it is closed here, once, and counted.
  if (!pins_.uartEnabled()) {
    stream_.close();
    open_ = false;
    ++lost_;
    return false;
  }
  return true;
}

bool LinkPort::send(const std::uint8_t* data, std::size_t size) {
  if (!isOpen() || data == nullptr || size == 0) {
    ++failed_sends_;
    return false;
  }
  if (stream_.write(data, size) != size) {
    ++failed_sends_;
    return false;
  }
  if (!stream_.flush(kFlushMs)) {
    ++failed_sends_;
    return false;
  }
  ++sent_;
  return true;
}

std::size_t LinkPort::read(std::uint8_t* out, std::size_t max) {
  if (!isOpen() || out == nullptr || max == 0) {
    return 0;
  }
  return stream_.read(out, max);
}

bool LinkPort::idleFor(std::uint32_t ms) { return isOpen() && stream_.idleFor(ms); }

}  // namespace client
}  // namespace link
}  // namespace core
