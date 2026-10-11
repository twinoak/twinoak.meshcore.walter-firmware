// The port: the UART and its two pins, taken and given back together (task p1-02, acceptance
// criterion 4). What this file holds is the shape of the thing - LinkPort::open() is the only
// caller of RadioPins::uartEnable() and LinkPort::close() the only caller of uartRelease(), and
// nothing reads or writes the stream unless both halves are live - and what the fake pins see on
// the wires while it happens (docs/ARCHITECTURE.md §2 rule 1: TX is driven only in that window).
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "core/board/pin_map.h"
#include "core/link/client/client.h"
#include "fixtures/link_fields.h"
#include "sim/link_bench.h"

namespace link = core::link;
namespace lc = core::link::client;
using core::board::kWalterRx;
using core::board::kWalterTx;
using interfaces::PinMode;

TEST_CASE("TX is driven between open and close, and pulled up the rest of the time") {
  sim::LinkBench bench;
  // The safe state (rule 1): TX pulled up through the chip's weak pull-up, not driven, while the
  // radio is believed powered. RX an input.
  CHECK(bench.pins.modeOf(kWalterTx.gpio) == PinMode::kInputPullUp);
  CHECK_FALSE(bench.pins.driving(kWalterTx.gpio));
  CHECK(bench.pins.modeOf(kWalterRx.gpio) == PinMode::kInput);
  CHECK_FALSE(bench.port.isOpen());
  CHECK(bench.radio.opens == 0);

  REQUIRE(bench.port.open() == core::board::Outcome::kOk);
  CHECK(bench.port.isOpen());
  CHECK(bench.board.uartEnabled());
  CHECK(bench.pins.modeOf(kWalterTx.gpio) == PinMode::kOutput);
  CHECK(bench.pins.driving(kWalterTx.gpio));
  CHECK(bench.pins.levelOf(kWalterTx.gpio));  // idle high (§1: the line's idle level)
  CHECK(bench.radio.opens == 1);
  CHECK(bench.radio.is_open);

  bench.port.close();
  CHECK_FALSE(bench.port.isOpen());
  CHECK_FALSE(bench.board.uartEnabled());
  CHECK_FALSE(bench.pins.driving(kWalterTx.gpio));
  CHECK(bench.pins.modeOf(kWalterTx.gpio) == PinMode::kInputPullUp);
  CHECK(bench.radio.closes == 1);
  CHECK(bench.pins.violations.empty());
}

TEST_CASE("a closed port sends nothing and reads nothing, and no pin moves") {
  sim::LinkBench bench;
  const std::size_t moves = bench.pins.log.size();
  const std::uint8_t byte = 0x55;
  CHECK_FALSE(bench.port.send(&byte, 1));
  std::uint8_t out[4] = {};
  CHECK(bench.port.read(out, sizeof out) == 0);
  CHECK_FALSE(bench.port.idleFor(0));
  CHECK(bench.pins.log.size() == moves);
  CHECK(bench.radio.writes.empty());
  CHECK(bench.port.failedSends() == 1);
}

TEST_CASE("a call opens the port by itself, and the frame goes out whole") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  CHECK(bench.port.isOpen());
  CHECK(bench.port.opens() == 1);
  CHECK(bench.port.sent() == 1);
  CHECK(bench.port.failedSends() == 0);
  // §2: a frame goes out in one piece, with nothing interleaved - one write() per frame.
  REQUIRE(bench.radio.writes.size() == 1);
  CHECK(bench.radio.writes[0].size() >= 6);
}

TEST_CASE("a kill takes the pins from under the port: it closes the stream and reopens later") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  REQUIRE(bench.port.isOpen());

  // ARCHITECTURE §2 rule 3: before the kill line goes high every other radio-facing pin is a
  // plain input, TX included - whoever held the UART loses it, and is not told.
  REQUIRE(bench.board.killCycle(2000) == core::board::Outcome::kOk);
  CHECK_FALSE(bench.board.uartEnabled());
  CHECK_FALSE(bench.port.isOpen());
  CHECK(bench.port.lost() == 1);
  CHECK(bench.radio.closes == 1);
  CHECK_FALSE(bench.radio.is_open);
  CHECK(bench.pins.violations.empty());

  // The next call takes it back. (The radio over there rebooted, so a real one would need a new
  // hello; the port does not care.)
  REQUIRE(bench.client.ping(1).ok());
  CHECK(bench.port.opens() == 2);
  CHECK(bench.radio.opens == 2);
  CHECK(bench.pins.violations.empty());
}

TEST_CASE("a stream that will not open leaves the pins as they were") {
  sim::LinkBench bench;
  bench.radio.refuse_open = true;
  CHECK(bench.port.open() == core::board::Outcome::kPinRefused);
  CHECK_FALSE(bench.port.isOpen());
  CHECK_FALSE(bench.board.uartEnabled());
  CHECK_FALSE(bench.pins.driving(kWalterTx.gpio));
  CHECK(bench.pins.modeOf(kWalterTx.gpio) == PinMode::kInputPullUp);
  // And a call says so rather than pretending to have sent something.
  CHECK(bench.client.ping(1).fault == lc::Fault::kClosed);
  CHECK(bench.radio.writes.empty());
}

TEST_CASE("a pin the driver refuses is a refused open, and the board is back in the safe state") {
  sim::LinkBench bench;
  bench.pins.fail_when = [](const fakes::Op& op) {
    return op.gpio == kWalterTx.gpio && op.kind == fakes::OpKind::kMode;
  };
  CHECK(bench.port.open() == core::board::Outcome::kPinRefused);
  CHECK_FALSE(bench.port.isOpen());
  CHECK(bench.client.status().fault == lc::Fault::kClosed);
  CHECK(bench.radio.writes.empty());
  bench.pins.fail_when = nullptr;
  // Once the driver is willing again, the next call opens the port.
  CHECK(bench.client.status().ok());
  CHECK(bench.pins.violations.empty());
}

TEST_CASE("the port's idle measurement is the line's, not the clock's") {
  // §2's rule is about the line: nothing written and nothing received for 100 ms. The scripted
  // radio answers that question from the fake clock, which is what the ESP-IDF implementation
  // does from esp_timer (platform/uart.cpp).
  sim::LinkBench bench;
  REQUIRE(bench.port.open() == core::board::Outcome::kOk);
  CHECK(bench.port.idleFor(0));
  CHECK_FALSE(bench.port.idleFor(link::kIdleMs));
  bench.clock.advance(link::kIdleMs);
  CHECK(bench.port.idleFor(link::kIdleMs));
  const std::uint8_t byte = 0x42;
  REQUIRE(bench.port.send(&byte, 1));
  CHECK_FALSE(bench.port.idleFor(link::kIdleMs));
}

TEST_CASE("the first frame after an idle line carries §2's 0x00, and a frame after one does not") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  REQUIRE(bench.radio.writes.size() == 1);
  // Nothing had gone out before, so the line had been idle for as long as the client existed.
  CHECK(bench.radio.writes[0][0] == 0x00);

  // A second request in the same breath: under 100 ms since the last frame, so no 0x00.
  REQUIRE(bench.client.ping(1).ok());
  REQUIRE(bench.radio.writes.size() == 2);
  CHECK(bench.radio.writes[1][0] != 0x00);

  // And after a quiet second, the next one carries it again.
  bench.clock.advance(1000);
  REQUIRE(bench.client.status().ok());
  REQUIRE(bench.radio.writes.size() == 3);
  CHECK(bench.radio.writes[2][0] == 0x00);
}
