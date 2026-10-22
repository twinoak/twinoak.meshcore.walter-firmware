// The console's one seam into the agent's uplink (task p4-03): `agent url` - the server this
// unit is configured for and the version header it sends - and `agent probe`, one
// `GET /agent/v1/config` now, for the bench's first look at a server (docs/bench.md §17). The
// uplink itself is components/agent/'s ModemUplink over phase 3's HTTPS client, and core/ cannot
// name it (agent/ is built on core/, not the other way round), so the console takes this
// interface and main/ hands it the implementation (components/agent/uplink_probe.h). A build
// without one answers in one line.
//
// What crosses it is the `Call` record's fields and the config's numbers and words, copied; the
// token crosses it in no field, and the error text is one sentence of static words and numbers
// (never a byte of a body). The console prints what it is given and nothing else.
#pragma once

#include <cstdint>

namespace core {
namespace console {

struct UplinkProbeResult {
  // The Call record (agent/IUplink.h) as the uplink filled it.
  bool reachable = false;
  bool ok = false;
  int status = 0;
  std::uint32_t retry_after_s = 0;
  int rate_limit_remaining = -1;
  char error[128] = {};
  std::uint32_t elapsed_ms = 0;
  std::uint32_t reply_bytes = 0;
  const char* via = "";
  const char* endpoint = "";  // the path under the URL: no query, no token

  // The config the server answered, when `ok` (mm::AgentConfig's fields).
  long long agent_id = 0;
  char label[64] = {};
  char transport[16] = {};
  int max_transactions_per_minute = 0;
  int min_gap_seconds = 0;
  int poll_interval_s = 0;
  char server_time[40] = {};
};

class UplinkProbe {
 public:
  virtual ~UplinkProbe() = default;
  // A URL and a token are set, so a call can be made at all.
  virtual bool configured() const = 0;
  virtual bool tokenSet() const = 0;
  // `server.url` as the uplink uses it (a trailing slash off); "" when unset.
  virtual const char* url() = 0;
  // What X-Agent-Version carries.
  virtual const char* version() const = 0;
  // One GET /agent/v1/config, now, blocking for as long as the transport's deadline allows.
  // Returns `out->ok`.
  virtual bool probe(UplinkProbeResult* out) = 0;
};

}  // namespace console
}  // namespace core
