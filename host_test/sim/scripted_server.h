// A scripted HTTPS server (task p3-02): what stands at the other end of host_test/sim/
// fake_tls_socket.h. It takes a request as the bytes the firmware's writer produced, parses it
// (so that what went over the wire is what is checked), and answers from a script - files by
// path with ranges that can be switched off, redirects, statuses with a Retry-After, a body one
// byte too long, a 206 with a wrong Content-Range, chunked bodies, bodies with no length, a raw
// response for the parser's own tests - and it remembers every request it saw, headers included,
// so that a test can ask whether an Authorization header reached a second host.
//
// Which root a host's chain "ends at" is part of the script too: the fake socket refuses a
// connection whose root is not the host's, the way the modem refuses a chain it cannot verify.
//
// One rule here is **not** scripted but held, because meshmaster.dk holds it: every `/agent/v1`
// request carries `X-Agent-Version`, and one that does not is a 400 (task p10-05;
// `kAgentVersionRequired` below).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/net/trust.h"

namespace sim {

struct ScriptedHost {
  std::string name;
  std::uint8_t root;  // which root of core/net/trust.h the chain ends at
};

struct ServedFile {
  std::vector<std::uint8_t> bytes;
  std::string etag;           // sent when not empty
  std::string last_modified;  // sent when not empty
  std::string content_type = "application/octet-stream";
  bool chunked = false;        // Transfer-Encoding: chunked, in chunks of `chunk_size`
  std::size_t chunk_size = 100;
  bool no_length = false;      // neither Content-Length nor chunked: the body ends with the connection
};

struct Redirect {
  int status = 302;
  std::string location;
};

struct StatusRule {
  int status = 500;
  std::string retry_after;  // sent when not empty
  std::string body;
};

struct SeenRequest {
  std::string method;
  std::string host;  // the Host header
  std::string path;
  std::map<std::string, std::string> headers;  // names lower-cased
  std::string body;
  bool has(const char* name) const;
  std::string header(const char* name) const;
};

// What a request hook answers with (task p4-03): a status, headers of its own, a body. Sent
// with a Content-Length and `Connection: close`, as the scripted answers are.
struct ScriptedReply {
  int status = 200;
  std::vector<std::string> headers;
  std::string body;
  // **This one request gets no answer at all** (task p13-02): the server takes it and says
  // nothing, so the call ends at its deadline with no status. `no_response` above is the same
  // thing for every request; this is for a script that wants one route to stall and the rest to
  // answer - *the heartbeat came back 503 and the claim never came back* is one wake, and that
  // is the wake a failed-call list has to be able to show.
  bool no_response = false;
};

// meshmaster.dk's agent API prefix, and the header every request under it carries
// (`src/MeshMaster.Web/Agents/AgentV1.cs`: `Prefix`, `VersionHeader`, read at commit
// d7b9b3ea9deb). `kMaxAgentVersion` is `AgentLimits.Version`
// (`src/MeshMaster.Core/Transport/Agents.cs`): a header longer than it reads as *not given*, as
// `VersionOf` does, with an absent one and an empty one.
constexpr const char* kAgentPrefix = "/agent/v1/";
constexpr const char* kAgentVersionHeader = "X-Agent-Version";
constexpr std::size_t kMaxAgentVersion = 64;

class ScriptedServer {
 public:
  ScriptedServer();

  // -- the script --------------------------------------------------------------------------
  std::vector<ScriptedHost> hosts;
  std::map<std::string, ServedFile> files;       // by path (the query is ignored for the lookup)
  std::map<std::string, Redirect> redirects;
  std::map<std::string, StatusRule> statuses;
  bool ranges = true;               // false: a Range header is ignored and the whole file is sent (200)
  bool wrong_content_range = false; // a 206 whose Content-Range begins one byte late
  int extra_bytes = 0;              // appended to every file body, Content-Length counting them
  bool honour_if_range = true;      // false: If-Range is ignored
  std::string raw_response;         // when set, sent verbatim to any request
  bool no_response = false;         // the server takes the request and says nothing (a stall)
  // **meshmaster.dk's rule, not a knob of the script** (task p10-05): a request to a path under
  // `kAgentPrefix` that does not carry `kAgentVersionHeader` is answered with the 400 of problem
  // details `AgentV1.RequireVersionHeaderAsync` answers, before the script is consulted at all.
  // It is on by default because it is on at the server, and the one request this firmware made
  // without the header - the download of a release, in every version up to v0.1.2 - passed every
  // host test there was. The server's `DownloadsWithoutAVersion` (its task p4.28-177) serves the
  // three download routes without the header for the units already in the field; that exemption
  // is deliberately **not** modelled here, because it is temporary - it goes the day no unit
  // reports a version older than 0.1.3 - and a fake that modelled it would let the same mistake
  // through again. `false` is for a test of this rule itself.
  bool agent_version_required = true;
  // How many requests this rule refused, so that a test can show it was not the thing that passed.
  int unversioned_refused = 0;
  // A server that decides per request (task p4-03: a scripted meshmaster.dk whose claim answer
  // depends on what was claimed before). Consulted after the request is recorded and after the
  // two knobs above, before the files, the redirects and the statuses; `true` sends `reply`,
  // `false` falls through to the script.
  std::function<bool(const SeenRequest& request, ScriptedReply* reply)> on_request;

  // -- what happened -----------------------------------------------------------------------
  std::vector<SeenRequest> requests;

  // Whether this host is scripted at all, and which root its chain ends at (false when unknown).
  bool rootOf(const std::string& host, std::uint8_t* root) const;

  // Serve a file: the simplest script. The same bytes under `path`.
  void serve(const std::string& path, const std::vector<std::uint8_t>& bytes,
             const std::string& etag = "", const std::string& last_modified = "");

  // The response to a request as it arrived, as the bytes to send back. Empty when `no_response`.
  std::vector<std::uint8_t> respond(const std::string& request_text);

 private:
  static std::string statusText(int status);
  static void append(std::vector<std::uint8_t>& out, const std::string& text);
  std::vector<std::uint8_t> response(int status, const std::vector<std::string>& headers,
                                     const std::uint8_t* body, std::size_t body_size,
                                     bool with_length);
  std::vector<std::uint8_t> chunkedResponse(int status, const std::vector<std::string>& headers,
                                            const std::uint8_t* body, std::size_t body_size,
                                            std::size_t chunk);
};

}  // namespace sim
