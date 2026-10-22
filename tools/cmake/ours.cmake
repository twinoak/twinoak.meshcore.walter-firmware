# The flags of our own components - core, hal, platform, modem, main - and of the host tests. Not
# ESP-IDF's, not the managed ones' (tasks/README.md: warnings are errors in our own components).
#
# ESP-IDF 5.5 compiles everything with -Wall -Werror but exempts the unused-* and
# deprecated-declarations warnings and the whole of -Wextra ($IDF_PATH/tools/cmake/build.cmake,
# the compile_options list), and gnu++2b. The lines below come after those on the command line and
# put each exemption back for our code only. C++17: what core/ is written in, host and target
# alike; exceptions and RTTI are off by sdkconfig for the firmware and by host_test/ for core.
#
# One dialect, gnu++17 - what ESP-IDF's own is a later version of - and the copied agent core is
# compiled in it too (host_test/CMakeLists.txt, task p4-01). It took a second argument for one
# release of that library, whose `ConfigReply.cpp` had a parameter named `unix` and so could not
# be compiled in any GNU dialect on a Linux host; v0.3.0 renamed it and greps its own sources for
# every such name, so nothing here needs a dialect of its own.
function(twinoak_ours target)
  set(dialect gnu++17)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Werror -Werror=extra
    -Werror=unused-function -Werror=unused-variable -Werror=unused-but-set-variable
    -Werror=deprecated-declarations
    -Wunused-parameter -Wsign-compare
    $<$<COMPILE_LANGUAGE:CXX>:-std=${dialect}>)
endfunction()
