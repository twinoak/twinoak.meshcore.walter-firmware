# Phase 8 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–06 goes below them when it
is written.

## Found while doing 01 (6 November)

- [ ] **[reviewer] A first install whose *flash* failed says `no image to return to`.**
  `RadioUpdate::fail` (`components/core/update/radio_update.cpp`) is given the real reason -
  `kFlashFailed`, with the flasher's step in `detail` - and then, for `Kind::kFirstInstall`,
  enters *stuck* with `describe(Reason::kNoImageToReturnTo)` as the note and
  `Reason::kNoImageToReturnTo` as the reason. So the console's last word on 6 November,
  `rak update: STUCK: no image to return to - the radio has no proved application`, was true of
  the unit's situation and said nothing about *why the flash failed*, which was the thing worth
  knowing. `rak update status` still shows the step. Either carry the real reason into the entry
  and print both (*the flash failed at `<step>`; there is no image to return to*), or say in
  `docs/update.md` that on a first install the note is the situation and not the cause. Not fixed
  in 01: it is a second finding, and 01's business was the watchdog.
- [ ] **[reviewer] Nothing counts a journal step that has reset the Walter before.**
  `WakeCycle::stepResume` resumes an unterminated run at every wake, and `RadioUpdate::resume()`
  drives *flashing-candidate* into the flash again - which is right (`docs/update.md`, *After a
  power cut*) and is what repeated the 6 November fault nineteen times instead of once. The only
  thing that noticed was the interval policy, which lengthened the sleep
  (`core/power/interval_policy.cpp`, `faults_in_a_row`, bounded by `kMaxFaultDoublings`). A
  resumed step that ends in a watchdog or a panic every time is indistinguishable, from inside
  the journal, from one that is merely slow; whether the journal should carry "this entry has been
  resumed N times" and refuse at some N - or whether the supervisor's report should say it - is a
  design question, not a bug. 01 made this particular step stop resetting the Walter; it did not
  give the machine a way to notice the next one.

## Found while doing 04 (7 November)

- [ ] **[reviewer] A firmware job for the unit's own repeater still does a `JOB_BEGIN` on the link
  while the radio has no identity.** The runner begins every claimed job on the mesh before it
  hands it to the extension, and `LinkMesh::beginJob` can only call it local when `setOwnKey` has
  the key. So a `twinoak.fw.state` or a `twinoak.power.cycle` claimed in a wake that has no key -
  the two cases task 04 is about - spends one §6.1 `JOB_BEGIN` naming the unit's own key as a
  stranger's. **Nothing goes on the air** (`JOB_BEGIN` names a peer and sends no packet, which is
  why task 04's tests count only the `SEND_*` requests) and on a silent radio the request simply
  fails, so this is one wasted link request per firmware job and not a fault. Whether a firmware
  job should begin on the mesh at all - it touches this unit's store and its two MCUs, not a
  neighbour - is a question for the extension's seam and not for task 04, whose business was the
  claim.
- [ ] **[reviewer] A timer wake whose step 2 found the radio silent spends one more `HELLO`.**
  `Identity::open()` asks whenever there is no session, and after a step 2 that classified the
  silence (and may have climbed the ladder) there is none. Task 04 deliberately kept one path
  rather than a branch on `known.wake.radio`: the extra cost is the link's three tries on a UART
  in a wake that has just paid for an LTE attach, and after a rung that power-cycled the radio an
  ask that finds it back is worth more than the tries it costs. If the figure on the bench says
  otherwise, the knowledge to skip it is already in `BackendKnowledge::wake.radio`.
