# 05 — The fetch says who it is

**Depends on:** nothing in this phase.
**Spec:** meshmaster.dk, read only: `src/MeshMaster.Web/Agents/AgentV1.cs` (`RequireVersionHeaderAsync`: every `/agent/v1` request without `X-Agent-Version` is a 400), `spec/API.md` §1 and §3, and its `tasks/phase-4.28/177-a-download-needs-no-version.md` (the server's half). This firmware: `components/core/update/fetch.cpp` (the manifest, the signature and the ranges: `net::Options` with `authorization` and no header of its own), `components/core/include/core/net/client.h` (`Options::headers`, `kMaxRequestHeaders` = 4, the `Range` and `If-Range` the ranges already take), `components/core/net/client.cpp` (*a status this firmware does not act on*), `components/agent/modem_uplink.cpp` (where the agent's calls set `X-Agent-Version`), `components/agent/firmware_jobs.cpp` (`runOffer`: the `reason` it posts), `core/update/uplink_check.h` and `tls_check.cpp` (the trial's third condition); `docs/flashing.md` (*From 0.1.1 to 0.1.2, over LTE*), `docs/update.md`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

8 November, the first update of a unit over LTE. Job 22841, the offer of the Walter's v0.1.2, failed
in two seconds with *a status this firmware does not act on*. meshmaster.dk refuses any `/agent/v1`
request without `X-Agent-Version` with a 400. The agent's calls carry the header (`modem_uplink.cpp`),
but the release download never has, in any version. So **no release of either box could ever be
fetched over LTE**. The pile 9 report that wrote the LTE walk read the code and missed it, and so did
every host test, because the fake server takes any request.

The server's half (meshmaster.dk 177) lets the units already in the field download without it. This
task is the firmware's half, and the reason it went unseen.

## Deliverables

1. **Every request to `server.url` says who it is.**
   - The download's manifest, signature and every range carry `X-Agent-Version` (`core::version()`),
     within `kMaxRequestHeaders` beside `Range` and `If-Range`.
   - Find every request this firmware makes to `server.url`, from any path, and list it in the report
     with the headers it sends. That covers the agent's calls, the download, and the trial's third
     condition if it is a request and not only a handshake. None goes without the header.
   - A download from a host that is not `server.url` (`fetch` from the console) carries no token, as
     now. Whether it carries the header is yours to decide; say which and why.
2. **A refusal says what it was.** Replace *a status this firmware does not act on* with the status
   and the request: *http 400 for manifest.json*. When the server sends a problem-details body, add
   its `detail`, cut to fit. That goes in the offer's posted `reason`, the console's `fetch` and
   `http get` lines, and the job log. A token is never part of it.
3. **The fake server holds the real rule.** The host tests' fake for `/agent/v1` refuses a request
   without `X-Agent-Version` with meshmaster.dk's 400, exactly as `AgentV1.cs` does, so this kind of
   mismatch cannot pass a host test again. Name the server file and commit the rule was read from.
4. **Tests:** a download of each product through the fake that refuses unversioned requests; the new
   refusal words for a 400, a 404 and a 416; the trial's third condition unchanged.
5. **Docs.** `docs/flashing.md`'s LTE walk gets a dated correction: before 0.1.3 the download needs
   meshmaster.dk with 177 deployed, and until then the cable is the way. `docs/update.md` says which
   headers a download sends.

## Acceptance criteria

- [ ] Every request to `server.url` carries `X-Agent-Version`, listed in the report; the download of both products passes a fake that refuses without it (tests).
- [ ] A refused request names its status and its file; no token in any message (tests).
- [ ] The LTE walk corrected; `tools/verify.ps1` green.

## Out of scope

meshmaster.dk (its pile 4.28). Making the Walter's own download resume (04 leaves that open).
