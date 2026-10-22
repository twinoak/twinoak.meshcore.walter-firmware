#include "walter_agent/uplink_probe.h"

#include <cstdio>

namespace agent {

bool ModemUplinkProbe::probe(core::console::UplinkProbeResult* out) {
  if (out == nullptr) return false;
  *out = core::console::UplinkProbeResult{};
  const bool ok = uplink_.fetchConfig();
  const mm::IUplink::Call& c = uplink_.lastCall();
  out->reachable = c.reachable;
  out->ok = c.ok && ok;
  out->status = c.httpStatus;
  out->retry_after_s = c.retryAfterS;
  out->rate_limit_remaining = c.rateLimitRemaining;
  std::snprintf(out->error, sizeof out->error, "%s", c.error);
  out->elapsed_ms = uplink_.lastElapsedMs();
  out->reply_bytes = uplink_.lastReplyBytes();
  out->via = c.via;
  out->endpoint = uplink_.lastEndpoint();
  if (ok && uplink_.hasConfig()) {
    const mm::AgentConfig& cfg = uplink_.config();
    out->agent_id = cfg.agentId;
    std::snprintf(out->label, sizeof out->label, "%s", cfg.label);
    std::snprintf(out->transport, sizeof out->transport, "%s", cfg.transport);
    out->max_transactions_per_minute = cfg.budget.maxTransactionsPerMinute;
    out->min_gap_seconds = cfg.budget.minGapSeconds;
    out->poll_interval_s = cfg.pollIntervalS;
    std::snprintf(out->server_time, sizeof out->server_time, "%s", cfg.serverTime);
  }
  return out->ok;
}

}  // namespace agent
