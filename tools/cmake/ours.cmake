# The flags of our own components - core, hal, platform, modem, main - and of the host tests. Not
# ESP-IDF's, not the managed ones' (tasks/README.md: warnings are errors in our own components).
#
# ESP-IDF 5.5 compiles everything with -Wall -Werror but exempts the unused-* and
# deprecated-declarations warnings and the whole of -Wextra ($IDF_PATH/tools/cmake/build.cmake,
# the compile_options list), and gnu++2b. The lines below come after those on the command line and
# put each exemption back for our code only. C++17: what core/ is written in, host and target
# alike; exceptions and RTTI are off by sdkconfig for the firmware and by host_test/ for core.
#
# A second argument is the dialect, for code that is not ours: `twinoak_ours(agent_core c++17)`
# gives the copied library and its suites the same strictness in the strict dialect upstream
# compiles them in (host_test/CMakeLists.txt says why that matters there, task p4-01). Our own
# code stays gnu++17, which is what ESP-IDF's own dialect is a later version of.
function(twinoak_ours target)
  set(dialect gnu++17)
  if(ARGC GREATER 1)
    set(dialect ${ARGV1})
  endif()
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Werror -Werror=extra
    -Werror=unused-function -Werror=unused-variable -Werror=unused-but-set-variable
    -Werror=deprecated-declarations
    -Wunused-parameter -Wsign-compare
    $<$<COMPILE_LANGUAGE:CXX>:-std=${dialect}>)
endfunction()
