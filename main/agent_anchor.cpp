// The agent core, linked and not yet called (task p4-01).
//
// `components/agent-core/` is the exit-node firmware's `lib/agent-core/` at one commit
// (docs/upstream.md, docs/ARCHITECTURE.md §13). The three interfaces it stands on are phase 4's
// tasks 02, 03 and 04, and `AgentLoop::wake()` becomes step 8 of the wake cycle in task 05; until
// then nothing in this firmware calls a line of it, and `--gc-sections` would drop every object
// of the library out of the image - so the board build would prove that the library *compiles*
// and not that it links.
//
// So this file does what `core/image/verdict.h`'s anchor did for the six checks before the image
// store had a caller, and the same way round, which matters: the volatile read is **here**, in a
// translation unit of its own, and `main()` only keeps the pointer this function returns. A
// volatile pointer-to-member read written directly in `app_main` is elided - the compiler can see
// that nothing observes it - and the library goes with it.
//
// Taking the address of a function makes the linker resolve it, and with it everything it calls:
// `AgentLoop::wake` reaches the heartbeat, the claim, the results, the observation ring, the
// backoff, the server policy, the uplink's ledger of seconds and bytes, and the image a sleep
// keeps (`KeptImage.cpp`, which is why no object of the library is left to anchor by name but the
// two below); `AgentLoop::loop` reaches the reboot guard;
// `JobRunner::claimAndRun` reaches all thirteen job types, the login sequence, the config read and
// apply engines, the session cache, the airtime budget, the reply codec and the result buffer.
//
// Two of the library's twenty-three objects are reached by none of those three, because nothing in
// the library calls them - they are the consumer's to call: `ConfigReply.cpp`'s reply parsers,
// which an `IUplink` runs over the bytes it read (task 03, docs/library.md §4), and `LogRing.cpp`,
// which is a 4 kB ring a consumer may keep its own log in. Both are anchored by name so that all
// twenty-three compile *and link* for the board here; `LogRing::copyTo` rather than `logRing()`,
// because the free function hands back a static ring and would put its 4 kB of BSS in an image
// that has no use for it yet.
//
// The anchor allocates nothing and constructs nothing: `JobRunner` is 66 kB of buffers and
// `AgentLoop` 7.6 kB (docs/library.md §6), and neither belongs in the image until something
// drives it. Its cost is five pointers of data and the code the linker keeps.

#include <agent/AgentLoop.h>
#include <agent/ConfigReply.h>
#include <agent/JobRunner.h>
#include <agent/LogRing.h>
#include <agent/Version.h>

#include <cstddef>

namespace {

using Wake = mm::WakeReport (mm::AgentLoop::*)(const mm::WakeIntent&, const mm::Budget&);
using Loop = void (mm::AgentLoop::*)();
using ClaimAndRun = int (mm::JobRunner::*)();
using ParseConfig = bool (*)(const char*, std::size_t, mm::AgentConfig&, char*, std::size_t);
using CopyLog = std::size_t (mm::LogRing::*)(char*, std::size_t) const;

}  // namespace

namespace main_app {

// The library's version, as the library itself states it: one of the two versions this firmware
// reports (docs/library.md §10 - "report both versions from your firmware: yours, and
// AGENT_CORE_VERSION"). Task 05 puts it in the console's banner; this is where it comes from.
const char* agentCoreVersion() { return AGENT_CORE_VERSION; }

const void* agentCoreAnchor() {
  static Wake volatile wake = &mm::AgentLoop::wake;
  static Loop volatile loop = &mm::AgentLoop::loop;
  static ClaimAndRun volatile claim = &mm::JobRunner::claimAndRun;
  static ParseConfig volatile parse = &mm::parseAgentConfig;
  static CopyLog volatile copy_log = &mm::LogRing::copyTo;
  static const char kAnchored = 0;
  const Wake w = wake;
  const Loop l = loop;
  const ClaimAndRun c = claim;
  const ParseConfig p = parse;
  const CopyLog g = copy_log;
  const bool all_there = w != nullptr && l != nullptr && c != nullptr && p != nullptr && g != nullptr;
  return all_there ? &kAnchored : nullptr;
}

}  // namespace main_app
