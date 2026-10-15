// Job types compiled into the agent beside the thirteen (task p1.5-15): the seam PLUGINS §3 of
// twinoak/meshmaster.dk describes as *handlers compiled into the agent*.
//
// The runner owns everything around a job - the claim, the lease check, the pacing against the
// airtime budget, the result envelope and its log, the post - and the extension owns the job's
// own exchange: it writes the result's `data` object and names the outcome. A claimed job whose
// type is none of the thirteen is offered to the extension (JobRunner::setExtension) before it
// is answered `unsupported`, and the heartbeat advertises the extension's types after the
// thirteen, so the server routes them here at all (routing requires the type advertised). The
// Heltec sets no extension, and its heartbeat is unchanged.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/IMesh.h>
#include <agent/IPlatform.h>
#include <agent/JobEnvelopes.h>

namespace mm {

// How many types an extension may add to the heartbeat's list.
constexpr int kMaxExtensionTypes = 8;

// What the runner lends an extension for the length of one job.
class JobContext {
 public:
  virtual ~JobContext() = default;
  // The mesh, with the job begun on it: the peer is set and the path the server knew is in
  // place, so a send goes to the job's repeater.
  virtual IMesh& mesh() = 0;
  virtual IPlatform& platform() = 0;
  // The budget's turn to speak, and the lease's: waits for the airtime budget, counts one
  // transaction, and is false when the lease ran out first - then nothing more goes on the air
  // and the outcome is `timeout`.
  virtual bool spend() = 0;
  // How long to wait for a reply on this job's path, by the claim's timeouts and the hop count.
  virtual uint32_t replyWaitMs() const = 0;
  // One line into the job's log, which is the result's `log`. Never a password.
  virtual void log(const char* line) = 0;
};

// Where an extension puts what it found.
class ResultWriter {
 public:
  virtual ~ResultWriter() = default;
  // The result's `data` object, as JSON already written. False when it does not fit.
  virtual bool data(const char* json, size_t len) = 0;
  virtual void outcome(Outcome o) = 0;
};

class IJobExtension {
 public:
  virtual ~IJobExtension() = default;
  // The types this extension handles, for the heartbeat: at most kMaxExtensionTypes, each a
  // static string that outlives the extension.
  virtual int typeCount() const = 0;
  virtual const char* type(int i) const = 0;
  // Run a claimed job of a type that is none of the thirteen. True when the extension ran it
  // (and set the outcome); false when it declines it, which the runner reports `unsupported`.
  virtual bool run(const ClaimedJob& job, JobContext& ctx, ResultWriter& out) = 0;
};

}  // namespace mm
