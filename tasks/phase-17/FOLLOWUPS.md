# Phase 17 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[Thomas, upstream] The modem library keeps a debug buffer in every image.**
  `_logEscapeBuffer`, 3 080 bytes of `.bss`, is in the builds of 16 and 19 November although
  the maximum log level is INFO. The library declares and uses it under
  `#if CONFIG_LOG_MAXIMUM_LEVEL >= ESP_LOG_DEBUG`
  (`managed_components/dptechnics__walter-modem/src/WalterModem.cpp` ~L180, ~L1782, ~L2052).
  `ESP_LOG_DEBUG` looks to be an enum value, which a preprocessor reads as 0, so the test is
  always true; not checked in the pinned ESP-IDF. If so, every line the modem says is also
  escaped into that buffer for a log line nothing prints. QuickSpot's `walter-esp-idf` 1.5.0 to
  fix; not removable here without a copy of the library.
- [ ] **[later] The modem library's other static storage is 46 kB**, by the map of 19 November:
  the event queue 12 376 (eight items of about 1 547 bytes, for a ring or a close that is a few
  bytes), the buffer pool 12 352 in `.data`, the command pool 10 944, and three task stacks of
  8 192, 4 096 and 4 096. The queue's length, the pool's size and the stacks are the library's
  Kconfig. One too few loses a line or an event without a word, so the high-water marks of a
  board come first.
- [ ] **[later, found by 01] The byte bound refuses a big release before the image bound does, and
  with the wrong reason.** `kMaxManifestBytes` is 8 192 (`core/image/receive.h` ~L92) and a manifest
  filled to task 01's bounds — six images of five files — is **10 302 bytes** of canonical JSON, so a
  release of more than about four images of four files is refused for its size before the parser sees
  it, rather than at check 2 with the bound it broke. Today's biggest release is two images of four
  files (3.4 KB), so nothing is refused today. The constant was out of 01's scope; only `receive.h`'s
  comment, which claimed 8 KB was "anything this firmware could accept", was corrected. Whoever
  takes this up: the two 8 KB buffers of the item below are the same constant.
- [ ] **[later] A manifest's bytes seem to be held twice.** `fetch` is 10 104 bytes of `.bss` and
  `receiver` 9 424, and `kMaxManifestBytes` is 8 192 (`core/image/receive.h` ~L92;
  `core/update/fetch.h` ~L145: *static storage: the manifest, its signature, the URL*). The
  cable and the server are not read at the same time. Read from the map and the two headers,
  not from the code that uses them.
- [ ] **[later] meshmaster.dk's catalogue does not know this firmware's bounds.** A release of
  seven images imports there and no Walter can read it; the unit says so at check 2 and the
  page shows the refusal. Nothing warns at the import.
- [ ] **[review] `FirmwareApplyProgress.json` in `host_test/fixtures/contracts-ahead/` is not
  meshmaster.dk's file** (compared on 19 November; from pile 14). Here it is the whole post,
  `{"data":{"journal":[…]},"note":…}`; there it is the `data` object alone. The same wire
  under one name for two things, most likely; to be confirmed against what the unit posts.
  The six waiting files of pile 16 and the two of pile 13 are the same bytes as meshmaster.dk's.
  **Confirmed from the other side** by the sync pile's task 01 (20 November): the copy of
  agent-core at `v0.7.3` brings meshmaster.dk's own file for all nine of the waiting ones, and
  this is the only one whose bytes differ - in exactly that way. Nothing fails on it: the four
  tests that read the waiting files read them by name out of `host_test/fixtures/contracts-ahead/`,
  and no test or rail counts either fixture set. Moving the eight that agree over to the copy's
  own directory is a task of its own.
- [ ] **[later, found by the sync pile's 01] The kept image may grow 88 bytes before the scratch
  must.** `agent::kKeptScratchBytes` is 19 456 and the largest image agent-core can write is now
  `mm::kMaxKeptImageLen` = **19 368** (it was 19 328; upstream's p1.13-55 appended the 40 bytes of
  a pending batch's client id). The margin was 128 bytes and is 88. The figure is read off the
  library now rather than added up here, so the next growth fails the `static_assert` in
  `walter_agent/agent_backend.h` instead of quietly dropping the tail of what a sleep keeps - but
  whoever has to raise it should know that 19 456 is the top of the allocator's bucket (anything
  from 18 433 to 19 456 takes the same block), so the next byte above it costs a bigger block in
  the heap the main task's stack comes out of (rail R22, and phase 13's item above).
