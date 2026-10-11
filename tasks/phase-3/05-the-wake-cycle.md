# 05 — The wake cycle

**Depends on:** 02, 04.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §10, §7, §8; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-7, §5, §8; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.3, §5.4, §5.8, §7, §8; [`spec/proposals/`](../../spec/proposals/) — read all three **as context for where the seam must be, not as an interface to implement**.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Everything built so far becomes one firmware: a wake that runs the same steps
in the same order every time, each bounded in seconds and in what it may
spend, and then sleeps. The supervisor, the clock, the telemetry, the modem,
an update step — each already exists and is tested alone; this task is the
order they run in, what happens when one of them fails, and the guarantee that
nothing is skipped silently and nothing runs twice.

And it ends at a seam. Where the firmware would talk to its backend there is
an interface, and the implementation in this repository does nothing. That is
deliberate (`spec/SYSTEM.md` §8): the server side is three proposals, not a
decision, and code written against a guess would have to be found and removed
later.

## Deliverables

- **`components/core/cycle/WakeCycle`** — one pass, over interfaces only (the
  link client, the supervisor, the update machines, the uplink, the backend,
  the policy, the sleeper, the journal, a clock). A list of steps, each with
  a name, a precondition, a time bound and an outcome that is recorded:

  1. **why** — the wake's reason (task 04);
  2. **the radio** — the supervisor's check. A radio that is a scanner, or
     silent, or mid-update changes what the later steps may do, and each of
     those is a named state of the wake, not an error;
  3. **resume** — if the journal says an update of either MCU was in
     progress: the radio's continues here (bounded slices; a flash may take
     several wakes' worth of time and is allowed to hold the wake open up to
     its own bound); the Walter's own trial (phase 2, task 04) runs its
     conditions through the steps below;
  4. **the radio's state** — `STATUS`, `POWER_GET`, `ENV_GET`; the pending
     attention acknowledged and its reason kept;
  5. **the band** — the interval policy with the fresh reading: what the rest
     of this wake may do;
  6. **the uplink** — if the band and the backoff allow: bring it up within
     the budget; the network's time;
  7. **the clock** — set the radio's (`TIME_SET`) when we have a network time
     and the radio's is unset, restored, or off by more than a named
     threshold; never from a time that failed task 01's sanity checks;
  8. **the backend** — the seam, below;
  9. **at most one update step** (S-7): either the radio's or the Walter's
     own, never both, only if the band allows, only if the backend's answer
     or a console command asked for it, and never while the other is
     unconfirmed;
  10. **sleep** — task 04's way out, with the interval from step 5 (shortened
      if a download is half done and the band is `plenty`; never shortened in
      `saving` or `critical`).

  A step that fails records why and the wake goes on to the next step that
  does not depend on it; the table of which step needs which is data, with a
  test. The whole wake has a budget in seconds; a step that would overrun it
  is skipped and recorded as *no time*.
- **`components/hal/Backend`** — the seam:

  ```
  exchange(what_we_know, deadline) -> what_is_asked | nothing | failed
  ```

  where *what we know* is a plain structure this firmware fills — the unit's
  identity as the radio gave it, both firmware versions and descriptors, the
  image store's state, the supervisor's incidents, the telemetry ring, the
  wake's own record, the uplink's signal — and *what is asked* is a small
  closed set of requests **this firmware already knows how to carry out by
  itself**: fetch a release from this base URL as candidate; run the radio's
  update; run the self-update; set these settings (non-secret keys only); run
  the ladder; nothing. No JSON, no URL paths, no job types, no job ids: those
  belong to whoever implements the interface.

  Two implementations here: **`NullBackend`** (returns *nothing*; the
  firmware's default) and the tests' **`ScriptedBackend`**. The self-update's
  third trial condition (phase 2, task 04) stays what that task made it.
- **The mesh client's calls are not used.** The link client has them (phase
  1); nothing in the wake cycle starts a mesh job, pulls observations or
  sends on the mesh. A test asserts that a simulated month of wakes sends no
  request in the `JOB_*`, `SEND_*`, `PATH_*` or `OBS_PULL` range. That is the
  agent's work, and the agent is not here.
