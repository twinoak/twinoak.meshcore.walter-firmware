// Which scope a flood goes out in, and the counts that say whether the unscoped try still earns
// its airtime (meshmaster.dk's D-47, its task p4.20-151; this repository's task p1.8-32).
//
// A flood is the login to a repeater this exit node holds no path for, path discovery, and the
// `advert` a job asks for. Direct-routed traffic - most of what MeshMaster sends - is not
// affected, because a repeater only checks the scope of a flood.
//
// A stock repeater forwards an **unscoped** flood only while its wildcard region allows flood and
// only for `flood.max.unscoped` hops (15 under the Danish standard today; the firmware's own
// comment in `sendFloodReply` names the end state, *repeaters running `flood.max.unscoped=0`*,
// where such a flood dies at the first hop). A **scoped** flood is forwarded only by a repeater
// whose region map holds the scope with flood allowed - `dk` on every repeater that follows the
// Danish standard, and *not* on a repeater with no region map at all, which would have forwarded
// the unscoped one. So: scope by default, and **one unscoped try while unscoped still works**.
//
// The rule lives here, on a host, so that every branch of it is walked natively
// (test/test_flood_scope): `floodModeFor` is the whole decision, `FloodPolicy` is the counting,
// and the board supplies the key and the transport code (src/mesh/TransportScope.h) and the send
// (src/mesh/AgentMesh.cpp). Nothing here hashes anything or knows what a transport code is.
//
// **A count is per flooded transaction and never per send.** A transaction is one retry loop of
// the runner's - a login's two tries with the blank login between them, a request and its retry,
// a discover's flooded login and the path-discovery request after it - and it moves exactly one
// of the three counts: the scoped flood was answered, the unscoped one was, or neither. A
// transaction that never flooded moves none of them, which is every transaction of an exit node
// whose repeaters all have paths.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// What one flood goes out as. A direct send ignores it: a path is a path, whatever the scope.
enum class FloodMode : uint8_t { Scoped, Unscoped };

// `flood.unscoped` on the console: whether the **second** flood of a transaction drops the scope.
// `Never` is for the day unscoped floods stop being forwarded at all.
enum class UnscopedRetry : uint8_t { Retry, Never };

// The one word `flood.scope` may carry that is not a region's name: every flood goes out with no
// transport code at all. The same word the contract has (`AgentFloodScopes.Unscoped`).
extern const char kFloodScopeUnscoped[];

// Is this the scope that is no scope? `nullptr` is not: that is a mesh which says nothing about
// scope at all, and whatever its radio holds decides.
bool floodScopeIsUnscoped(const char* scope);

// The rule, in one place: the **first** flood of a transaction is scoped and the second and after
// are unscoped, while `retry` allows it. `flood` is 1 for the first flood of the transaction.
FloodMode floodModeFor(int flood, UnscopedRetry retry);

// The three counts `status` prints and the heartbeat's `flood` object carries, per flooded
// transaction since boot.
struct FloodCounts {
  uint32_t answeredScoped = 0;    // the transaction's scoped flood was answered
  uint32_t answeredUnscoped = 0;  // its unscoped flood was - the number the retry is judged by
  uint32_t unanswered = 0;        // it flooded and nothing came back, in either scope (`none` on `status`)
};

// The setting and the counting, over one transaction at a time.
//
// `begin()`/`end()` are **nestable**: a request that lost its session logs in again inside its own
// retry loop, and that login's floods belong to the transaction already running rather than to one
// of their own. Only the outermost pair counts.
//
// The one case worth naming: a transaction that flooded and whose answer came back to a *direct*
// send (the flood was silent, a re-login taught a path, the next attempt went direct and was
// answered) counts as `unanswered` - which is true of the flood, and the flood is what this
// measures.
class FloodPolicy {
 public:
  void setRetry(UnscopedRetry retry) { retry_ = retry; }
  UnscopedRetry retry() const { return retry_; }

  // What the heartbeat's `unscopedRetry` says: the setting is on **and** there is a scope to drop
  // for one send. A node that floods unscoped already has nothing to drop, and the contract asks
  // for `false` there (`AgentFlood.UnscopedRetry`).
  bool unscopedRetryFor(const char* scope) const {
    return retry_ == UnscopedRetry::Retry && scope != nullptr && !floodScopeIsUnscoped(scope);
  }

  const FloodCounts& counts() const { return counts_; }

  // A transaction begins. Nestable; the floods of a nested one belong to the outer.
  void begin();
  // The mode the next flood of this transaction goes out in, given the mesh's scope: always
  // `Unscoped` when the scope is `*` (there is nothing to drop and nothing to add), else
  // `floodModeFor`.
  FloodMode modeForNextFlood(const char* scope) const;
  // What the send came to. `flooded` false is a direct send: not counted, and it does not advance
  // the transaction's flood number.
  void sent(bool flooded, FloodMode mode);
  // A reply came back to the last send.
  void answered();
  // The transaction is over: one count moves if it ever flooded.
  void end();

  // The words a job's log line prints for a send, beside what it came to: `direct`,
  // `flood, scope dk` or `flood, unscoped`.
  static void describeSend(bool direct, FloodMode mode, const char* scope, char* out, size_t cap);

 private:
  UnscopedRetry retry_ = UnscopedRetry::Retry;
  FloodCounts counts_;

  int depth_ = 0;            // nesting; the outermost pair is the transaction
  int floods_ = 0;           // floods made in this transaction, counted from one
  bool lastFlooded_ = false; // the last send went out as a flood
  FloodMode lastMode_ = FloodMode::Scoped;
  bool answered_ = false;    // a flood of this transaction was answered
  FloodMode answeredMode_ = FloodMode::Scoped;
};

}  // namespace mm
