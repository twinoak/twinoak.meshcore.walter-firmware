# 02 — The battery says how many claims

**Depends on:** the sync of `agent-core` to the exit-node firmware's release that carries its tasks p1.20-69 and p1.20-70 (`tasks/sync/`, made by `tools/new-sync-task.ps1` from the `upstream` workflow's pull request). **If `components/agent-core/include/agent/AgentLoop.h` has no `WakeIntent::maxClaimsThisWake`, stop: change nothing, open no pull request and write no report (a report on `main` makes the driver skip this task for good). Say why in your last message.**
**Spec:** `components/agent/include/walter_agent/identity.h` (the cap, band by band ~L174–205; `intentFor` ~L208–210), `components/agent/agent_backend.cpp` (the intent and the budget of a wake ~L470–482; `kWakePostBytes` in `agent_backend.h` ~L86), `components/agent/modem_uplink.cpp` (`claimJobs` ~L549–583, `coverTheLeases` ~L585–630, the room for a call ~L244–270) and `modem_uplink.h` (`kClaimReplyBytes` ~L84, the kept payloads ~L368), `components/core/include/core/cycle/wake_guard.h` (`kWakeBoundMs` ~L52, `kRaisedBoundMax` ~L85) and `wake_room.h`, `components/agent/firmware_jobs.cpp` (an offer ~L402–640, the apply's bound ~L914–918, the session's ~L2175–2181), `docs/cycle.md`, `docs/bench.md` §17; the exit-node firmware's tasks p1.20-69 and p1.20-70; meshmaster.dk's pile 4.47 (253: `jobsWaiting`, `transactionsLeft`; 254: `holding`; 255: an apply made by its offer's result)
**Size:** one session.
**Model:** claude-opus-5

## Goal

A wake of this unit makes one claim and runs two jobs. The library can now make more
(`WakeIntent::maxClaimsThisWake`), and a caller that says nothing still gets one. Thomas's rule
for this board: it takes jobs until none are left or the battery says sleep.

After this task the band of the battery says how many claims a wake may make, the bound on the
wake covers each of them as it covers the first, and a wake says in its own words how many it
made and why the last one was the last.

## What was read before the run (the reviewer, 22 November)

- **The server hands back more than the sync's report says.** meshmaster.dk's task 254 as it
  was built (`src/MeshMaster.Core/Transport/HandedBack.cs`, `Lost`): on a claim that carries
  `holding`, **every** job leased to this exit node whose lease is still running, that has no
  result, and whose id the claim did not name is handed over again, first, its attempt
  unspent. The server keeps no record of an answer that did not arrive. The sync's report
  (`tasks/sync/reports/02.md`, *`holding` in every claim*) says *only a lease whose answer it
  never managed to deliver*, and so *nothing on this claim* for the three kinds of work this
  firmware keeps outside the library's buffer: a job the result gate left to its lease, a
  session the bound cut, an apply whose result waits for the next boot.
- **With one claim a wake that mostly holds**, because those leases have run out by the next
  wake. **With several it does not.** A fetch left to its lease for want of budget is handed
  back by the very next claim of the same wake, and by the one after. And a wake a person asks
  for inside a session's or an apply's lease, whose owed result did not get through, is handed
  that job again. Deliverable 11 is the answer to both, and it is this task's because this
  task is what makes a wake claim more than once.

## Deliverables

1. **The number, band by band**, beside the transaction caps in `identity.h` and with their
   warning: placeholders to be set on the bench. `plenty` 8, `normal` 4, `saving` 1,
   `no-reading` as `normal`, `critical` none (the modem is not brought up). A worse band never
   promises more, held by a `static_assert` as the caps are. `intentFor` carries it.
2. **The bound on the wake covers every claim.** `coverTheLeases` runs at each claim and the
   bound is raised from where the wake stands then. A claim is not made when the wake cannot be
   raised far enough to hold a job's lease and the calls after it (`kRaisedBoundMax`): the wake
   ends its jobs there, and that is *no room*, not a failure. Show with a test that a wake of
   several claims is never ended by the reset the bound exists for.
3. **Between two claims nothing of the first is left.** The claim's answer, the kept payloads
   and every credential of the first claim are wiped before the second is read, as they are at
   the end of a wake today. Say where.
4. **After a job that takes the radio away.** An apply of a radio image, a session and a power
   cycle end with the radio starting again. The wake claims again after one only when the radio
   is healthy by the supervisor's own word; otherwise the wake's jobs end there. After an apply
   of the Walter's own image there is no wake left to claim in.
5. **An offer and its apply in one wake** (meshmaster.dk's 255): the offer's result is
   answered `jobsWaiting`, the next claim brings the apply, and the apply runs under its own
   bound from where the wake stands. A test with the fake server; say what the wake costs in
   time for a radio image and for the Walter's own.
6. **The bytes.** `kWakePostBytes` stays the cap on what a wake posts, and a result that does
   not fit is kept and ends the claiming, as the library has it. Say in the report how many
   ordinary results fit, and leave the number alone.
7. **What the unit says.** The wake's line in the journal, `agent wake` at the console and the
   `jobs` section of a report: claims made, jobs run, and the reason the last claim was the
   last (*nothing waiting*, *the band's number*, *no room*, *no transactions*, *a result
   kept*, *the radio is away*, *a claim failed*).
8. **Tests**, on the host with the fake server: five jobs waiting in `plenty` are three claims
   and five jobs; in `saving` one claim; two reports and a session are two claims, the session
   alone in the second; a claim whose answer is lost is asked again and its jobs run (the
   library's, seen through this board's uplink); the reasons of deliverable 7, each once.
9. **The bench**, as lines in `docs/bench.md`: a report, a scanner image and a session queued,
   one wake; the journal's line. Not run by this session.
10. **The words.** `docs/cycle.md` where it walks through a wake; `docs/console.md`; a line in
    `release-notes/unreleased/`.
11. **What this firmware holds is named in the claim.** The library fills `holding` from its
    own buffer of results. Where the claim is written (`ModemUplink::claimJobs`, or where the
    report argues is better) the ids this firmware holds are added to it, none twice, the
    request's room sized for them:
    - a job the result gate left to its lease, for the rest of the wake that left it, and
      after a sleep for as long as this firmware still means to continue it;
    - a session or an apply the journal has in progress, or whose result is owed and has not
      been taken by the server.

    A job named so is not handed back: its lease runs out and the server's own rules take it
    from there, as before this pile. **And a wake that left a job to its lease for want of
    budget claims no more**: the reason is *no room*. Tests, with the scripted server doing
    the server's rule (every running lease without a result that the claim did not name comes
    back first): a fetch that ran out of budget in a wake of several claims is not handed back
    in that wake and the wake ends its claims there; a wake asked for five minutes after a
    session the bound cut, with the owed result's post unanswered, is not handed the session
    and **no flash begins**; the same for an apply's owed result; and a claim whose answer was
    lost is still handed everything back, because nothing of it is named.

## Acceptance criteria

- [ ] Without the library's member the session stopped and wrote nothing; with it, everything below.
- [ ] `plenty` makes up to 8 claims, `normal` 4, `saving` and below 1; the `static_assert` holds the order (tests' names).
- [ ] A wake of several claims never meets the bound's reset, and a claim the bound cannot cover is not made (tests' names).
- [ ] Two reports and a session queued are run in one wake, the session alone in its claim (test's name).
- [ ] An offer answered `jobsWaiting` is followed by its apply in the same wake (test's name).
- [ ] The report and the journal name the reason the last claim was the last, for each of the seven (tests' names).
- [ ] A job this firmware holds outside the library's buffer is named in `holding` and is not handed back; a wake that left a job for want of budget makes no further claim (tests' names).
- [ ] No session and no apply is begun a second time when its owed result's post went unanswered and the next claim came inside its lease (tests' names).
- [ ] Rails R22, R23 and R24 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The library: the exit-node firmware's pile 1.20, taken by the sync. The server: meshmaster.dk's
pile 4.47. More than two jobs in a claim. The lost answer itself: parked until the bench.
Setting the bands' numbers: the bench's.
