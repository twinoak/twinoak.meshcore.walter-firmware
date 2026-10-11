// The modem library is compiled, linked into the image and called by nothing (task p0-01). This
// proves that the component manager resolves dptechnics/walter-modem 1.5.0 inside the container
// and that the library builds and links at this ESP-IDF, before any task depends on it. Taking the
// address of one of its functions is what makes the linker keep its objects; nothing calls it.
// The uplink that will use it is phase 3's (docs/ARCHITECTURE.md §9).
#include "WalterModem.h"

extern "C" const void* twinoak_modem_library_anchor() {
  // begin(uart_port_t, uint16_t) is the ESP-IDF overload (WalterModem.h, the #else of ARDUINO).
  bool (*begin)(uart_port_t, uint16_t) = &WalterModem::begin;
  return reinterpret_cast<const void*>(begin);
}
