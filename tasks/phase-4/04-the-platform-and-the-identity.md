# 04 — The platform and the identity

**Depends on:** 02, 03.
**Spec:** the exit-node firmware at the pinned commit: `docs/library.md` (*IPlatform*, *the lifecycle*, `HeartbeatIdentity`/`WakeIntent`), `lib/agent-core/include/agent/{IPlatform,AgentLoop}.h`, `Observations.h` (`ObservationRing`, `HeardAdvert`), `Envelopes.cpp` (`writeHeartbeat`, what each field means); meshmaster.dk at today's `master`: `spec/EXIT-NODES.md` §5 (*Self-description*), §6 (*as built* for a sleeping agent, pile 4.14's 120), `tests/fixtures/contracts/AgentHeartbeat.json`; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.1 (`HELLO`: `fw_version`, role, board, link version), §5.6 (`IDENTITY`), §5.7 (`CLI` for the radio's settings), §7 (`OBS_PULL`: the record and its fields), §8 (attention reason 6, a fault the node wants reported); this repository's `components/hal/` (the clock, random, the log sink, retained storage), `components/core/cycle/TelemetryRing` and the log ring of phase 3, `components/core/power/IntervalPolicy`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The library needs a platform — time, randomness, a log, a place to write a
heap figure — and an identity to put in its heartbeat: who this exit node is
(the repeater's own key), what it runs (both versions), what its radio is set
to, what jobs it offers. Every one of those comes from the radio over the
link or from this firmware's own tables, none is typed by a person, and the
heartbeat that results is the fixture's shape to the byte.

## Deliverables

- **`components/agent/WalterPlatform : IPlatform`**: `millis()` from the hal
  clock; `delayMs()` as a *yield* that pumps the link client and the
  console, never a busy spin (the library idles through it); `random()` from
  the hal's hardware source; the log sink → this firmware's log ring, **with
  the library's redaction preserved** (the library never hands a reply body
  or a password to the sink; assert with the capture that nothing of
  `FakeMesh`'s payloads appears); `now()`/`hasTime()` from phase 3's
  network time and the clock keeper (the server's time from the heartbeat is
  taken by the library through `takeServerTime` — make sure the Walter's
  clock keeper is told too, so the radio's `TIME_SET` of the next wake has
  the better source); `freeHeap()`/`minFreeHeap()` from ESP-IDF through the
  hal; `rebootForHealth(why)` **journals and does not reboot** — the
  supervisor and the wake cycle own restarts.
- **The identity** (`components/agent/Identity`), filled from the radio once
  per boot and refreshed when `boot_id` changes:
  - `companionPublicKey` ← `IDENTITY` (the repeater's key: this exit node
    *is* its repeater, SYSTEM S-1);
  - `companionFirmware` ← `HELLO`'s `fw_version` (the repeater firmware's
    version string, as its heartbeat row expects — read what the repeater
    firmware's `ver` reports and what meshmaster.dk's `FirmwareVersion.Parse`
    accepts);
  - `agentVersion` ← this firmware's version; `board` ← `walter`;
    `transport` ← `native` (meshmaster.dk `AgentTransports`);
  - the radio block (frequency, bandwidth, SF, CR) ← the repeater's settings
    over the link's `CLI` (`get freq`, … — the exact lines from the
    repeater's `docs/console.md`), parsed, cached, re-read after a restart
    or a `config.apply` on the own repeater;
  - `jobTypes` ← the thirteen core types the library handles **plus** the
    extension's (05's four `twinoak.*`), minus nothing: the server routes by
    this list and the local-door table of 02 says which of the core types
    the unit answers `unsupported` *for its own repeater* — that is the
    runner's answer per job, not the list's business; say so in the report
    and in ARCHITECTURE §13, because it is the one place the server could
    be surprised (a `status` on the own repeater goes to the unit and comes
    back `unsupported`; 4.14's routing then tries the next candidate — check
    that it does, in meshmaster.dk's `EligibleAsync`, and if it does not,
    that is a finding for its pile);
  - `budget` as the server last set it (the library keeps it).
- **The intent** (`WakeIntent`): `nextCheckInS` ← the interval policy's
  next interval for the current band (phase 3, task 04), in seconds;
  `maxTransactionsThisWake` ← a per-band constant table (plenty: the
  library's claim max × the type's largest cost; normal: half; saving: one
  job's worth; critical: zero — the uplink is not used in `critical` at all,
  so the intent is moot there). Named constants, *to be set on the bench*.
- **Observations** (`components/agent/Observations`): `OBS_PULL` drained at
  each wake (§7) into the library's `ObservationRing` as `HeardAdvert`s,
  field by field (the record's key, path, hops, SNR, RSSI, name, role,
  position flags — map each; what the link has and the library's struct
  lacks, or the reverse, is a gap for the report), **except records whose
  key is this unit's own** (meshmaster.dk's 119 ignores them anyway; do not
  spend bytes sending them). The RAK's `obs_dropped` from `MESH_COUNTERS`
  goes to the heartbeat's error counters under the name the reference
  agent uses for its ring's drops.
- **Attention from the agent's side**: a `401` from the server (03) and a
  `rebootForHealth` request each raise the Walter's own attention for the
  next wake's record and the console's `status` (phase 3's wake record gains
  a letter for *agent*); nothing wakes the radio for it.
- **The console**: `agent id` (the identity as it would be sent, the token
  as *set/unset*), `agent heartbeat` (build the heartbeat body and print it —
  the bench's way to compare with the fixture by eye), `agent obs` (the ring:
  count, dropped, the last five).
- **Tests (host)** with the scripted radio and the copied fakes: the
  heartbeat body built from a scripted `HELLO`/`IDENTITY`/`CLI` set equals
  the fixture's for a `native` agent proposing an interval (regenerate
  nothing: the fixture is meshmaster.dk's; if it has no such example, build
  the expected bytes from the fixture of a plain heartbeat plus the two
  fields and say so); a `boot_id` change refreshes the identity; the radio
  block re-read after a local `config.apply`; `OBS_PULL` records land in
  the ring field for field, own-key records dropped, `obs_dropped`
  reported; the log capture holds no payload; `rebootForHealth` journals;
  the intent follows the band.

## Acceptance criteria

- [ ] The heartbeat body equals the fixture's bytes for the scripted identity (test), and `agent heartbeat` prints the same.
- [ ] Every field of the identity has its source in a table in the report (link message or firmware table), none typed.
- [ ] The observation mapping table is in the report with its gaps, if any.
- [ ] `rebootForHealth` never restarts the Walter (test); the log sink never carries a payload (test).
- [ ] `tools/verify.ps1` green.

## Not verified here (Thomas, with the rig and a SIM)

Add to `docs/bench.md`, "15. The agent": `agent id` shows the RAK's key and
version; `agent heartbeat` shows the radio block the RAK's console agrees
with; after `rak role scanner`, `agent id` says the identity is unknown and
no heartbeat is built.

## Out of scope

Sending any of it (05). The server's handling of the fields (meshmaster.dk).
