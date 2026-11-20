// The airtime budget at the point of transmission (EXIT-NODES §5, §8).
//
// One transaction at a time, ever; at least `minGapSeconds` between the starts of two; at most
// `maxTransactionsPerMinute` started in any sixty seconds. A burst of claimed jobs is executed at
// this pace and never refused - a read that would exceed the budget waits. The budget is the
// server's, heard on every heartbeat, and may be tightened here and never loosened.
//
// The arithmetic is the .NET agent's `Throttle` (src/MeshMaster.Agent/Budget.cs): an exact
// sliding window of the last starts, and the longer of "the gap since the last start" and "until
// the oldest start in the window is a minute old". `transactionsFor()` beside it is
// `TransactionCosts.For` (src/MeshMaster.Core/Transport/PlannedLoad.cs), which is the table of
// EXIT-NODES §7 as code and what the server's planned load prices a schedule with - so what this
// board spends and what the page promised are the same number.
//
// Host-portable: millisecond counts in, nothing else.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/ConfigRead.h>
#include <agent/ConfigReply.h>
#include <agent/ReplyCodec.h>

namespace mm {

// The login, when there is no session for this repeater yet.
constexpr int kTransactionsForLogin = 1;

// **A line that cannot be asked twice, and the check that confirms it** (task p1.10-48).
// `setperm <own key> <level>` is sent **once** - its retry cannot be answered, because if the
// line ran it has moved this key off admin and the repeater drops a console line from anybody
// below it - and its silence is read back with a blank login. So such a line is **two**
// transactions where EXIT-NODES §7's row is one: the line, and the check it needs when the line
// is not answered.
//
// A check answered at admin means the line never arrived, and then the line goes once more and
// is checked once more - two rounds at most, four transactions. That second round is as far
// beyond this estimate as the login's own second try has always been (`kTransactionsForLogin`
// is one and `loginSequence` tries twice, with a blank probe between them on one path), and
// what bounds both is the lease and the gate rather than the estimate. Counting it here instead
// would price every ordinary probe through a pair with a grant at six transactions - a wake
// slice twice what such a job has ever needed - because the restore below is counted wherever
// there is a grant, and the daily status and neighbour reads are what that would refuse.
constexpr int kTransactionsForOwnEntryWrite = 2;

// The restore (task p4.1-60): `setperm <own key> <granted level>` as the last command of every
// admin session on a repeater where this exit node holds a grant - one more on every admin
// type through an `acl` pair, and on a blank login the repeater answered above the grant. Since
// task p1.10-48 that line is checked when it is not answered, so it is the two above; what it
// costs on a repeater that answers the line is still the one §7 prices.
//
// **Or the removal that stands in for it** (task p1.10-50, meshmaster.dk's p4.33-191): a claim
// that says level **0** ends the session with `setperm <own key> 0` - take this exit node's key
// off - and that is the same one line at the same price. So this is now also what an admin job
// through a pair that holds **no** grant of ours costs: a guest association's daily configuration
// read above all. `transactionsOf` asks whether the claim carried a level and not what the level
// was, so it has always priced both.
constexpr int kTransactionsForAclRestore = kTransactionsForOwnEntryWrite;

// What one run of `type` at `credentialClass` costs, without the login. `items` is what the job
// is as long as - the number of neighbours for `neighbours` - and -1 is what the system assumes
// when nobody says otherwise: one page.
//
// Two of these are not one: a `status` read costs **two**, because the owner-info reply comes
// from a second request; and `neighbours` costs one transaction **per page** of nine, because the
// firmware answers a page at a time.
// `credentialClass` no longer decides what a `status` read costs (meshmaster.dk's task
// p4.30-181 took the class parameter off `TransactionCosts.StatusRequests` for the same reason):
// the owner-info request is made at every class. It is still in the signature because the table
// of EXIT-NODES §7 is a table of (type, class) and every other row reads it.
int transactionsFor(const char* type, CredentialClass credentialClass, int items = -1);

// Requests for a neighbour list of `neighbours` entries; unknown or none is one.
int neighbourPages(int neighbours);

class AirtimeGate {
 public:
  // Sixty a minute is far past anything a budget will ever say (the default is two), and the
  // window only ever holds a minute of starts.
  static constexpr size_t kMaxTracked = 60;

  // AgentBudget has member initialisers, so it is not an aggregate under gnu++11 and cannot be
  // brace-initialised here: the conservative default of §8 is set in the constructor instead.
  AirtimeGate() {
    budget_.maxTransactionsPerMinute = kDefaultMaxTransactionsPerMinute;
    budget_.minGapSeconds = kDefaultMinGapSeconds;
  }

  void setBudget(const AgentBudget& budget) { budget_ = budget; }
  const AgentBudget& budget() const { return budget_; }

  // 0 when a transaction may start now, else how long until one may.
  uint32_t waitMs(uint32_t nowMs) const;

  // Record a transaction that is about to go on the air.
  void took(uint32_t nowMs);

  uint32_t transmissions() const { return transmissions_; }
  uint32_t totalWaitMs() const { return totalWaitMs_; }
  void addWait(uint32_t ms) { totalWaitMs_ += ms; }

  // How many starts are inside the last minute.
  size_t inWindow(uint32_t nowMs) const;

 private:
  AgentBudget budget_;
  uint32_t starts_[kMaxTracked] = {};
  size_t first_ = 0;
  size_t count_ = 0;
  bool hasLast_ = false;
  uint32_t last_ = 0;
  uint32_t transmissions_ = 0;
  uint32_t totalWaitMs_ = 0;
};

}  // namespace mm