- **`components/core/cycle/TelemetryRing`** — the readings of step 4 with
  their times, kept across sleep while there is nobody to give them to: a
  bounded ring in retained storage (and, if that is too small for a useful
  history, in the journal's partition — decide with the numbers), oldest
  dropped first, a dropped count kept. Emptied only when the backend says it
  took them. Timestamps taken before the clock was known are marked as such,
  not guessed.
- **The wake's record**: one compact line per wake in the log and the journal
  — reason, the radio's state, band, each step's outcome in a letter, the
  seconds awake, the next interval — so that `journal` after a week on a mast
  is a page a person can read. The format is documented and has a test.
- **`main/app_main`**: the real things constructed and handed to `WakeCycle`;
  with a USB host present the console runs beside the cycle and `cycle now`
  runs a pass on request, `cycle dry` prints what a pass would do without
  doing it.
- **`docs/cycle.md`**: the steps, the dependency table, the budgets, what a
  wake record's letters mean, and — one page — **what an implementer of
  `Backend` has to provide and may rely on**, which is the hand-over to the
  agent work.

## Tests (host — everything scripted)

- The good wake: every step, in order, once.
- **Each step failing alone**: the wake completes, the dependent steps are
  skipped with the reason, the independent ones run, sleep is reached.
- The states of the radio: a scanner (no clock set, no telemetry, the wake
  still reports and sleeps); silent (the supervisor's ladder within its caps,
  the uplink still used to say so); mid-update (resumed, nothing else touches
  the radio).
- S-7: a backend that asks for both updates in one wake gets one; the other
  is still wanted at the next wake and happens then, if the first confirmed.
- A backend that asks for something in `critical`: refused, recorded, and
  told at the next exchange.
- A backend that asks to set a secret key, or a protective threshold: refused.
- The budget: a slow modem eats the time; the backend step is *no time*; sleep
  is reached within the bound.
- The telemetry ring: a week without uplink fills it, drops the oldest,
  counts; the uplink returns and everything kept is offered once and removed
  only when taken; a restart in between loses nothing that was stored.
- **A simulated month**: random radio behaviour, modem coverage and battery,
  a few thousand wakes — every wake ends in sleep; no wake exceeds the hard
  bound; the supervisor's caps hold; no mesh request is sent; the number of
  flashes is at most the number asked for.
- The wake record's format, and that it contains no secret and no IMEI.

## Acceptance criteria

- [ ] `tools/verify.ps1` green; the firmware's default image runs the cycle with `NullBackend` and sleeps.
- [ ] `components/hal/Backend` contains nothing that names MeshMaster, a URL path, JSON or a job; the report says how the closed set of requests was chosen and what an agent would need that it does not offer.
- [ ] The step dependency table is data and is printed in the report.
- [ ] The simulated month's totals are in the report: wakes, modem attempts, seconds awake per day, flashes, incidents.
- [ ] `docs/cycle.md` has the page for the implementer of `Backend`.

## Not verified here (Thomas, with the rig and a SIM)

Add to `docs/bench.md`, "14. A day":

- Leave a provisioned unit alone for a day with `wake.interval` at 15
  minutes: `journal` shows one line per wake, all reaching sleep; the RAK's
  uptime is a day; its clock is right.
- `cycle dry` and `cycle now` at the console.
- Cover the panel (or lower a bench supply) until the band changes: the
  interval follows, and in `critical` the modem is not used.

## Out of scope

The MeshMaster agent, its protocol, its jobs, exit-node work on the mesh
(`spec/proposals/01` and `03`). Deciding when a fleet updates
(`spec/proposals/02`).
