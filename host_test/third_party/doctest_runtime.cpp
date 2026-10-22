// doctest's implementation without its main, for the GoogleTest binaries of task p4-02: the
// simulators under host_test/sim/ (the scripted radio of task p1-02 among them) assert with
// doctest's REQUIRE inside their own code, and doctest lets an assertion run outside a test case
// once an implementation is linked - a failure then throws, which GoogleTest reports as the
// failure of whichever test was running. core_tests has host_test/main.cpp instead (the same
// implementation with the main); the two are never in one binary.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

// An assertion reads the context's state even outside a test case, so one context exists for the
// life of the binary and is the default for exactly that. Nothing runs through it: GoogleTest's
// main is the main.
namespace {
struct OutOfTestCases {
  doctest::Context context;
  OutOfTestCases() { context.setAsDefaultForAssertsOutOfTestCases(); }
} g_doctest;
}  // namespace
