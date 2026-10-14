# 06 — The rails and the bench, again

**Depends on:** 05.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-12 (*the rails of the agent firmware hold here*), S-13; [`CLAUDE.md`](../../CLAUDE.md) (its last paragraph); `tasks/phase-3/06-the-rails-and-the-bench.md` and its report (the rails table and `check_rails.py`, `docs/bench.md` as one walk, `docs/field.md`, the bench-set constants table); the exit-node firmware's `docs/library.md` (*the rails the consumer inherits and must keep*, each with its test there) and `README.md` (*Rails that hold in every task*); every report of this phase.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Phase 3's last task made every promise of this firmware a test or a grep.
Phase 4 added an agent with promises of its own, inherited from the firmware
the library came from: nothing the mesh says is logged as text, a password
is in RAM for the exchange, one packet per command, TLS verified, a token
printed by nothing. This task holds each of them **over the link and the
modem** — where the exit-node firmware's own tests cannot see — folds the
new bench items into the one walk, and makes the README say what the unit
now is.

## Deliverables

- **The rails table** (`host_test/rails/`, `tools/check_rails.py`) gains a
  row per inherited rail, each held by a test that runs the month of 05 and
  searches everything captured:

  | Rail | Held by |
  |---|---|
  | nothing the mesh said is logged as text | the log capture and the journal searched for every payload `FakeMesh` produced and every observation's name |
  | a password is in RAM for the exchange only | 02's read-back tests, run again inside the month; a grep that no `SEND_LOGIN` or sensitive `SEND_CLI` payload reaches the frame log or the log ring |
  | one packet per command | `client_tx` equals the runner's sends, every wake of the month |
  | the token is printed by nothing and sent to one host | 03's tests inside the month; the URL log searched; every `Call.error` searched |
  | TLS verified | phase 3's rail, now scanning the component too (01) |
  | the server never chooses an image and never decides one is good | a test that an offer whose manifest fails check 3 for this slot is refused *by this firmware* although the server offered it; an apply whose candidate the server calls good but the machine's health check fails rolls back regardless |
  | no job left leased by a finished wake | 05's property test, here as a rail |
  | the library's boundary (no Arduino, no ESP in `agent-core`) | 01's grep |
  | nothing starts an update but a claimed job or a console command | a grep of the call sites of `RadioUpdate::start` and `SelfUpdate::start` |

  For each: holds, fixed here (which fix), or cannot be tested and why.
- **`docs/bench.md`**: the new items of 02–05 become sections 15 (*The
  agent*) and 16 (*A day as an exit node*) in the order a person would do
  them — token, `agent probe`, `agent id`, `agent heartbeat`, `agent wake`,
  the page, a probe on the own repeater, a job on a neighbour, an offer, an
  apply, a day — each with what to connect, what to type, what to see, what
  it means if not; the three steps that matter most marked (the first
  heartbeat recognised; the offer resuming after a pulled cable; the apply
  rolling back on a bad candidate). The *What to bring back* table gains
  04's intent constants and the budgets 05 used.
- **`docs/field.md`**: what a unit does as an exit node — when it checks in,
  what it will and will not do for its own repeater and for neighbours,
  what *asleep* means on the page, what *Wake it up* does and costs, how an
  update reaches it and what a person sees at each step, what to do when
  the server says *uplink silent*.
- **`docs/provisioning.md`**: the session gains the token (`set server.url`,
  `set server.token` from `/exit-nodes/new` — never printed back), and the
  check that the exit node's page shows the unit; the host test that replays
  the session is extended.
- **`README.md`**: the unit is now *a repeater, an exit node for its
  neighbours and a remotely updatable one*; how it is registered; the state
  of things — *no board has run this yet* until one has, and what 4.14 of
  meshmaster.dk it needs.
- **`docs/ARCHITECTURE.md`** held against the code once more, §13 included;
  the differences listed.
- **FOLLOWUPS**: the specification gaps every report of this phase raised
  (link 1.1 candidates: the local `status`/`neighbours`; anything in
  `unit-protocol.md` this firmware does differently), with the repository
  each belongs to; the bench pile's agent items.

## Acceptance criteria

- [ ] `tools/verify.ps1` green with the new rails; each rail has its line in the report.
- [ ] `docs/bench.md` has sections 15 and 16 and no phase-4 *Not verified here* item is missing (the report maps each).
- [ ] `docs/field.md`, `docs/provisioning.md` and `README.md` updated; the provisioning replay test passes.
- [ ] The report ends with the honest list again: what in the agent is most likely to be wrong when the first unit talks to the real server, in order, and why.

## Not verified here

Everything in `docs/bench.md` §15–16.

## Out of scope

New features. The bench pile.
