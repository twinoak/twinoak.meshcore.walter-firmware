// The seven pins this firmware shares with the radio: net, D-sub pin, Walter GPIO. From the
// hardware repository (twinoak.meshcore.standard-repeater-design-v2) at
// b798aab043281fa4c9b17802a164fa94be12e206, hardware/pcb/INTERCONNECT.md, "D-sub pinouts", the
// "LTE box (B-side)" column - frozen 2026-10-10 - and the same numbers in docs/ARCHITECTURE.md §2,
// which host_test/core/board/test_pin_map.cpp reads and holds these constants against, so that
// the document and the code cannot drift apart.
//
// Nothing outside core/board/ (RadioPins) names these pins: tools/verify.sh greps for it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace board {

struct PinDef {
  const char* net;    // the net name in INTERCONNECT.md
  std::uint8_t dsub;  // the LTE box's D-sub pin
  std::uint8_t gpio;  // the Walter GPIO (INTERCONNECT.md: "Walter IOn")
};

// INTERCONNECT.md, D-sub pinouts, LTE box: pin 3 LORA_nRESET (Walter IO1), 4 LORA_SWDCLK (IO2),
// 5 LORA_SWDIO (IO4), 6 LORA_PWR_KILL (IO5), 7 spare / NC (IO6 per the adapter pin map, "SPARE"),
// 8 WALTER_TX (IO7), 9 WALTER_RX (IO8). Pins 1 (GND) and 2 (VLOAD+) are not GPIOs.
constexpr PinDef kNReset{"LORA_nRESET", 3, 1};
constexpr PinDef kSwdClk{"LORA_SWDCLK", 4, 2};
constexpr PinDef kSwdIo{"LORA_SWDIO", 5, 4};
constexpr PinDef kPwrKill{"LORA_PWR_KILL", 6, 5};
constexpr PinDef kSpare{"spare", 7, 6};
constexpr PinDef kWalterTx{"WALTER_TX", 8, 7};
constexpr PinDef kWalterRx{"WALTER_RX", 9, 8};

constexpr std::size_t kRadioPinCount = 7;
constexpr PinDef kRadioPins[kRadioPinCount] = {kNReset, kSwdClk, kSwdIo, kPwrKill,
                                               kSpare,  kWalterTx, kWalterRx};

}  // namespace board
}  // namespace core
