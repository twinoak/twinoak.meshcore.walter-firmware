// What one wake spends at the server, counted in one place (task p1.6-21).
//
// A wake has bounds (agent/AgentLoop.h's `Budget`): the seconds of the whole wake, the seconds
// its calls to meshmaster.dk may take together, and the bytes of body it may post on a plan that
// is priced by the byte. Two objects make those calls, though - the loop (the config, the
// heartbeat, the observation batches) and the runner (the claim, and every result a job posts) -
// and before this task the runner's were counted by neither: a result `wakeResults` had kept
// back for the byte cap was flushed with the whole offline buffer by the claim's first job, in
// neither `report.bytesPosted` nor the uplink's seconds. On an LTE plan one claimed job could
// post a day's kept results.
//
// So the ledger is one object both of them hold, and **every call either of them makes goes
// through `spend()`**: it reads the clock either side of the call and weighs the body that went
// through. Nothing else times a call and nothing else counts a byte, which is what makes the
// rule checkable rather than remembered - `test/test_loop`'s
// `EveryCallToTheServerInTheLibraryIsMadeThroughTheMeter` reads `IUplink.h` for the list of
// calls there are and both sources for where they are made, so a call added later and counted
// nowhere is a red build.
//
// Two things it does not do. It does not **refuse** a call: what a bound means for a step is the
// step's own business - a batch that will not fit is shortened, a result that will not fit is
// kept behind the queue, a job that will not fit is never put on the air - so the callers ask
// (`mayPost`, `timeLeft`) and the ledger answers. And it does not know the transaction cap: that
// is the intent's `maxTransactionsThisWake`, counted by the airtime gate on the way to the radio
// (agent/Airtime.h) and not here.
//
// `loop()` uses the same ledger with no bounds set, so the Heltec runs the path the Walter's
// wake runs: a bound of 0 is "no bound", as everywhere else in the Budget.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/IPlatform.h>

namespace mm {

// "No byte cap": what `loop()` posts with, and what a `Budget` of 0 bytes means.
constexpr size_t kNoByteCap = static_cast<size_t>(-1);

class UplinkMeter {
 public:
  // The clock every bound is read off: a test's fake clock is the budget (support/FakePlatform.h).
  void begin(IPlatform& platform) { platform_ = &platform; }

  // The bounds of one wake, and the counters it fills. The three are the `Budget`'s own, and 0
  // is "no bound" in each of them.
  void beginWake(uint32_t wakeSeconds, uint32_t uplinkSeconds, size_t postBytes) {
    wakeMs_ = wakeSeconds * 1000u;
    uplinkMs_ = uplinkSeconds * 1000u;
    bytesLeft_ = postBytes == 0 ? kNoByteCap : postBytes;
    startedAtMs_ = now();
    uplinkSpentMs_ = 0;
    bytesPosted_ = 0;
  }

  // Back to no bounds: what `loop()` runs with, and what a wake leaves behind it. What was spent
  // stays readable until the next `beginWake`, because the report is filled after this.
  void endWake() {
    wakeMs_ = 0;
    uplinkMs_ = 0;
    bytesLeft_ = kNoByteCap;
  }

  // What is left of the wake's seconds, and the whole of a uint32 when it set none.
  uint32_t wakeMsLeft() const {
    if (wakeMs_ == 0) return 0xFFFFFFFFu;
    const uint32_t spent = now() - startedAtMs_;
    return spent >= wakeMs_ ? 0 : wakeMs_ - spent;
  }

  // The wake's seconds and the uplink's together: false when either has run out, which is when a
  // wake stops calling the server and keeps what is left for the next one.
  bool timeLeft() const {
    if (wakeMsLeft() == 0) return false;
    return uplinkMs_ == 0 || uplinkSpentMs_ < uplinkMs_;
  }

  size_t bytesLeft() const { return bytesLeft_; }

  // Whether a body of `len` bytes may go out at all: there is time for the call and there are
  // bytes for the body. What may not go is **kept**, never dropped and never posted out of turn.
  bool mayPost(size_t len) const { return timeLeft() && len <= bytesLeft_; }

  // What this wake has spent: the server calls together, and the bytes of body that went
  // through. Both are in the WakeReport.
  uint32_t uplinkSpentMs() const { return uplinkSpentMs_; }
  size_t bytesPosted() const { return bytesPosted_; }

  // The one place a call to the server is made from. `bodyBytes` is what goes on the wire of the
  // library's own making - a result, an observation batch, a progress report - and 0 for a call
  // whose body the uplink writes itself (the config, the heartbeat, the claim: a struct and an
  // integer, and the bytes are the implementation's, not countable from here).
  template <typename Call>
  bool spend(size_t bodyBytes, Call call) {
    const uint32_t at = now();
    const bool ok = call();
    uplinkSpentMs_ += now() - at;
    if (!ok) return false;
    bytesPosted_ += bodyBytes;
    if (bytesLeft_ != kNoByteCap) bytesLeft_ = bytesLeft_ > bodyBytes ? bytesLeft_ - bodyBytes : 0;
    return true;
  }

 private:
  uint32_t now() const { return platform_ == nullptr ? 0 : platform_->millis(); }

  IPlatform* platform_ = nullptr;
  uint32_t wakeMs_ = 0;        // 0: no bound
  uint32_t uplinkMs_ = 0;      // 0: no bound
  uint32_t startedAtMs_ = 0;
  uint32_t uplinkSpentMs_ = 0;
  size_t bytesLeft_ = kNoByteCap;
  size_t bytesPosted_ = 0;
};

}  // namespace mm
