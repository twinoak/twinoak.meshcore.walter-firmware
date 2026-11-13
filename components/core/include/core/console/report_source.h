// What `twinoak.report` reads the unit out of (task p13-01): the console's own sections, by name.
//
// On 13 November a wake request reached the unit's radio, the radio answered *ok*, and the unit
// did not check in. Everything that says why is printed by this console - `wake`, `agent state`,
// `lte` - and on a unit on a mast there is no cable to type them on. So meshmaster.dk asks for
// them instead, and what it gets back is **the console's own lines**: the same printers, writing
// into a buffer instead of into the port, so that no wording of this firmware exists twice and a
// line added to a command is a line the report gains with it.
//
// The seam is here, in core/, rather than a `#include "core/console/console.h"` in
// components/agent/, for the reason every other probe of this console is a seam: the job that
// asks is the agent's (`agent::FirmwareJobs`), the console is core/'s, and neither should have to
// name the whole of the other. core::console::Console implements it; a test hands over its own.
//
// **Nothing a section writes changes anything** - that is the contract of the whole job, not a
// property of the implementation: a section is a read-only command (`agent obs` without its pull,
// the counts of `rak events` without taking the events out of the queue), and the one thing a
// section may do on the wire is ask this unit's own radio over the box link. Nothing goes on the
// air.
#pragma once

#include "interfaces/text_out.h"

namespace core {
namespace console {

class ReportSource {
 public:
  virtual ~ReportSource() = default;

  // The names this firmware has a section for, in the order the report's table lists them.
  virtual int sectionCount() const = 0;
  virtual const char* sectionName(int i) const = 0;

  // One section's lines into `out`, each ended with '\n', instead of into the port. False - and
  // nothing written - when this firmware has no section of that name, which is what the result's
  // `unknown` is for.
  virtual bool writeSection(const char* name, interfaces::TextOut& out) = 0;
};

}  // namespace console
}  // namespace core
