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

## Review of 01–06 (7 November)

An independent read of the code and tests against the task files; nothing built, no port opened.
**No code fault that changes what the board does this morning.** Verdicts:

- **01:** met. `letIdleTaskRun` (`delayMs(10)`, two ticks) sits before each page read and each write
  segment of the flasher, and in `rak provision`'s `flushRun`/`compareRun`; always between transfers.
  Polls are bounded at 300 ms or less; the watchdog's settings are unchanged. The fake clock keeps the
  board's tick rule.
- **02:** met. `writeBytes` writes verbatim, is bounded at 1 s and false when short; the conversion is
  one helper (`crlf.h`) used by the board and the console host; the hello says protocol 2 and walterctl
  undoes the conversion only for 1 over a serial port. The reviewer's `UndoCrlf` and `SerialPort.read`
  are kept.
- **03:** mostly met. `SocketRotation` in `core/`, the modem layer thin; the second id is 6, which the
  library allows. Late closes one and two connections back are tested, and so is a genuine early close.
  See S2 and S3.
- **04:** met for the claim. `Identity::open()` runs before `loop.wake()`, `supports()` needs the key,
  and meshmaster.dk routes by the heartbeat's job types, so no mesh job is leased and then refused.
  See S1.
- **05:** met. The band is read only from a 1.1 body and trailing bytes are ignored. Byte for byte
  with the repeater's e752e09; both fixture files are identical across the repositories, and 1.0's is
  unchanged since v0.1.0. `band` sits in `slots[]`, as `PickImage` reads it.
- **06:** met. `v0.1.1` → a85ca83 (lightweight, as `docs/release.md` says); `verify_release.py` passes
  on the downloaded files. The 179 minutes were CI and the release workflow (about 25 minutes of
  writing), with no failure and no retry. *A unit at 0.1.0* in `docs/flashing.md` is correct.

- [ ] **[reviewer, done in part] S0 · The morning's step 2 was wrong.**
  - This README said the stored v0.1.0 candidate would be flashed, and so does the published
    `release-notes/v0.1.1.md`. It cannot be: `RadioUpdate::fail` marks the failed image bad before it
    enters *stuck* (`radio_update.cpp`, about line 266), `rak update` refuses it (about line 436), and
    the store refuses it pushed again (`store.cpp`, about line 484).
  - The README is corrected. **The GitHub release body needs Thomas's edit.**
  - Row 2 of the same notes blames the 2.5-hour push on the conversion; it was walterctl's
    `SerialPort.read`.
- [x] **[done by p9-03] S1 · A scanner or an empty slot now counts as a dead radio.**
  - `agent_backend.cpp` (about line 223) sets `radio_silent = !radio_up || …`. A slot running
    NoiseScope, or awaiting its first install, has no frame session either, and is now "link known
    down", which `supervisor.h` (about lines 345–347) rules out on purpose.
  - That lifts the link-down and battery-unknown refusals of a `twinoak.fw.apply`
    (`radio_update.cpp`, about lines 458 and 477).
  - It also marks the radio's condition *not applicable* in a self-switch confirmation
    (`self_update.cpp`, about lines 218 and 464–467).
  - Untested. It does not affect this morning.
  - **Done by task p9-03.** The two facts are apart. *No key to tell a local job by* stays where
    p8-04 put it - the identity read before every wake, and the one line that reports it. *The
    link is known down* is the supervisor's, and there are now **two questions with one answer
    each**, beside the enumeration they are about (`core/supervisor/supervisor.h`):
    `linkKnownDown()` is `kSilent` **alone**, which is what an apply's `HELLO` refusal and its
    battery-unknown refusal read; `radioKnownDown()` is the three classified silences and **not**
    `kUnknown`, which is what a self-switch's trial reads. `WakeCycle` and `FirmwareJobs` both
    call them - the jobs off the `Supervisor` they were already given - and `WakeContext` carries
    nothing about the radio's health at all.
  - Tested, where it was not before: `host_test/core/supervisor/test_known_down.cpp` is the table
    over all five healths (as `static_assert`s too, and an invariant tying the two questions), and
    `host_test/agent/test_radio_health_and_the_refusals.cpp` walks the classifications end to end
    through the agent - what the claim takes, what an apply refuses, what a self-switch's trial
    says of the radio - including the two cases where the key and the classification disagree.
    **Eleven assertions of it fail on the code of 7 November** (`out/experiments/p9-03-before.txt`).
  - One thing the review's third bullet turned out not to be: a *scanner* never reached
    `self_update.cpp`'s waiver, because a scanner answers in text and `switchNow` counts that as
    up. What did reach it is any wake whose `HELLO` had simply gone unanswered - the console's
    `agent wake` on a radio that died after the last reading - and that is the case the new test
    holds. `radioKnownDown` gained `kAwaitingFirstInstall`, which the wake cycle's own expression
    did not have: a slot with a bootloader and no application cannot answer a trial's `HELLO`
    until it is flashed, and holding the trial to one would roll back every Walter image a freshly
    provisioned unit is offered (report p9-03, *Decisions taken without asking*).
