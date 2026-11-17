# Phase 15 — what every wake was paying

Written on 16 November.

**What happened.** Thomas asked whether *Wake it up* had worked. It had. The unit's report, read
for that, showed something else: all eight calls in its list of failed ones were the same one,
and its journal said every sleep had been shortened. He asked for the failures to be looked into
thoroughly.

**What was found.** Two faults, each at every wake.

- **The answer to the observations call is lost.** The batch reaches meshmaster.dk and is
  stored; the `200` never reaches this firmware; one modem command hangs for the library's
  30 seconds, and the call ends *nothing answered, the connection closed before the body was
  complete*. Known since 8 November (`tasks/phase-10/FOLLOWUPS.md`, first item) and not
  followed up. The cause is not known.
- **Every sleep is five minutes instead of fifteen.** A committed radio update leaves the
  candidate slot reading as a download half done.

A wake that should be 9 seconds is 39, and there are three where there should be one: about
2¾ hours a day with the modem on, against a quarter of an hour.

**And one that follows from the first.** The id of a batch whose answer was lost is not kept
over a sleep, so every wake posts everything again as new and the server stores it again.

**What this pile builds.** The slot (01). The firmware writes down what the modem said in a
call that failed, so that the unit in service says it in its report (02). One more read before
a call is given up (03). And, once the agent library keeps a batch's id over a sleep, what is
left for this board (04).

**04 waits.** It needs the library at a release that holds the exit-node firmware's task 55
(its pile 1.13). So the pile is run to 03 first:

```powershell
.\tasks\run-all.ps1 -Phase '15' -To 03
```

**And three from the first night of 0.1.8** (written on 17 November). The second ask of 03
was made 35 times and answered none; it goes (05). Two wakes took the hourly `status` job, ran
into the five-minute bound and were ended by reset, and nothing says what they were doing; a
wake that is ended now leaves word (06). And the unit has twice said what a failed call looks
like, with nothing to lay it beside; the report prints a call that worked too (07). They do
not wait for the library:

```powershell
.\tasks\run-all.ps1 -Phase '15' -From 05 -To 07
```

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-voided-slot-is-no-download](01-a-voided-slot-is-no-download.md) | — | a slot a promotion made void reads as empty, on the unit in service too; a sleep is shortened only for a download that is half done |
| 02 | [what-the-modem-said](02-what-the-modem-said.md) | — | the receiving side of the newest failed call, kept over a sleep and printed by `agent calls` and the report |
| 03 | [read-again-before-giving-up](03-read-again-before-giving-up.md) | 02 | one more ask after a receive that failed with the close seen; a kept count of the answers it recovered |
| 04 | [a-batch-keeps-its-id](04-a-batch-keeps-its-id.md) | the library's bump | a lost answer costs one resend the server knows; the scratch held to the library's figures; `agent obs` names a waiting batch |
| 05 | [the-second-ask-goes](05-the-second-ask-goes.md) | — | a failing call costs one command timeout again; the count of task 03 and its key are gone; the record of task 02 is untouched |
| 06 | [what-a-wake-was-doing-when-it-was-ended](06-what-a-wake-was-doing-when-it-was-ended.md) | — | a wake that did not reach sleep leaves the step, the call or the job it was in and for how long; the next boot journals it; a fifth total in the heartbeat |
| 07 | [what-a-call-that-worked-looks-like](07-what-a-call-that-worked-looks-like.md) | 05 | `agent calls` and the report print the latest answered call beside the failed one, with each receive's two times and what the answer said of itself |

After the library is here:

```powershell
.\tasks\run-all.ps1 -Phase '15' -From 04 -To 04
```

## Models

- **Opus**: 01, 02, 03, 04, 05, 06, 07.

## Afterwards (Thomas)

- **After 01 to 03: release**, merge its notes pull request, import it on meshmaster.dk, mark
  it canary, offer and apply. Then, a few wakes later, *Fetch now* on the Diagnostics tab:
  - the journal's wakes say *next 15 min* and no longer *the sleep was shortened*;
  - `agent calls` shows what the modem said in the newest failed call, and whether an answer
    was read after a failed receive.
- **After 05 to 07: release**, import, canary, offer, apply. Then, after a night:
  - *Fetch now*, and `agent calls` shows a call that worked beside the one that failed;
  - the wakes are about 40 s and none is 68;
  - if a wake is ended by its bound again, the journal's next line says what it was doing, and
    `wakesOverrun` (or the name task 06 gives it) moves on the exit node's page.
- **The bench, with a board that has a radio on its link.** `docs/bench.md` §13, the ★ line:
  the trace build, `lte up`, `agent wake` with adverts waiting. A Walter alone hears no adverts
  and has nothing to post, so a spare board helps only on a base with a radio. The trace names
  the command that goes unanswered, which the record of 02 cannot.
- **For 04:** run the exit-node firmware's pile 1.13 and release that firmware. This
  repository's weekly `upstream` workflow then opens the pull request that moves the library
  (it can be started by hand); if that pull request is red,
  `.\tools\new-sync-task.ps1 -Tag <tag> -Pr <n>` and `.\tasks\run-all.ps1 -Phase sync`. Then 04,
  and a release.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
