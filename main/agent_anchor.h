// The two things main/ asks of main/agent_anchor.cpp, which is the only file of this firmware that
// includes a header of the agent core today (task p4-01, docs/ARCHITECTURE.md §13). Declared here
// rather than in main.cpp so that the anchor's own translation unit and its caller agree by a
// header and not by a repeated signature.
#pragma once

namespace main_app {

// AGENT_CORE_VERSION, the version in the library's own agent/Version.h: one of the two versions
// this firmware reports (docs/library.md §10).
const char* agentCoreVersion();

// A pointer the linker can only produce if the library was linked. Keep the result in a volatile
// in app_main; the volatile read that does the work is in the .cpp, on purpose.
const void* agentCoreAnchor();

}  // namespace main_app
