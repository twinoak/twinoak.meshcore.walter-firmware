#include <agent/FloodScope.h>

#include <cstdio>
#include <cstring>

namespace mm {

const char kFloodScopeUnscoped[] = "*";

bool floodScopeIsUnscoped(const char* scope) {
  return scope != nullptr && std::strcmp(scope, kFloodScopeUnscoped) == 0;
}

FloodMode floodModeFor(int flood, UnscopedRetry retry) {
  if (retry == UnscopedRetry::Never) return FloodMode::Scoped;
  return flood >= 2 ? FloodMode::Unscoped : FloodMode::Scoped;
}

void FloodPolicy::begin() {
  if (depth_++ > 0) return;  // a nested retry loop floods inside the one already running
  floods_ = 0;
  lastFlooded_ = false;
  lastMode_ = FloodMode::Scoped;
  answered_ = false;
  answeredMode_ = FloodMode::Scoped;
}

FloodMode FloodPolicy::modeForNextFlood(const char* scope) const {
  if (floodScopeIsUnscoped(scope)) return FloodMode::Unscoped;
  return floodModeFor(floods_ + 1, retry_);
}

void FloodPolicy::sent(bool flooded, FloodMode mode) {
  lastFlooded_ = flooded;
  lastMode_ = mode;
  if (flooded) floods_++;
}

void FloodPolicy::answered() {
  if (!lastFlooded_) return;  // a direct send was answered: this measures floods
  answered_ = true;
  answeredMode_ = lastMode_;
}

void FloodPolicy::end() {
  if (depth_ > 0) depth_--;
  if (depth_ > 0 || floods_ == 0) return;
  if (!answered_) {
    counts_.unanswered++;
  } else if (answeredMode_ == FloodMode::Unscoped) {
    counts_.answeredUnscoped++;
  } else {
    counts_.answeredScoped++;
  }
  floods_ = 0;
  answered_ = false;
  lastFlooded_ = false;
}

void FloodPolicy::describeSend(bool direct, FloodMode mode, const char* scope, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return;
  if (direct) {
    std::snprintf(out, cap, "direct");
    return;
  }
  if (mode == FloodMode::Unscoped || scope == nullptr || floodScopeIsUnscoped(scope)) {
    std::snprintf(out, cap, "flood, unscoped");
    return;
  }
  std::snprintf(out, cap, "flood, scope %s", scope);
}

}  // namespace mm
