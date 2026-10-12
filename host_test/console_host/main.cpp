// The Walter's console on stdin and stdout (task p2-02), so that `tools/walterctl.py push` can be
// tested against the real thing before a board exists - the acceptance criterion's "behind a
// pipe". It is the same `core::console::Console` the firmware builds, with the same image store
// and the same framed transfer; what differs is what is behind the interfaces:
//
//   the flash     host_test/sim/sim_flash.h, in memory - the layout's size, erased
//   the clock     the real one (std::chrono), because a laptop is on the other end
//   the pins      a plain array: nothing here drives a radio, and the pin rules are
//                 host_test/core/board/'s business, not this program's
//   the UART      a stream that will not open: there is no radio, so `rak ...` says the link is
//                 down, which is true
//   the keys      the specification's public test key, read from the generated fixture header -
//                 so this program is a test build by construction and says so in its banner
//
// It is not a firmware and not a fake of one: it is the console's own code with a simulated flash
// under it. Nothing here is compiled into the firmware (host_test/ is its own CMake project).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/image/choose.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/image/trust.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "interfaces/byte_stream.h"
#include "interfaces/clock.h"
#include "interfaces/pins.h"
#include "interfaces/restart.h"
#include "interfaces/retained.h"
#include "interfaces/text_out.h"
#include "interfaces/wall_clock.h"
#include "manifest_fixtures.h"
#include "sim/sim_flash.h"

namespace {

class HostPins final : public interfaces::Pins {
 public:
  bool setMode(std::uint8_t gpio, interfaces::PinMode mode) override {
    mode_[gpio] = mode;
    return true;
  }
  bool write(std::uint8_t gpio, bool high) override {
    level_[gpio] = high;
    return true;
  }
  bool read(std::uint8_t gpio) override { return level_[gpio]; }
  bool holdThroughSleep(const std::uint8_t*, std::size_t, bool) override { return true; }

 private:
  interfaces::PinMode mode_[64] = {};
  bool level_[64] = {};
};

class HostClock final : public interfaces::Clock {
 public:
  std::uint64_t nowMs() override {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now() - start_).count());
  }
  void delayMs(std::uint32_t ms) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }
  void delayUs(std::uint32_t us) override {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
  }

 private:
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Retained storage in ordinary memory: this program is one run, so "survives a reset" has no
// meaning here and a power cut is the process ending.
class HostRetained final : public interfaces::Retained {
 public:
  bool load(interfaces::RetainedSlot slot, void* out, std::size_t size) override {
    const std::size_t i = static_cast<std::size_t>(slot);
    if (i >= kSlots || !have_[i] || size_[i] != size) return false;
    std::memcpy(out, bytes_[i], size);
    return true;
  }
  bool store(interfaces::RetainedSlot slot, const void* data, std::size_t size) override {
    const std::size_t i = static_cast<std::size_t>(slot);
    if (i >= kSlots || size > interfaces::kRetainedRecordMax) return false;
    std::memcpy(bytes_[i], data, size);
    size_[i] = size;
    have_[i] = true;
    return true;
  }

 private:
  static constexpr std::size_t kSlots = static_cast<std::size_t>(interfaces::RetainedSlot::kCount);
  std::uint8_t bytes_[kSlots][interfaces::kRetainedRecordMax] = {};
  std::size_t size_[kSlots] = {};
  bool have_[kSlots] = {};
};

class HostRestart final : public interfaces::Restart {
 public:
  void restart() override { std::printf("(a restart was asked for; this program keeps running)\n"); }
};

// stdout, unbuffered: the laptop on the other end of the pipe is waiting for frames.
class StdOut final : public interfaces::TextOut {
 public:
  void write(const char* text, std::size_t length) override {
    std::fwrite(text, 1, length, stdout);
    std::fflush(stdout);
  }
};

// There is no radio: the stream will not open, and the link client says the link is down.
class NoRadio final : public interfaces::ByteStream {
 public:
  bool open() override { return false; }
  void close() override {}
  std::size_t read(std::uint8_t*, std::size_t) override { return 0; }
  std::size_t write(const std::uint8_t*, std::size_t) override { return 0; }
  bool flush(std::uint32_t) override { return true; }
  bool idleFor(std::uint32_t) override { return true; }
};

class NoWallClock final : public interfaces::WallClock {
 public:
  bool now(interfaces::WallTime*) override { return false; }
};

bool hexToBytes(const char* hex, std::uint8_t* out, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    unsigned value = 0;
    if (std::sscanf(hex + i * 2, "%2x", &value) != 1) return false;
    out[i] = static_cast<std::uint8_t>(value);
  }
  return true;
}

HostPins g_pins;
HostClock g_clock;
HostRetained g_retained;
HostRestart g_restart;
StdOut g_out;
NoRadio g_radio_uart;
NoWallClock g_wall;

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // The trust table: the specification's public test key and nothing else, so that the fixture
  // release this is tested against is accepted. A unit's own table comes from
  // keys/release-keys.txt at build time and is empty today (task p2-01).
  static core::image::TrustedKey keys[1] = {};
  if (!hexToBytes(fixtures::kTestPublicKeyHex, keys[0].key, core::image::kEd25519PublicKeySize)) {
    std::fprintf(stderr, "the fixture test key is not hexadecimal\n");
    return 2;
  }
  core::image::keyIdOf(keys[0].key, keys[0].id);
  static const core::image::TrustTable trust(keys, 1);

  // What this "unit" is, for a manifest's `requires`: the specification's fixture manifests ask
  // for Walter 0.3.0 and link major 1, and a test harness that claimed to be 0.0.0-dev would be
  // refused at check 3 for a reason that has nothing to do with what is being tested.
  core::image::ThisUnit unit;
  unit.version = "0.3.0+consolehost";
  unit.link_majors[0] = 1;
  unit.link_major_count = 1;

  static sim::SimFlash flash(core::image::kStoreBytes);
  static core::image::Store store(flash, g_wall);
  static core::image::ImageVerdict verdict(trust, unit);
  static core::image::Receiver receiver(store, verdict, g_out, g_clock);

  static core::board::RadioPins radio_pins(g_pins, g_clock, g_retained);
  static core::board::SingleTarget target;
  static core::board::SlotLock slots(target);
  static core::health::Health health(g_retained, g_clock);
  health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
  static core::link::client::LinkPort port(radio_pins, g_radio_uart);
  static core::link::client::Client link(port, g_clock, g_wall, slots);

  const core::image::StoreError opened = store.begin();
  if (opened != core::image::StoreError::kNone) {
    std::fprintf(stderr, "the simulated image store would not start: %s\n",
                 core::image::describe(opened));
    return 2;
  }

  static core::console::Console console(g_out, radio_pins, health, g_clock, g_restart, link, slots,
                                        &store, &receiver);
  std::printf("twinoak-walter console host (a simulated flash, the specification's TEST KEY, no radio)\n");
  console.begin();

  for (;;) {
    const int c = std::fgetc(stdin);
    if (c == EOF) break;
    console.feed(static_cast<char>(c));
  }
  return 0;
}
