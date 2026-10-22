# 04 — The unit checks in

**Depends on:** 03; meshmaster.dk pile 4.16 deployed; in `bench.local.json` `"sim": true` and `"server": true` — which mean that Thomas has fitted the SIM, registered the unit at meshmaster.dk (*New exit node*, transport `native`, **no repeater**) and typed `lte.apn`, `lte.pin`, `server.url` and `server.token` on the console himself.
**Spec:** `docs/bench.md` §12 *The modem*, §13 *HTTPS*, §14 *Provisioning and the console*, and the agent's sections phase 4 task 06 added (*The agent*, *A day as an exit node* — numbered after §16 *A day*); `docs/provisioning.md` (the session, and the token step phase 4 added); `docs/field.md` *What one wake does*; `docs/cycle.md` (the ten steps of a wake); meshmaster.dk at its deployed commit: `docs/dev/unit-protocol.md` (the heartbeat, the claim, what the server answers), `docs/ops/exit-nodes.md` §15 (a unit registered before its repeater, pile 4.16 task 132), `tests/fixtures/contracts/AgentHeartbeat.json`; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) §8 (*the unit before its repeater*).
**Size:** one session, long.
**Model:** claude-opus-5

**Stop before the first line** without `"sim": true` and `"server": true` in `bench.local.json`.

## Goal

The unit on the server, for the first time: the modem attaches, the clock is set from the network,
TLS to meshmaster.dk is verified against the root this build carries, and the agent's first real wake
sends a heartbeat that carries the radio's key — the key task 03's first boot made. That heartbeat is
what lets Thomas register the repeater from the exit node's page and link the two in one click, and
it is the last thing between this unit and an update over LTE.

## Deliverables

- **`bench walk 12`** — the modem: `lte` before, `lte up` (the cold attach time, from the command to
  *registered*; the band and operator the modem reports), `lte time` (the network's time against this
  machine's clock), `lte id` (masked as the console masks it), `lte down`. A refused SIM PIN is a stop,
  never a retry: the PIN has three tries and the console already refuses to resend a refused one
  (phase 3's follow-up about where that flag lives is a phase 4 item; if it is not yet fixed, say so
  in the report and do not power-cycle the Walter while the modem holds a refused PIN).
- **`bench walk 13`** — HTTPS: `trust tls` to `server.url`'s host (verified chain, the root's name),
  `http get` of `server.url` (a status line, no body printed), and the refusal of a host whose chain
  this build cannot verify if `docs/bench.md` §13 names one (a test host is not invented here).
- **`bench walk 14`** — the console's provisioning lines that are not secrets: `get` shows `lte.pin`
  and `server.token` *set*, `server.url` as typed, `unit.name`; the refusals `docs/provisioning.md`
  lists (each refused by the console, not by the tool — the tool refuses before them only lines that
  would *set* a secret, which none of these are).
- **The agent** (*The agent*'s lines, phase 4 task 06): `agent id` — the radio's key from `IDENTITY`, the
  repeater firmware's version from `HELLO`, `transport native`, the job types; `agent heartbeat` (the
  body, printed) held against meshmaster.dk's `AgentHeartbeat.json` field by field (present, type,
  plausible value — not bytes: the values are this unit's); `agent probe` (the config call, its
  status); then **one real wake**: `wake.interval` set short for the walk (recorded and restored by
  `bench record`/`restore`), `cycle now`, the USB port gone while the Walter sleeps and back with the
  same serial number (01's wait), and `status`/`journal` after it: the heartbeat's HTTP status, what
  the server answered (`nextCheckInS` accepted or changed, jobs waiting or not), observations posted,
  the time awake, the bytes sent and received if the modem reports them. Two wakes, so the second is a
  warm attach; both times into *What to bring back*.
- **A unit with no radio key** (phase 4's follow-up `[04]`): not walked here — `rak role scanner`
  would cost task 03's identity. Name in the report the host test of phase 4 that holds the keyless
  heartbeat against meshmaster.dk's `AgentHeartbeatNoKey.json` (pile 4.16, task 132). On the rig the
  radio has its key; that is the case Thomas needs.
- **Thomas's next lines**, in the report and in `docs/bench.md` *The agent* if task 06 of phase 4
  did not write them: on meshmaster.dk, the exit node's page now shows *asleep until …* and the radio's key
  with **Register this unit's repeater**; that button, then the repeater's page shows the unit as its
  uplink — the link made at that moment, not at the next check-in (pile 4.16, task 132). The session
  cannot see the server's pages (it holds no user credential); it says what the unit sent and what
  the server answered, and Thomas confirms the rest.
- `bench walk 12|13|14 --tick` and the agent's lines ticked; result files under `out/bench/`.

## Acceptance criteria

- [ ] The modem attached, the clock was set, TLS verified, and the cold and warm attach times are in *What to bring back*.
- [ ] Two wakes with the server: each heartbeat answered 2xx, each wake ended in sleep, and the Walter came back on USB by its serial number.
- [ ] The heartbeat carried the radio's key and `transport native` (from the Walter's own log of the wake, in the report).
- [ ] No secret value appears in the report, the result files or any log (the tool's canary check, run over `out/bench/`).
- [ ] `tools/verify.ps1` green.

## Not verified here (Thomas)

The exit node's page and the repeater registered from the key (pile 4.16). The update over LTE:
`twinoak.fw.offer` and `.apply` started by a person on meshmaster.dk, with a release signed by the
**real** key — the hardware repository's `software/FIRST-UNIT.md`, last part. Sleep current on a
meter (*Sleep* ★).

## Out of scope

Jobs for the unit's own repeater or a neighbour (Thomas starts them from meshmaster.dk once the
repeater is registered and linked). The Walter's own update over LTE.