- [ ] **[later] Piles 12 to 16 are not reviewed.**

## The review of 01–02 (19 November)

Both merged with CI green (#85, #86); in no release yet. 02 closes the two criteria phase 16's
task 05 left open: the start FITS with the PSRAM configured, 6 348 bytes to spare where there
were 644.

Read: both reports; `components/core/net/socket_rotation.cpp` ~L10–36 (the assert that was
replaced, and why); every `std::atomic` of ours (nine objects, in `core/net/call_record.h`
~L344–346 and `socket_rotation.h` ~L234–244; none is touched from an interrupt, and this
firmware has no interrupt handler or IRAM code of its own); `components/platform/heap.cpp`
(`bringUp` ~L122, `takeExternal` ~L147, the figures read at the internal capabilities); the nine
`CONFIG_SPIRAM*` lines of `sdkconfig.defaults`. Nothing found. What the reasoning about the
atomics stands on is ESP-IDF's own `__atomic_*` for an address outside external RAM, which is in
the build's image and not in this repository: read in report 02, not checked again here.

- [ ] **[Thomas, at the bench, before the release] The PSRAM has never been brought up on a
  board.** QEMU has none, so every test of 02 scripts it. `docs/bench.md` ~L177–193: `heap`,
  `heap room`, `heap` on a spare Walter, and the line it must print
  (*room: 65536 psram of 2097152 …*). A module whose PSRAM does not answer takes the 8 192-byte
  room, and says so.
- [ ] **[Thomas, at the bench] The sleep current, once, with this image.** The chip's PSRAM
  hardware is now initialised at every boot (`CONFIG_SPIRAM_BOOT_HW_INIT`), the memory only for
  a session. Nothing here says the current asleep changed; nothing measured it either, and the
  unit lives on its battery. One reading against 0.1.9's.
- [ ] **[none] 02: every atomic of the firmware is compiled as a call now**
  (`-mdisable-hardware-atomics`), and `tools/verify.sh` builds from nothing when
  `sdkconfig.defaults` changes, which is what would have shown the failed assert before CI did.
  Read and agreed with; kept here so that the next person who meets `is_always_lock_free` false
  on this board finds why.

## The library watch, run for the first time (20 November)

- [ ] **[none] `upstream.yml` had never run past its first step.** Until `AGENT_CORE_READ_TOKEN`
  was set, every run stopped green at the credential. Run #5 (20 November, by hand) was the
  first to go on, and failed at *Is there a newer tag?*: the options were splatted as an array,
  which hands `-GitHubOutput` over as a value (*A positional parameter cannot be found*). The
  step now splats a hashtable (changed by hand, 20 November; no task). The run after it
  went through and opened pull request #87, *agent-core v0.7.3*, 63 files.
- [ ] **[none] The steps after it ran for the first time in that run:** the re-copy, the pull
  request's body, the push and `gh pr create`. All four did what they say.
- [x] **[next, the sync task of #87] The bump is red, and not by what was written here first.**
  This item said the waiting fixtures' tripwire would fail. There is no tripwire in this
  repository; that was the exit-node firmware's. Run 44695217610 stops in `host-tests` on
  compile errors: the library's loop suite, as copied, uses four members that
  `host_test/agent-uplink/shim/support/FakeUplink.h` does not have (`askForAnUpdate`,
  `updateResults`, `loseTheAnswerToObservations`, `seenBatches`). `tasks/sync/01-agent-core-v0-7-3.md`
  has it, with what else was read. The copy does bring meshmaster.dk's own file for each of
  the nine that wait in `host_test/fixtures/contracts-ahead/` (eight the same bytes;
  `FirmwareApplyProgress.json` the post's `data` member), and nothing fails on that.
  *Done in the sync phase, task 01: the shim has the four, and pull request #87 is merged.*
- [ ] **[later] The nine waiting fixtures can move now.** The four tests that read them
  (`test_unit_report.cpp`, `test_an_apply_keeps_its_lease.cpp`, `test_json_scan.cpp`,
  `test_a_session_is_a_job.cpp`) read the copied files instead, the post of
  `FirmwareApplyProgress` is made from the copied `data` member, and the waiting files go. A
  small task once the bump is on `main`.

## The review of the sync's task 01 (20 November)

Merged at 19:13 CET (#87, main 803088f), 97 minutes, five of five criteria. **The sync path ran
for the first time and worked from end to end:** the watch opened the pull request, the task
was generated, the session worked on `upstream/v0.7.3` and merged it. Read: the whole report.
No code was read. The copy is at `b6d4cfb2`, 0.7.3.

- [ ] **[none] What the session had to answer, by its report.** The shim's four knobs; nothing
  built for an update (no provider is handed to the loop, so a heartbeat is byte for byte what
  it was); `kLargestKeptImage` now reads the library's constant, 19 368 bytes in a scratch of
  19 456; the simulated RAK reflects a console line's prefix as stock firmware does.
- [ ] **[none] A bug of this firmware's own, found by upstream's new test and cured.** A line
  nobody waits for left the radio awaiting and the next send was `ERR_BUSY`, so a region map
  could not be loaded into a repeater through a Walter (since p4-02). `LinkMesh` gives such an
  exchange up before the next send. Eighty bytes, the only firmware change of the bump.
- [ ] **[later] Two of the library's 123 runner cases are not run through the link shim.** On
  this road a reply that comes late ends the exchange that is then waiting, and the job pays
  one more try, where the Heltec's wait goes on. They are replaced by two cases of this
  repository's own that say so. Whether the link should let a wait go on is LINK-PROTOCOL §6.2
  and §6.3, in the design repository.
- [ ] **[none] The image is 10 224 bytes larger** (847 744), all but 80 the library's. The heap
  at the start holds: 15 636 bytes over in the main stack's heap, 6 308 in the tightest.
- [ ] **[next] Task 15/04 can run now.** The bump did one of its three things, the scratch held
  to the library's figure. What a sleep keeps of a batch's id, and `agent obs` naming a
  waiting batch, are still its own.
- [ ] **[Thomas, on a board] The three items of `docs/bench.md` §26:** a `config.read`'s values
  under their own keys (the one that needs a real repeater: it rests on stock firmware
  reflecting the prefix), a region map loaded by a `config.apply`, and the same adverts not
  stored twice after a night of hourly wakes.

## A claim whose answer was lost (21 November)

Read from the unit's own report of 21 November 20:12Z (jobs 39366 and 39367), the first on
0.1.10+c07576d. No code was changed.

- [ ] **[next, Thomas to decide] The lost answer took a claim, and the server's jobs with it.**
  Wake 602 (19:11Z) is in the list as *timer MS-; awake 73 s*, and `agent calls` holds
  *claim, wake 602: http 200* among the calls that did not come back whole (`ModemUplink::call`
  records a 2xx whose body was cut or did not arrive whole; a claim that does not parse is not
  put in that list). An ordinary wake is 44 to 45 s with the one 30 s stall of the observations
  post; 73 s is two stalls. The server had answered 200 and leased two jobs to the unit:
  `twinoak.fw.offer` 39302 (the scanner image) and `status` 39312. The unit never held either.
  The offer has one attempt and ended *failed: the exit node took this job and never reported a
  result*; the status waited out its lease. The same shape as job 32763 on 17 November (*taken
  twice, and no answer came before it expired*). The counter reads 28 claim failures in 1 628
  heartbeats.
- [ ] **[next] What would make it harmless**, as rules and not guesses: a claim that hands back
  the leases this exit node already holds and has not reported (meshmaster.dk), and one more
  claim in the same wake when a claim came back 2xx without its body (here). Offered, not
  written.
- [ ] **[later] The list says *http 200* and not which of the two it was.** `noteFailedCall`
  keeps the status and the outcome; *cut at the bound* and *did not arrive whole* read the same
  in a report, and the sentence that told them apart was a log line the sleep took.

## The first session on hardware (21 November, 22:20Z to 22:41Z)

Session 1 on the unit at Bellevue (meshmaster.dk job 39506, `/noisescope/1`), Walter
0.1.10+c07576d, radio and scanner 0.1.6+fcf52d5. Read from the session's journal, the unit's
report of 23:23Z and the code. No code was changed.

- [ ] **[next] The golden image is proved while the scanner's watchdog is still running, so
  every session ends `stuck`.** The journal: *flashing-golden* 22:30:38, *booting: the radio was
  reset* 22:39:52, *proving* 22:39:54, **stuck: the radio restarted inside the proving period**
  22:40:53. The report an hour later: `radio status: uptime 2570 s, booted: 3 (watchdog)`, *the
  radio is healthy; the supervisor is idle*. The scanner starts the nRF52840's hardware watchdog
  at 60 s (`src/roles/scanner/Wdt.h` and `config.h` ~L116–136 in the repeater firmware: *cannot
  be stopped and survives the soft reset*), with `HALT_Pause`, so it stands still while the core
  is halted for the flash. The flasher leaves by `Nrf52::systemReset()`, which is
  AIRCR.SYSRESETREQ (`components/core/flasher/flasher.cpp` ~L508–520,
  `components/core/nrf52/nrf52.cpp` ~L291–304): a soft reset, which does not reset that
  watchdog. The repeater feeds nothing (*upstream's own loop has no watchdog*), so it is reset
  59 s after its first boot, and a restart inside the three-minute proving period is a failure by
  rule (`radio_update.cpp` ~L731–741). The watchdog's own reset clears it; the repeater has run
  since, adverted under its own key at 22:41Z and answered an admin login over the mesh at
  23:22Z. Nothing was lost but the verdict. **What a fix has to decide:** a reset that clears the
  watchdog after a flash over a scanner (the reset line, with the spacing rule of
  `LINK-PROTOCOL.md` §10 and debug interface mode ended first), or a proof that begins at the
  boot after the watchdog's. Offered, not written.
- [ ] **[none] What worked, for the record.** The PSRAM's first use on a board: *room: 65536
  psram of 2097152; internal 32748, block 12288, least 30604*. Six steps, 284 lines,
  101 037 bytes, every scope step its full 60 frames. The flashes took 4 min 48 s (the scanner,
  127 kB) and 9 min 14 s (the golden image, 424 kB); the repeater was off the mesh 19 min, where
  the page says about 37.
- [ ] **[later] The scanner was never given the time.** Its `info` line says `rtc:0`, every line
  carries `ts:0`, and meshmaster.dk places the windows *by the time since the scanner started*.
- [ ] **[later] The scanner says `reset: por`** after a flash over SWD and a SYSRESETREQ.
- [ ] **[later, Thomas] Whole frames lit across every frequency**: 14 of 60 frames in the
  400 kHz scope with a median of -88 dBm where the others read -125.5, peaks to -32 dBm; 11 of
  60 and 4 of 60 in the two wider ones; the dwell on 869.618 MHz has a strongest second of
  -33 dBm. Most likely the unit's own LTE beside the antenna; not checked against the modem's
  activity. If so, the repeater hears nothing while the Walter sends.
- [ ] **[next] The unit took the session up again an hour later and restored it, and the server
  was never told.** Its journal, read on 22 November (report 39710/39711): seq 1084 *session,
  stuck*; seq 1085 wake 608, *attention*, 90 s, nothing done about the session; then at the
  timer wake 609 (00:24Z, awake 611 s) seq 1086 to 1094: *check - switch back to the repeater*,
  *rolling-back*, *flashing-golden*, *booting*, *proving*, *done (the health check passed
  182964 ms of proving)*, **session, restored slot 0**. So a session that ends `stuck` by the
  watchdog costs the repeater a second outage of about seven minutes at the next timer wake and
  then is right. meshmaster.dk still reads *Stuck: the repeater's own image did not come back*:
  the job's one result was `stuck` and nothing posts the later ending.
- [ ] **[later] A wake a person asked for does not take an open session up; the timer's does.**
  Seen once (wake 608 against 609); not read in the code.