- [x] **[done by p9-02] S2 · A close that never comes leaves a socket id one close behind for the
  modem's life.** If the modem sends no `+SQNSH` after our own `AT+SQNSH`, and our close goes out before
  the server's FIN, `close()` → `end()` leaves a debt nobody pays. Every later connection on that id then:
  - waits 250 ms for nothing;
  - has its real early close discounted, so a dropped body reads as a timeout.

  The counters (`owedTimeouts`, `discountedCloses`, `otherIdEvents`) are printed nowhere; show them in
  `http get` or `lte`. `kOwedCloseWaitMs` is still *TO BE SET ON THE BENCH* (`docs/bench.md`).
  - **Done by task p9-02.** A debt now lives for **one connection**: if it is still unpaid when the
    connection that waited for it has finished its exchange, it is written off and that id is
    marked as one whose closes do not come - nothing is owed on it and nothing is discounted until
    a close actually arrives, and when one does the rule comes back. So the whole cost of a lost
    `+SQNSH` is one 250 ms wait and one write-off, per id, once. The second half of the finding -
    a real early close discounted, so a dropped body reads as a stall - cannot happen after the
    write-off. `host_test/core/net/test_late_close.cpp`, *a modem that never answers a close*, is
    eight connections through a socket layer that reports none: two waits, two write-offs, nothing
    after. The counters, `writeOffs` among them, are on `http get` and `lte`.
  - `kOwedCloseWaitMs` is still 250 ms and still *TO BE SET ON THE BENCH*; report p9-02 has what
    the library says it has to cover (the event task dispatches one event per ~20 ms).
