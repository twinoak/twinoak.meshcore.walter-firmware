# 03 — The radio's update

**Depends on:** 02.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5, S-7, §6; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §7; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.1–§5.4 (what a healthy radio says); [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §4 (the descriptor the health check is held against).
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

The procedure that replaces the radio's firmware and **cannot leave the unit
worse off than it found it**: flash the candidate, make it prove itself, keep
it — or put the golden image back and never try that candidate again. Every
step is written down before it is taken, so a Walter that loses power in the
middle picks up where it was. This is the reason the hardware has two MCUs.

## Deliverables

- **The journal** (`components/core/update/Journal`): a small append-only log
  in its own flash partition (through task 02's region interface and
  `SimFlash`): each entry is a step about to be taken or a result obtained,
  with a sequence number and a CRC; an entry is valid only when completely
  written; reading the log yields the last valid entry, whatever a power cut
  did to the one after it. It wraps without ever losing the entries of the
  update in progress. The same journal serves task 04 and the supervisor.
- **`components/core/update/RadioUpdate`** — a state machine over interfaces
  (the store, the flasher, the link client, the board, a clock, the battery),
  one step per call, so that the wake cycle of phase 3 can run it in slices
  and so that every transition is testable:

  ```
  idle ─► check ─► flashing-candidate ─► booting ─► proving ─► committing ─► done
             │            │                 │          │
             │            └─────────────────┴──────────┴──► rolling-back ─► restored
             └─► refused (a precondition; nothing was touched)       └────► stuck
  ```

  - **check** — the preconditions, each a named refusal: a candidate is in the
    store and verifies; it is not on the bad list; it is for this slot (role,
    board, band, and a role this slot may hold); the battery is above the
    update threshold and not falling fast; the link is up, or is down in a way
    the supervisor has already classified; no other update is journalled as in
    progress (S-7); **a golden image exists** — or the caller passed
    *first-install*, which only a person's console command does.
  - **flashing-candidate** — task 05's `flash()`; resumable by construction.
  - **booting** — the radio was reset by the flasher; wait for `EV_BOOT` or a
    `HELLO` answer, up to a window.
  - **proving** — the health check, all of:
    1. `HELLO` answers, and its `fw_version`, role and board are the
       candidate's descriptor's, and its link major is one this Walter speaks;
    2. the clock can be set (`TIME_SET` accepted) — the first thing a healthy
       repeater must take;
    3. `STATUS` says the radio initialised;
    4. the link stays up — a `PING` every few seconds — for a **proving
       period** (a build constant; minutes, not seconds), with **no restart**
       in it (`boot_id` unchanged);
    5. where the role has telemetry, `POWER_GET` answers.
    Anything else is a failure. A candidate that boots, answers and then
    restarts itself two minutes later has failed.
  - **committing** — the store's promotion; then *done*, journalled.
  - **rolling-back** — journal the candidate's hash onto the bad list **first**;
    flash the golden image; boot; prove it by the same health check (with the
    golden image's descriptor); *restored*. If the golden image does not come
    back either: *stuck* — the supervisor's last rung, a report, and no further
    attempt by this machine.
  - **Every state is journalled before its action starts.** At start-up
    `resume()` reads the journal: mid-flash → continue the flash; booting or
    proving → start the proving over (never assume it passed); committing →
    finish the promotion; rolling-back → continue rolling back.
- **Never both** (S-7): the machine refuses to start while the Walter's own
  update (task 04) is journalled as unconfirmed, and says so.
- **A switch to another role and back** is the same machine with a different
  source and no promotion: `flashRole(scanner)` flashes the stored scanner
  image, proves only that *something* that calls itself a scanner is there
  (the text-mode detection of phase 1, task 02), and `flashRole(repeater)`
  puts the golden image back and proves it. It exists so that a NoiseScope
  session later needs no new flashing logic.
- **The console**: `rak update` (run it, printing each transition), `rak update
  --first-install`, `rak update status` (the journal's story of the last
  update, in words), `rak role scanner|repeater`.
- **`docs/update.md`**: the diagram, the health check, what each console line
  means, and what state a unit is in after a power cut at each step.

## Tests (host — scripted radio, simulated chip, simulated flash)

- The good path, end to end: candidate becomes golden; the old golden is gone
  only after the new one is in place.
- **Each proving failure** in turn: no `HELLO`; `HELLO` with the old version;
  the wrong role; the wrong board; `TIME_SET` refused; the radio not
  initialised; a restart inside the proving period; silence after a good
  start. Each ends *restored* with the candidate on the bad list — and a
  second `rak update` with the same candidate is *refused* at **check**.
- The golden image fails to come back: *stuck*, once, journalled; nothing
  retried.
- **A Walter reset at every journal entry of the good path and of a rollback**
  (a loop): after `resume()` the machine reaches *done* or *restored* — the
  same outcome as without the reset — and the radio's flash log shows no write
  outside the application's pages and the settings page.
- A power loss *during a journal write*: the torn entry is ignored and the
  entry before it governs.
- Each precondition refused, with the flash log empty.
- No golden image and no *first-install*: refused. With it: allowed, and a
  failure ends *stuck* with the reason "no image to return to".
- The scanner round trip leaves golden and candidate untouched.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The reset-at-every-entry loop covers both paths; the report gives the number of entries and shows one resumed run's transitions.
- [ ] The bad list is written **before** the rollback flash, and a test proves a failed candidate is never flashed a second time.
- [ ] The proving period and every window and threshold are named constants with a sentence each; the report lists them as "to be set on the bench".
- [ ] `docs/update.md` exists.

## Not verified here (Thomas, with the rig) — with task 05 of phase 1, the session that matters most

Add to `docs/bench.md`, "7. Updating the radio":

1. A repeater release as golden (from the previous task), a newer one pushed
   as candidate: `rak update` — watch the transitions; afterwards `image list`
   shows it golden and `rak hello` its version.
2. A candidate that is valid and signed but **broken on purpose** (a build
   that never starts its link): `rak update` → it fails proving, the golden
   image is flashed back, `rak hello` shows the old version, `image bad` lists
   the broken one; `rak update` again is refused.
3. Pull the Walter's power during the flash, during proving, during the
   rollback. Each time, after power returns, it finishes by itself and `rak
   update status` tells the story.
4. `rak role scanner`, look at `rak text`, `rak role repeater`: the same ID
   and name as before.

## Out of scope

Deciding *when* to update (the wake cycle and, later, the server). Fetching a
candidate (phase 3). The Walter's own update (04).
