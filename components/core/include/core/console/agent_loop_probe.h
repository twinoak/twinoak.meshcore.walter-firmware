// The console's seam into the agent's loop (task p4-05): `agent wake` - one wake of the loop
// now, each step and the report printed - `agent jobs` - the last ten jobs as the library
// remembers them - and `agent state` - the state a sleep keeps. The loop itself is
// components/agent/'s (`agent::AgentBackend`, over the copied library), and core/ cannot name
// it (agent/ is built on core/, not the other way round), so the console takes this interface
// and main/ hands it the implementation. A build without one answers in one line.
//
// What crosses it is text into the console's own `TextOut`, written by the implementation: the
// library's own `jobs` report and this firmware's lines about the loop's state. **No token and
// no password crosses it**: a job's record is an id, a type, an outcome and two counts; the state
// is counters and durations; and the one wake `agent wake` runs posts through the same uplink
// as a wake from sleep does, whose rails (task p4-03) keep the token in the header and nowhere
// else.
#pragma once

#include "interfaces/text_out.h"

namespace core {
namespace console {

class AgentLoopProbe {
 public:
  virtual ~AgentLoopProbe() = default;

  // One wake of the loop, now, with the uplink the console has already found up: the six steps
  // in order, each printed as it ends, then the report. False when the loop could not run at all
  // (not configured, halted on a 401), with the reason printed.
  virtual bool wake(interfaces::TextOut& out) = 0;

  // The last ten jobs, newest first: id, type, outcome, transactions, posted.
  virtual void jobs(interfaces::TextOut& out) = 0;

  // The loop's saved state - what the next wake restores - and what the sleep keeps beside it.
  virtual void state(interfaces::TextOut& out) = 0;
};

}  // namespace console
}  // namespace core
