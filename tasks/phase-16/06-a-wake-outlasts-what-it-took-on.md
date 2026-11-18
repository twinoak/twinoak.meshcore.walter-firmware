# 06 — A wake outlasts what it took on

**Depends on:** —
**Spec:** `components/core/include/core/cycle/wake_guard.h` (`kWakeBoundMs` ~L52, `kRaisedBoundMax` ~L75, `allow` ~L121, `raiseFor` and `putBack` ~L139, `awakeMs` ~L146), `components/core/cycle/wake_cycle.cpp` (`stepBackend`: the exchange's deadline and the one place the bound is raised, after it has returned; `allow` ~L422), `components/core/include/core/cycle/wake_cycle.h` (`kBackendBoundMs` ~L237); `components/agent/agent_backend.cpp` (`setDeadline` ~L429 and ~L579; the console's `agent wake`, which raises the bound for the whole exchange, ~L707–724), `components/agent/modem_uplink.cpp` (`claimJobs`; a call that is not begun for want of time, ~L247–254) and `include/walter_agent/modem_uplink.h` (`kLeastCallMs` ~L103, `setDeadline` ~L179), `components/agent/firmware_jobs.cpp` (~L805: an apply raises the bound itself); the copied library, read and not changed: `components/agent-core/src/JobRunner.cpp` (~L117 and ~L146: a job ends `timeout` when its lease would be passed) and `include/agent/JobEnvelopes.h` (`ClaimedJob.leaseUntil` ~L75); `docs/cycle.md` (*The budget, and the things that may hold a wake open*, ~L399–425), pile 15's task 06 and its report; meshmaster.dk's `docs/dev/unit-protocol.md` §3 (the lease).
**Size:** one session.
**Model:** claude-opus-5

## Goal

In the night of 17 November the unit took the hourly `status` of its own repeater at 01:29:53
UTC and was next heard at 01:34:56, as a new boot: the bound on a wake had reset it. The same at
02:05:24 and 02:10:28. Each reset was read as a watchdog and followed by thirty minutes of sleep.
The job, leased for four minutes, was taken twice and answered neither time, and nothing was
measured for over two hours. meshmaster.dk's page of job 32763 says it: *taken twice by Risskov
LTE, and no answer came before it expired*.

What those wakes were in is not known, and pile 15's task 06 makes the next one say. This task
is about the arithmetic that lets it happen whatever they were in:

- Step 8 gives the exchange a deadline of `kBackendBoundMs`, thirty-nine minutes, because *the
  agent's jobs are minutes*. `docs/cycle.md` lists *a backend exchange* among the things that
  raise the bound on a wake.
- It does not. The bound is five minutes. Step 8 raises it after the exchange has returned,
  and only when an update machine ran inside it. An apply raises it for itself. The console's
  `agent wake` raises it for the whole exchange, so the bench never meets this.
- So in a wake of its own an exchange is promised thirty-nine minutes and reset at five. A job
  leased for four minutes fits only when everything else in the wake takes under one: the
  radio's question, the modem's attach, the heartbeat, the claim, the result's call, and an
  observations call that stalls for thirty seconds at nearly every wake.

A job the radio never answers must end as a `timeout` the server is told of and a wake that
goes to sleep. The reset is for a wake that hangs.

## Deliverables

1. **The bound covers what the wake took on.** When jobs are claimed, the wake's bound is
   raised to the end of the latest lease among them and what the calls after them may take: a
   result for each job and the observations, each at the modem's command timeout. Never past
   `kRaisedBoundMax`. A wake that claims nothing keeps its five minutes.
2. **A lease never passes the wake.** Where that ceiling cuts the bound short of a lease, the
   lease the library is handed for the job ends where the room for the calls after it begins,
   so that the library ends the job `timeout` by its own rule and the result is posted. The
   copied library is not changed.
3. **A wake ends itself.** Inside the exchange no call is begun that the bound has no room
   to finish, by the rule a call already has for the uplink's deadline. What was not sent
   waits as it does today. The wake then sleeps its ordinary interval.
4. **One figure.** The deadline the exchange is given and the bound on the wake are worked
   out in one place, and every call takes what is left of the sooner.
5. **An apply, a session, a climb and the console's `agent wake` keep the bounds they have.**
6. **The rail stays.** A wake that hangs past its bound, raised or not, is still reset, and
   the sleep after a reset is what it is.
7. **Said.** The wake's record and the report's `wakes` section: *the bound was raised to N s
   for 2 jobs*, and a call that was not begun for want of room. `docs/cycle.md`'s table says
   what is now true of a backend exchange.
8. **The words.** `docs/cycle.md`, `docs/console.md`. `docs/bench.md` §17, one item: with the
   radio's link pulled and a `status` waiting on a bench server, `stay` off: the job is
   reported `timeout`, the wake sleeps, and the next boot says it woke by the timer. One line
   in `release-notes/unreleased/`.

## Acceptance criteria

Against the scripted server, radio and modem, in a wake of the cycle's own and not through
`agent wake`.

- [ ] A `status` leased for 4 min whose radio never answers: the result posted is `timeout`, the wake reaches sleep, no restart is asked for, and the sleep is the ordinary interval (test).
- [ ] The same with the observations call after it stalling for the command's 30 s: the same (test).
- [ ] The same with the result's call unanswered for 30 s: the wake reaches sleep, and the result goes at the next wake as an unsent one does today (test).
- [ ] A job leased for 11 min that takes ten: it runs to its end and is reported `ok`; the bound was raised to cover it (test).
- [ ] Two jobs with leases of 4 and 11 min: the later lease governs (test).
- [ ] A job leased for 60 min: the lease the library sees ends inside `kRaisedBoundMax`, and a job still running then is reported `timeout` before the bound (test).
- [ ] A wake that claims nothing: its bound is `kWakeBoundMs`, as today (test).
- [ ] An apply and the console's `agent wake`: their bounds as today (tests unchanged).
- [ ] A call scripted never to return: the wake is reset at its bound, as today (test).
- [ ] The scripted night of 17 November, the same `status` taken at two wakes with a silent radio: two `timeout` results, two ordinary sleeps, no reset (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Why the radio did not answer on 17 November. The thirty minutes of sleep after a reset. The
lost answer to the observations call (pile 15). The library. What the server hands out in one
claim. The board: no session opens a port. A release: Thomas presses the button.
