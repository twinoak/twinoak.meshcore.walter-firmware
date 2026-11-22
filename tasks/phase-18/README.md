# Phase 18 — a session that ends well, and a wake that takes what is waiting

Written on 22 November.

**What happened.** On 21 November the unit at Bellevue gave its first NoiseScope sample: six
steps, 284 lines, the PSRAM's room on its first use. It took five wakes to get there, and the
session ended `stuck` although the repeater was back.

- **`stuck`** was the scanner's watchdog. It survives the soft reset the flasher leaves by, and
  reset the repeater 59 s into the proof (`tasks/phase-17/FOLLOWUPS.md`, *The first session on
  hardware*).
- **Five wakes** because a wake is one claim of two jobs, and a session is handed out alone.
- **An hour before**, a claim's answer was lost and the server had leased two jobs to a unit
  that never knew (the same follow-ups, *A claim whose answer was lost*).
- **Nine of the sixteen lines** of its one `hist` step never reached the server: the port was
  not read while a call to the server was out, and its ring is 1 024 bytes.

**What Thomas asked for.** That the Walter takes jobs until none are left or the battery says
sleep; and that an offer and its apply can be one visit.

**What this pile builds.** 01 and 03 are this repository's own and can run now. 02 turns on what the
library brings, and waits for it: the exit-node firmware's pile 1.20 (a wake claims again; a
lost claim is asked again), its release, this repository's `upstream` workflow and the sync
task that follows. meshmaster.dk's half is its pile 4.47.

**Chosen by me, his to change before the run:**

- **A reset pulse after a flash**, and not a proof that forgives one watchdog reset: the proof
  stays strict.
- **8, 4 and 1 claims** for `plenty`, `normal` and `saving`: placeholders, like the bands'
  transaction caps, until the bench has measured what a claim costs.

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-watchdog-ends-with-the-scanner](01-the-watchdog-ends-with-the-scanner.md) | — | the image a flash wrote is started by the reset line, so no watchdog a scanner started outlives it; the simulated chip has that watchdog; a session that measured ends `restored` |
| 02 | [the-battery-says-how-many-claims](02-the-battery-says-how-many-claims.md) | the sync of agent-core with p1.20-69 and p1.20-70 | claims a wake by band; the bound covers each; an offer and its apply, or two reports and a session, in one wake; the wake says why its last claim was the last |
| 03 | [a-step-is-listened-to](03-a-step-is-listened-to.md) | — | no call to the server between a step's command and its end; the port's ring sized by the baud rate; bytes lost at the port counted and said; the simulated port has the ring |

```powershell
.\tasks\run-all.ps1 -Phase '18' -To 01
.\tasks\run-all.ps1 -Phase '18' -From 03 -To 03
```

and, when the sync has merged:

```powershell
.\tasks\run-all.ps1 -Phase '18' -From 02 -To 02
```

**Not the whole pile before the sync.** 02 stops when the library has not arrived and writes no
report, the driver stops there, and 03 behind it does not start.

## Models

- **Opus**: 01, 02, 03.

## Afterwards (Thomas)

- **After 01 and 03:** release, offer and apply to the unit, then a session. It should end
  *restored*, about twenty minutes after it began, with one outage and not two. A `hist` step
  typed by hand should leave every line it sent (what is in them is the repeater firmware's
  pile 3.15).
- **For 02:** meshmaster.dk's pile 4.47 deployed; the exit-node firmware's pile 1.20 released;
  *Run workflow* on `upstream` here; `tools\new-sync-task.ps1` and
  `.\tasks\run-all.ps1 -Phase sync`; then 02, a release, and the unit updated.
- **Then try it:** queue a report, a scanner image and a session and wake the unit once.
- **The bench, when there is one:** the tasks' lines in `docs/bench.md`.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
