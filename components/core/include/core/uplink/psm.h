// The two 3GPP power-saving timers, as octets and as seconds (task p3-01). Pure arithmetic over
// the encodings of **3GPP TS 24.008 release 13**:
//
//   T3412 extended - the periodic tracking area update, which is how long the modem may sleep -
//   is a "GPRS Timer 3" information element, §10.5.7.4a: one octet, bits 8-6 a unit and bits 5-1
//   a value of 0 to 31.
//
//     000  value is a multiple of 10 minutes      100  30 seconds
//     001  1 hour                                 101  1 minute
//     010  10 hours                               110  320 hours
//     011  2 seconds                              111  the timer is deactivated
//
//   T3324 - the active time, how long the modem stays reachable after a transmission - is a
//   "GPRS Timer 2" information element, §10.5.7.3: the same shape, other units.
//
//     000  2 seconds                              111  the timer is deactivated
//     001  1 minute                               other  a multiple of 1 minute
//     010  decihours (6 minutes)
//
// The modem library wants them as eight-character strings of '0' and '1' (`AT+CPSMS=1,,,"<tau>",
// "<active>"`, WalterModem.cpp configPSM) and reports what the network granted the same way, in
// the +CEREG unsolicited report; parse() and bits() are that conversion.
//
// **Why this is written here and not taken from the library.** `WalterModem::durationToTAU` and
// `durationToActiveTime` exist for exactly this and are wrong at 1.5.0: `_convertDuration`
// (WalterModem.cpp:4776) never assigns `smallest_modulo`, so the *last* unit with a usable
// multiplier wins rather than the closest one, and it then reports
// `*actual_duration_seconds = final_base * final_mult` using the unit's **index** instead of
// `base_times[final_base]`. Both are reported in tasks/phase-3/reports/01.md. The encoding is
// four lines of arithmetic and a host test; a wrong TAU is a modem that sleeps for 2 seconds or
// for 320 hours.
#pragma once

#include <cstdint>

namespace core {
namespace uplink {

// Unit 111: "the timer is deactivated" in both information elements.
constexpr std::uint8_t kTimerDeactivated = 0xE0;

// The seconds an octet means. 0 is "deactivated" - which is also what an octet of 0 means (a
// value of 0 in any unit), and the two are the same thing to a caller.
std::uint32_t tauSeconds(std::uint8_t octet);
std::uint32_t activeTimeSeconds(std::uint8_t octet);

// The octet that asks for about this many seconds: the representable duration closest to
// `seconds` **without exceeding it**, except that a request below the smallest representable
// duration gets the smallest one rather than nothing. `actual` (optional) is what the octet
// really means, so that a caller can say what it asked for rather than what it meant to ask for.
std::uint8_t tauOctet(std::uint32_t seconds, std::uint32_t* actual = nullptr);
std::uint8_t activeTimeOctet(std::uint32_t seconds, std::uint32_t* actual = nullptr);

// "00000110" -> 0x06. False (and `*out` untouched) unless it is exactly eight characters of '0'
// and '1': the modem's report is not to be guessed at.
bool parseTimerBits(const char* bits, std::uint8_t* out);

// 0x06 -> "00000110". `out` is nine bytes: eight and a terminator.
void timerBits(std::uint8_t octet, char out[9]);

}  // namespace uplink
}  // namespace core
