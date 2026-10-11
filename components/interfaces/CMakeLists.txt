# interfaces/ - what docs/ARCHITECTURE.md §1 calls hal/: the interfaces core/ needs of the world -
# pins, a byte stream, a clock, flash regions, a key-value store, an uplink, a sleeper. Headers
# only, no ESP-IDF include, so host_test/ can give core/ fakes behind the same interfaces and
# platform/ the real thing. Phase 0's task 03 writes the first one (the pins). Nothing yet.
#
# Why not "hal": ESP-IDF has a component of that name ($IDF_PATH/components/hal, included as
# <hal/gpio_hal.h> and friends) and a project component with the same name replaces it, which
# breaks mbedtls and everything else that includes it (task p0-01, deviations). "hal/" in a task
# file or the architecture means this directory; the include prefix is "interfaces/".
idf_component_register(INCLUDE_DIRS "include")
