// The supervisor as a `core::cycle::AttentionSource` (task p3-04).
//
// §8's EV_ATTENTION is decoded in exactly one place in this firmware - the supervisor, which is
// also what §8's "acknowledged by the next valid request" belongs to, and which keeps the last
// few attentions for "whoever runs the wake" (core/supervisor/supervisor.h, takeAttention()).
// This is the three lines that let a wake ask it, without the supervisor having to know that
// core/cycle exists.
#pragma once

#include "core/cycle/wake_reason.h"
#include "core/supervisor/supervisor.h"

namespace core {
namespace cycle {

// A pointer and not a reference, because a build can be without a supervisor (main/main.cpp
// hands over nothing when there is no store or no journal). `there()` is false then, and the
// caller hands the wake reason a nullptr rather than a source that would spend §8's whole repeat
// window listening to nothing.
class SupervisorAttentions final : public AttentionSource {
 public:
  explicit SupervisorAttentions(supervisor::Supervisor* supervisor = nullptr)
      : supervisor_(supervisor) {}

  bool there() const { return supervisor_ != nullptr; }

  void listen() override {
    if (supervisor_ != nullptr) supervisor_->listen();
  }

  bool take(Attention* out) override {
    if (supervisor_ == nullptr) return false;
    supervisor::Attention a;
    if (!supervisor_->takeAttention(&a)) return false;
    out->reason = a.reason;
    out->detail = a.detail;
    out->repeat = a.repeat;
    return true;
  }

 private:
  supervisor::Supervisor* supervisor_ = nullptr;
};

}  // namespace cycle
}  // namespace core