- [x] **[done by p9-02] S3 · A check-then-set race in `SocketRotation`.** `end()` (main task) and
  `onClose()` (the library's event task) read and set flags without a lock. One interleaving leaves both
  set, which is S2's permanent debt. The window is narrow; *nothing to tear* covers torn writes, not
  this. A critical section.
  - **Done by task p9-02**, and not with a lock: the two flags became **one value** per id
    (`CloseState`, `core/net/socket_rotation.h`) and each of `begin()`, `end()`, `cancel()` and
    `onClose()` applies exactly one transition of it with a compare-exchange. There is nothing
    left between a read and a write to interleave, "both set" is not a state the value has, and a
    `static_assert` holds the value to one the hardware changes in a single instruction. The table
    of transitions is a `constexpr` function the test reads in full, and the interleaving the
    finding describes is in it both ways round: *every transition of the rule, and both orders of
    the two tasks' steps* (`host_test/core/net/test_socket_rotation.cpp`).
- [ ] **[Thomas] S4 · Any failure bans an image for ever.**
  - `fail()` marks the image bad for any reason, an SWD flash failure included.
  - The console then says *failed a health check*, and no command clears the eight-entry list.
  - That is how a good v0.1.0 was banned on 6 November: per report 01 the stuck run ended on a flash
    failure, after the watchdog loop.
  - Decide: does a flash failure ban the image, or only a failed proof; and is there a person's command
    to clear one hash?
- Nits:
  - p8-01's *before* figures are only in its report (`out/experiments/p8-01-before.txt` is not on disk).
  - `fake_tls_socket.cpp`'s `endConnection` comment is stale. (Corrected by task p9-02.)
  - Report 06 lists three of the five files' hashes: the `.sig` is 0c7aa12f…ff69, `SHA256SUMS`
    5d2c9840…53e3.
  - `docs/bench.md` §9 says *the run that the watchdog ended*; it ended on `kFlashFailed`.
    (Corrected by task p9-04, with report p8-01 named beside it.)
  - 03 relies on the library's unchecked `_socketGet(id)->state` (`WalterModem.cpp`, about line 3205).
    (Read again for p9-02: `_socketGet` matches on the id **alone** at 1.5.0, and that is
    deliberate upstream - QuickSpot/walter-arduino#109 was a panic when it also required the
    state not to be FREE, and #110 removed the state test. So the entry is found whatever the
    `+SQNSH` handler has done to its state, and the null dereference needs an id no entry carries,
    which this firmware cannot dial. The trace in #109 is the 7 November sequence in miniature:
    `AT+SQNSRECV=1,318`, then `+SQNSH: 1`, then `+SQNSRECV: 1,318`.)
- [x] **[settled by meshmaster.dk's 173: the restore is gated on the level the claim carries, and the unit is now handed none] No `setperm` after a local job.** The review
  of meshmaster.dk's 4.25 found agent-core's `restoreAcl` (`components/agent-core/src/JobRunner.cpp`,
  about lines 404–450) sending `setperm <own key> 1` to the unit's own repeater after every `acl` job:
  the local door answers the login at admin, and the runner puts the level it was handed back. Its
  173 stops handing the unit an `acl` level for its own repeater. Should agent-core restore anyway
  whenever it logged in at admin, the Walter skips the restore on a local job
  (`host_test/agent-link/test_link_mesh.cpp`, about lines 685–686, asserts it today).
- [x] **[done by p9-04] A first install's `check` entry journals a zero hash.** 7 November, `rak update
  status` on the first unit: `seq 28: check - first install, slot 0, image sha256 0000000000000000...`.
  The journal's story of the 6 November run never names the image it was about. The image is only on
  the bad list (`003a8895…`, repeater v0.1.0). Read whether `RadioUpdate` journals `check` before it
  has loaded the candidate's record, at 0.1.0 and at 0.1.1, and make the first entry carry the hash.
  - **It did, at both tags.** `start()` is `clear()` - which zeroes `status_` - and then
    `enter(State::kCheck, …)`, and every entry carries `status_.sha256`; the field is filled by
    `stepCheck`'s `loadImage`, one step *after* that entry is written. The two tags' `start()` are
    identical here, so every run of both releases named 64 zero bits in its first entry.
  - **Done by task p9-04:** `start()` reads the record of the image the run is about - the record
    alone, no reader opened and nothing verified: that is `stepCheck`'s business and its
    refusals are unchanged - and the hash and the version are in `status_` before the `check`
    entry is written. A slot with no record leaves the hash zero and `stepCheck` refuses with
    *no candidate*, which is the honest entry for a run about nothing.
  - Three tests in `host_test/core/update/test_radio_update.cpp`: an update's first entry, a
    first install's, and the empty slot's.
- [x] **[bench, 7 November] The first install worked on the board.** Walter 0.1.1 (factory flash by
  cable), repeater 0.1.1+3d81536 pushed by `walterctl` (protocol 2, no `--undo-crlf`), then `rak update
  --first-install`: 102 pages, read back whole, the radio reset, *proving*, *committing*, `done: … is the
  golden image`, with no Walter reboot. It took about a quarter of an hour at 125 kHz between Thomas's
  two console pastes (19:31–19:49 local), where `docs/flashing.md` says *seven minutes or so*: time it
  properly with `docs/bench.md` and correct the figure.
  - **The figure is corrected by task p9-04**, from the code's own page and poll counts rather
    than from a guess: 413 s for the flash of 417,284 bytes in 102 pages at 125 kHz (548 of
    p9-01's pauses in it, 5.5 s, about 1 %), up to 20 s of boot window and 180 s of proving -
    **about eleven minutes**, of which seven are SWD traffic. `docs/flashing.md` has the table
    and the bench's quarter of an hour beside it as the line still to fill; `docs/bench.md`
    §9 is where the flash alone is to be timed.
- [x] **[done by p9-04] *the clock check was skipped: this Walter has no time to give until phase 3's
  modem*.** The modem has been in since phase 3. The check was skipped because the uplink was not up in
  this boot, so there was no network time to give. Say that, and say that `lte up` before a
  `rak update` lets the check run. Or have the update ask the uplink for the time when it can.
  - **Done by task p9-04, both halves - because the second sentence was not true yet.**
    `Console::lteUp` printed the network's time and gave it to nothing: only the wake cycle's
    step 6 ever filled `core::cycle::NetworkClock`, so a boot a person drove by hand had no clock
    of its own however long the uplink had been up, and `lte up` before `rak update` would not
    have let the check run.
  - So `lte up` now hands the time it read to that object - the same `interfaces::NetworkTime`
    through the same window, one place still deciding what to believe and it is still the
    network - and says so: *this Walter's clock is set from it, so the radio can be given it
    too*. The update is given no uplink: it reads `interfaces::WallClock` as it always did.
  - The message says what is true: *the clock check was skipped: this Walter had no time of its
    own in this boot, so there was none to give the radio. 'lte up' before 'rak update' lets it
    run*, in `docs/update.md`, `docs/provisioning.md`'s replayed session and `docs/console.md`.
  - Tested: `host_test/core/cycle/test_wake_cycle.cpp`, *`lte up` at the console gives this
    Walter a clock, so the link has a time to give* - and the link then sends a real `TIME_SET`.
- [x] **[read by p9-02; what is left is on the bench] The fifth connection of a wake still stalled.** 7 November,
  Walter 0.1.1 on the first unit, `agent wake`:
  - config, heartbeat, claim and the job's result all went through (four connections, about 1.1–1.3 s
    each);
  - then `WalterModem: Command time-out (TX) Attempt 1 of 1` and `/agent/v1/observations -> no reply:
    the connection closed before the body was complete after 30935 ms`. That is the same symptom as
    6 November, on a later connection.
  - Two `http get`s just before had made four back-to-back connections cleanly (302 then 200, twice).

  Read it against S2 (an owed close never paid) and S3 (the check-then-set race), and against the
  library: which AT command timed out, from the trace build. Print `SocketRotation`'s counters on
  `http get` and `lte`, so the next one shows which id and whether a close was owed.
  A second wake (19:05 UTC) had the same shape:
  - heartbeat, claim, two results and a first `/agent/v1/observations` (4 adverts, 200) all went
    through: five connections;
  - the **second** observations batch then hit `Command time-out (TX)` and *closed before the body
    was complete* after 31055 ms.

  So twice the stall has come on the connection after five good ones. Check whether that is the
  count (the ids alternate, so the sixth reuses the third's) or the second batch in a row.
  - **Read by task p9-02, as far as the code can settle it.** The stall is **one AT command that
    got no answer for the library's whole 30 s command timeout**, and from the shape of the
    failure that command is `AT+SQNSRECV`: a send that had failed would have said *the request did
    not go out*, and `r.status == 0` says not one byte of the response had arrived. 30 935 ms is
    that timeout plus the connect. **Which id it was on cannot be read from the console lines
    there were**, which is why the rotation's counters are now printed. Two things were fixed on
    the way: the sentence (*the connection closed before the body was complete* was printed for a
    modem that had stopped answering, which is a different fault and now says so -
    `HttpOutcome::kSocketFailed`), and the two ways a socket id can go wrong that the review had
    named (S2 and S3 above), either of which could have put a connection on an id with a close
    owed on it.
  - **What is left is the trace**, and `docs/bench.md` §13 has the build, the three things the
    trace must show and the lines that get them. Report p9-02 has the reading of the library
    behind them: the event queue is eight deep and drops what does not fit, the event task
    dispatches one event per ~20 ms, and a `+SQNSRECV: <id>,<n>` whose declared length exceeds the
    bytes that follow it leaves the library's parser counting the rest of the UART as that payload
    until the command timeout clears it - which would explain a single 30 s gap with the next
    connection going through in 3.7 s.
- [ ] **[next Walter pile / agent-core] A local job is paced like the air.** 7 November: `telemetry` for
  the unit's own repeater went through the local door, `ok after 2 transaction(s)`, and took 20 s,
  which is the server's `gap 20 s` between two transactions. Neither went on the air (the local login
  and `LPP_GET`). The airtime budget is the mesh's, so a local job should neither spend it nor wait
  for it.
- [x] **[done by p9-02] `DEPRECATION: Use socketConfig(socket_id, pdp_ctx_id, mtu, exchange_timeout,
  conn_timeout, send_delay_ms, rsp, cb, args) instead`** on the first connection of a boot. Move
  `walter_tls.cpp` to the library's current signature.
  - **It already is.** Every `socketConfig` this firmware makes is the current overload; the line
    comes from the **one** deprecated call it makes on purpose - the overload with no id, which is
    the only public way to `_socketReserve()` and so the only way to a second socket id at 1.5.0
    (`components/modem/include/modem/walter_tls.h` has the short reading, report p8-03 the long
    one). Nothing else of the library's deprecated surface is called.
  - **Done by task p9-02:** `walter_tls.cpp` silences the library's `DEPRECATION` tag where that
    reserve is made, with the reason beside it. The line goes; the second id stays.
- [x] **[done by p9-04] The local door's counters are printed nowhere.** `LinkMesh::linkCounters()`
  (`local_jobs`, `local_silent`, `local_requests`, `local_unserved`) is not on the console. On
  7 November a telemetry job for the unit's own repeater came back *unreachable after 2
  transactions*. The cause was meshmaster.dk addressing the radio's **old** key (it changed on
  6 November: repeater firmware `tasks/phase-3.8/FOLLOWUPS.md`), so the job was a stranger's and went
  on the air. Only the server's audit log could tell. `agent id` or `agent jobs` should show whether a
  job went through the local door, and should say when a job's key is not this radio's.
  - **Done by task p9-04**, in both places and for both questions.
  - `agent id` ends with a `local door:` line: *open: a job naming this radio is answered here*
    or *shut: no key for this radio, so no mesh job is carried at all*, and the four counters.
  - `agent jobs` ends with two blocks the library's own report cannot write: `local` (the same
    four, and this radio's key shortened) and `begun` - **the last ten jobs by the repeater they
    named**, each *the local door: nothing went on the air* or *the mesh; NOT this radio's key
    …*, with both keys shortened to twelve hex characters. `components/agent-core` is a
    hash-checked copy of upstream, so the list is kept at the end that decides where a job goes:
    `LinkMesh::beginJob` (`agent::JobBegun`, seventy bytes of that object).
  - Tested: `host_test/agent-link/test_link_mesh.cpp` for the list itself, and
    `host_test/agent/test_agent_console.cpp` for the two commands' words, with a job for a
    neighbour beside one for the unit's own repeater.
  - One thing found on the way and fixed: `agent jobs`'s `firmware` line was a single `sayf` of
    more than the 200 bytes a line is printed into, so it had been cut at *0 pow* and had lost
    its newline. It is three lines now.
- [ ] **[next Walter pile / agent-core] An admin job for the unit's own repeater needs a password nobody
  reads.**
  - agent-core decides what to log in with before it knows a job is local. `loginMaterialFor`
    (`LoginSequence.cpp`) answers `nullptr` for an `admin`-class job whose claim carries no password,
    and `ensureSession` then reports `unauthorized` without sending anything.
  - So meshmaster.dk still hands a stored admin password to the unit for its own repeater (its 173,
    `AgentProtocol.cs` `DecideAsync`, *Not at `admin`, yet*), and **refuses** the job when none is
    stored.
  - 7 November: job 22016, the scheduled `config.read` of the re-registered repeater 41, failed with
    *no admin password stored*. The radio's key and settings had been lost, so its admin password is
    a random one nobody knows (S-13).
  - Every CLI job of the unit's own repeater (`cli`, `reboot`, `config.apply`, `clock.sync`,
    `advert.request`, `acl.set`, `password.set`) is `admin`-class.
  - **Fix:** skip the material check for a job the local door will answer. Then meshmaster.dk drops
    its exception and hands nothing at any class.
- [x] **[p9-01 did fix 1 and fix 3; fix 2 is p9-05's] A busy wait below one tick still starves the watchdog: a config read
  resets the Walter.** 7 November, 19:12 UTC, Walter 0.1.1, `agent wake` with job 22032
  (`config.read`, the unit's own repeater 41, 34 lines through the local door):
  `task_wdt: … IDLE0 (CPU 0) … CPU 0: main … Aborting`, a core dump to flash, a reboot.
  - **The backtrace**, decoded against `out/images/twinoak-walter.elf` (the same code, built
    04:17 that morning):
    `EspClock::delayMs` (`clock.cpp`:21, the busy branch) ← `WalterPlatform::delayMs`
    (`walter_platform.cpp`:109) ← `JobRunner::idle` (`JobRunner.cpp`:112) ← `JobRunner::spend`
    (:146) ← `cliLine` (:338/381) ← `readRegions` ← `runConfigRead` ← `runClaimed` ←
    `AgentLoop::wakeJobs` ← `AgentBackend::wake` ← `Console::agentWake`.
  - **The cause.** `idle()` waits out the airtime budget with `delayMs(1)` in a loop.
    `WalterPlatform::delayMs`'s own comment says *a yield and never a busy spin*, but
    `EspClock::delayMs(1)` is `esp_rom_delay_us(1000)` below one tick (`CONFIG_FREERTOS_HZ` 100).
    The unit's budget, *2 a minute, gap 20 s*, makes the third line of a config read wait up to
    about 40 s, which is past the 30 s watchdog. A `telemetry` (one 20 s wait) survived the same
    path.
  - **This is p8-01's fault in another loop.** p8-01 fixed the flasher's loop, not the clock.
  - **The fix, in this order:**
    1. **At the root:** `EspClock::delayMs(n ≥ 1)` blocks, never spins. Either
       `CONFIG_FREERTOS_HZ=1000`, so that 1 ms is a tick (check every `pdMS_TO_TICKS` and p8-01's
       `letIdleTaskRun` against it), or a `vTaskDelay(1)` floor. Keep busy waits for
       `delayUs` and the SWD wire only.
    2. A local job neither waits for nor spends the airtime budget (the item above, *A local job is
       paced like the air*). That alone would have saved this job.
    3. **A host test with the fake clock's tick rule** (p8-01's): a 34-line local `config.read`
       at a budget of 2 a minute never runs more than 2 s of simulated time without a blocking
       wait. It fails before the fix.
  - **Until it is released:** no `config.read` or `config.apply` of a unit's own repeater (on
    meshmaster.dk: cancel the job, and turn the repeater's scheduled config off). A queued one is
    retried at each wake, and resets the Walter each time.
  - **Done by task p9-01** (PR of `task/p9-01-a-delay-that-yields`), in the order the fix was
    written in:
    - **fix 1:** `CONFIG_FREERTOS_HZ=1000`, so a millisecond is a tick and `EspClock::delayMs(n ≥ 1)`
      is always a `vTaskDelay`. The alternative - a `vTaskDelay(1)` floor at 100 Hz - was not taken,
      because it would have multiplied four poll-counted timeouts by ten; report p9-01 has the
      choice and its costs. A `static_assert` in `platform/clock.cpp` holds the tick rate.
    - **fix 3:** `host_test/agent-backend/test_agent_watchdog.cpp` - the 34-line local
      `config.read` at *2 a minute, gap 20 s* through the real `JobRunner` and the real `LinkMesh`.
      It measured **1,020,000 ms** without a blocking wait before the fix and **0** after.
    - **fix 2 is not done here** and is still the thing that would have saved this job: it is the
      exit-node firmware's task 40 and this phase's task 05.
    - The watchdog is unchanged (30 s, panic, both idle tasks).

