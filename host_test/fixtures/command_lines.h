// Every command of the console's table, with something to type after it so that it runs
// (task p3-03's test of the secrets, task p3-06's rail R8). **One line per row of
// core/console/commands.h**: a command added without a line here fails
// host_test/core/console/test_console_secrets.cpp's first case rather than quietly escaping
// every test that runs "every command".
//
// It lives here, and not in the test that first needed it, because two rails walk it: the
// secrets of task p3-03 (a settings console over a simulated flash) and task p3-06's capture
// (a whole simulated day of wakes, then the same lines).
#pragma once

#include <cstddef>

namespace fixtures {

struct CommandLine {
  const char* word;  // the table's `word`
  const char* line;  // what to type
};

const CommandLine kCommandLines[] = {
    {"help", "help"},
    {"status", "status"},
    {"get", "get"},
    {"set", "set unit.name Window at Grenaa"},
    {"erase", "erase"},  // the refusal; `erase confirm` is a case of its own in each test
    {"reboot", "reboot"},
    {"reboots", "reboots"},
    {"dump", "dump"},
    {"pins", "pins"},
    {"rak reset", "rak reset"},
    {"rak kill", "rak kill 1"},
    {"rak hello", "rak hello"},
    {"rak ping", "rak ping"},
    {"rak status", "rak status"},
    {"rak id", "rak id"},
    {"rak time", "rak time"},
    {"rak power", "rak power"},
    {"rak env", "rak env"},
    {"rak cli", "rak cli get radio"},
    {"rak events", "rak events"},
    {"rak text", "rak text"},
    {"rak swd connect", "rak swd connect"},
    {"rak swd id", "rak swd id"},
    {"rak swd mem", "rak swd mem 0x10000100 4"},
    {"rak flash-test", "rak flash-test confirm"},
    {"rak provision check", "rak provision check"},
    {"rak provision", "rak provision 0badf00ddeadbeef confirm"},
    {"rak update", "rak update"},
    {"rak update --first-install", "rak update --first-install"},
    {"rak update status", "rak update status"},
    {"rak role", "rak role scanner"},
    {"rak watch", "rak watch"},
    {"rak watch reset-caps", "rak watch reset-caps"},
    {"rak ladder", "rak ladder"},
    {"rak mesh status", "rak mesh status a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"},
    {"image list", "image list"},
    {"image verify", "image verify candidate"},
    {"image erase", "image erase candidate"},
    {"image bad", "image bad"},
    {"image receive", "image receive candidate"},
    {"trust", "trust"},
    {"trust tls", "trust tls"},
    {"self status", "self status"},
    {"self receive", "self receive"},
    {"self switch", "self switch"},
    {"self confirm", "self confirm"},
    {"self reject", "self reject"},
    {"lte", "lte"},
    {"lte up", "lte up"},
    {"lte down", "lte down"},
    {"lte time", "lte time"},
    {"lte id", "lte id"},
    {"http get", "http get https://r.example/x"},
    {"fetch", "fetch https://r.example/v1"},
    {"agent url", "agent url"},
    {"agent probe", "agent probe"},
    {"wake", "wake"},
    {"power", "power"},
    {"sleep now", "sleep now"},
    {"sleep", "sleep 600"},
    {"stay", "stay"},
    {"cycle now", "cycle now"},
    {"cycle dry", "cycle dry"},
    {"journal", "journal"},
};

constexpr std::size_t kCommandLineCount = sizeof kCommandLines / sizeof kCommandLines[0];

}  // namespace fixtures
