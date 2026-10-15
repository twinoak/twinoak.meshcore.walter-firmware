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

// The restore (task p4.1-60): `setperm <own key> <granted level>` as the last command of every
// admin session on a repeater where this exit node holds a grant - one more on every admin
// type through an `acl` pair, and on a blank login the repeater answered above the grant.
constexpr int kTransactionsForAclRestore = 1;

// What one run of `type` at `credentialClass` costs, without the login. `items` is what the job
// is as long as - the number of neighbours for `neighbours` - and -1 is what the system assumes
// when nobody says otherwise: one page.
//
// Two of these are not one: an `acl`-class `status` costs **two**, because the firmware version
// comes from a second, owner-info request and the association's verify step records the version
// it was proved on; and `neighbours` costs one transaction **per page** of nine, because the
// firmware answers a page at a time.
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
