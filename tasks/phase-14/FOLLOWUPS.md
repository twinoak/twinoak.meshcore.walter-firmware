# Phase 14 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[later] The library counts a flood under the scope it *asked* for, not the one it got.**
  `FloodPolicy::sent(flooded, mode)` keeps the mode the rule chose and `answered()` files the
  transaction under it (`components/agent-core/src/FloodScope.cpp`), so a flood asked `Scoped`
  that §6.2's `scoped` says went out **unscoped** - a `region default` cleared under this end's
  feet, a `mode` 2 that did not take - is counted as `answeredScoped`. This end notices (the
  name is read again and `LinkMeshCounters::scope_surprises` counts it) and cannot correct the
  library's figure: `agent-core` is a hash-checked copy of upstream and is not changed here.
  The cure is in the exit-node firmware: `sent()` would take what the mesh says the send came
  to. Until then `scope_surprises` is how often the three counts could be wrong, and on a
  healthy unit it is 0.
- [ ] **[later] No way to turn the unscoped try off on a unit.** The library's `flood.unscoped`
  (*retry* or *never*) has a console key on the companion exit nodes and none here: the default,
  *retry*, holds. The day `flood.max.unscoped` is nought on the Danish mesh that try is airtime
  for nothing, and the heartbeat's `answeredUnscoped` is how the day is seen coming.
- [ ] **[later] The radio's half is the repeater firmware's pile 3.13.** Until a radio of link 1.3
  is on the unit this board says nothing about scope and its heartbeat has no `flood` object.
  That is correct, and worth knowing before somebody looks for the line on meshmaster.dk.
- [ ] **[later] The specification's source is not committed.** `spec/MANIFEST.txt` says *at commit
  204210b plus uncommitted changes*: links 1.1, 1.2 and 1.3 are in the working tree of
  `twinoak.meshcore.standard-repeater-design-v2/software/spec` and in no commit of it. Thomas's
  to commit there; the copy here is byte for byte what that tree holds.
- [ ] **[later] Phase 13's follow-ups are still open**, the first of them above all: a wake by the
  radio's line inside a wait sends nothing.
  The first is task 06 of this pile, written on 15 November.
- [ ] **[later] The radio says `ok` to `walter.wake 1` whether or not it raised an attention.**
  Refused inside the hourly limit, or with one already pending, the answer is the same `ok`
  (`Settings.cpp`, repeater firmware). `sensor get walter.wake` is where the truth is read, and
  meshmaster.dk's wake job does not read it. The repeater firmware's and meshmaster.dk's to
  settle, not this board's.
- [ ] **[later] After a power cut the wait is owed whole.** The saved state is in flash and
  survives; the radio restarted with the board, so nothing measures the time away. The epoch
  beside the saved state, once the board has a clock again, would.
- [ ] **[later] One sentence of the gate's log still says the old thing.**
  `.github/workflows/release.yml` ~L160 echoes *`$SHA` changes only VERSION and release-notes/
  above `$ci_sha`, and has no ci run of its own*. That is not a comment and not the pull
  request's body, and task 03's deliverable 4 held every other byte of that file, so it was left
  alone. The commit may well have a run of its own — a person's merge of the notes pull request
  raises a push event, and v0.1.5's merge commit got run 43484073183, cancelled 19 minutes later
  by the next push — it is simply not the run the gate waits for. The line is close to dead
  code besides: a release is refused with no waiting line, so main's head is never a
  release-notes-only commit when one runs, and this branch of the step does not print.
  `tools/release/release_notes.py`'s own sentence beside it was corrected.
- [ ] **[Thomas to decide] Is 3.7 V still the right floor for a unit awake for thirty-seven
  minutes?** `core/update/radio_update.h`'s `kBatteryMinMv` reasons 3 700 mV from *two MCUs awake
  for ten minutes of flashing and proving*. The unit's own journal says a flash is 14 min 51 s and
  an apply that **rolls back** is near forty minutes with both MCUs awake and the modem attached -
  near four times the load the number was argued from. Task p14-04 wrote the measured runs beside
  the constant and changed nothing: whether the floor rises (and whether a unit should refuse an
  apply it cannot finish rather than start one it cannot) is a decision with a meter on the pack,
  not a host test. The same question for `core/supervisor/supervisor.h`'s own thresholds.
- [ ] **[later] A **resumed** update is still bounded at twenty minutes.** `kUpdateBoundMs`
  (`core/cycle/wake_cycle.h`) bounds steps 3 and 9, and a run resumed from the journal that goes on
  to roll back is a flash and a proof twice - the same 38 min 40 s task p14-04 gave an apply. So a
  resume that has to roll back is cut off and carried to the wake after, once per wake until it
  gets through. Nothing is lost (that is what the journal is for) and no result is owed to anybody,
  which is why p14-04 left it alone; but it is the next number to move, and moving it means
  deciding what a wake that spends forty minutes **twice** costs the battery.
- [ ] **[later] The Walter's own apply asks for 900 seconds across a restart it cannot ask again
  after.** Task p14-04's deliverable 1 asked for the same fault to be looked for in the offer and
  in the self apply, and said the extensions between the other steps stay, so nothing was changed.
  The offer is clear: its longest stretch with no chance to extend is one range, three attempts of
  two minutes and two pauses - about six minutes against the 900 s asked. The self apply is not so
  clear. `extendLease` runs, then `switchNow()` **restarts the board**, and the next chance to say
  anything at all is the trial boot's first wake: a trial is 300 s, and a trial that is rolled back
  adds the restart into the old image and its own first wake. On 15 November that came to eleven
  minutes (660 s) against the 900 s held - a third to spare on the one call that cannot be made
  again. `kLeaseFlashSeconds` before the switch would cost nothing and would end the question.
- [ ] **[later] `FirmwareApplyProgress.json` is this end's reading of a contract, not the
  server's.** It is in `host_test/fixtures/contracts-ahead/` with the two of task p13-01, written
  from `tasks/phase-14/04-an-apply-keeps-its-lease.md`'s *The contract*. meshmaster.dk's task 227
  has the same text; when it is deployed, compare the two **byte for byte** and move the file into
  the copied tree where every other fixture is.
- [ ] **[later] The last progress line of an apply that overruns never reaches the server.** The
  uplink's deadline for the job is the apply's own bound, so a line written after that bound has
  passed - which is exactly the `rollback-booting` of a run that was cut off - is refused before it
  goes out (`host_test/agent/test_an_apply_keeps_its_lease.cpp`, the last-but-one case). A person
  watching sees `rolling-back` and then silence. It is honest and it is not wrong, and the cure is
  the one above: a bound long enough that nothing is written after it.
