# Phase 3 — follow-ups

Review findings, tagged with the task that must pick them up: `[NN]` for a task of this phase.
A session reads this file before it starts (RUNBOOK.md section 0) and treats every open item
tagged for its task as part of the task. Only the reviewer ticks an item.

No review finding was raised against tasks 01–05.

The rest of this file is task 06's handover: what the piles that come next will need, written down
where their first session will read it. Nothing here is a finding against a task of this phase, and
nothing here is tagged `[NN]`.

## For the bench pile, once the rig is on USB

The bench pile is "the sessions flash and read the consoles themselves" (`spec/SYSTEM.md` §7). What
of [`docs/bench.md`](../../docs/bench.md) it should automate, and what it cannot:

- [ ] **The console sessions that are pure text, replayed over a real port.** Three of them are
  already written as expectations and have host tests that replay them against the firmware's own
  console, so the bench pile's job is only to put a serial port where the pipe was:
  `docs/provisioning.md`'s whole session (`host_test/core/console/test_provisioning_doc.cpp`),
  `walterctl.py push` of a signed release (`tools/test_walterctl.py`, which already drives
  `host_test/console_host/`), and the command table against `help` (`test_console_secrets.cpp`).
  The expected outputs are in those tests; a `--port COM7` beside the `--console <binary>` they
  already take is most of the work.
- [ ] **`status`, `help`, `pins`, `trust`, `image list`, `self status`, `rak watch`, `wake`,
  `power`, `cycle dry` — read, not acted on.** Every one is deterministic on a unit in a known
  state, and the bench walk's §§1, 8, 10, 11, 14, 15 and 16 hold them to text. A session that can
  open the port can diff them, which turns a dozen bench items into a regression test.
- [ ] **What a session must still not do by itself**: anything with a ⚠ in `docs/bench.md`. A kill
  of the LoRa rail, a flash of the radio, opening a protected module, `rak watch reset-caps`,
  `erase confirm`. These want a person who knows what is on the desk.
- [ ] **The numbers.** The bench pile should write the measurements into
  `docs/bench.md`'s *What to bring back* table and then into the constants, one commit per number
  with the sentence beside it changed too. `tools/check_rails.py`'s entry **B** fails the build
  when a constant's mark and the table disagree, so the table cannot be left behind.
- [ ] **A `tools/bench.ps1`**, perhaps: the same `-Only <step>` shape as `verify.ps1`, over a port
  instead of a container, so that "walk §3" is a command. Decided by whoever writes the pile; this
  is only the suggestion.

## What the agent (phase 4) will need from this firmware that `Backend` does not offer

`interfaces/backend.h` is deliberately small: one exchange of what this unit knows for a closed set
of requests this firmware already knows how to carry out (`docs/cycle.md`, *The seam*). Everything
below is a thing the agent will want and the seam has no shape for. **None of it is a defect of the
seam** — the seam was drawn not to guess — but phase 4's first task should decide each one before it
writes code.

- [ ] **The mesh, at all.** `Backend` has nothing about peers, jobs, logins, CLI lines, discoveries
  or traces. The link client has a call for every one of them (`core/link/client/`,
  `spec/LINK-PROTOCOL.md` §6) and **this firmware uses none** — a grep and a simulated month hold
  it to that (rail R14). Phase 4 is where that rail changes shape, and the change should be
  deliberate: the rail becomes "no mesh request outside the agent" rather than "none at all", and
  `tools/check_rails.py`'s R14 is the line to edit.
- [ ] **An exchange that is more than one round.** `exchange()` is one call with one deadline and
  one answer. A job is a conversation — begin, poll, report, end — and either the agent runs a
  conversation *inside* one `exchange()` (which the wake's budget must then allow for) or the seam
  grows a notion of a session. The budget's arithmetic is `core/cycle/wake_cycle.h`'s and the
  backend step's bound is 60 s today.
- [ ] **Bytes, not a fixed structure.** `BackendKnowledge` and `BackendAnswer` are fixed-size
  structures with no heap behind them (there is no heap after start-up). An HTTP body of a job
  description is neither. The HTTPS client already streams a body in pieces to a sink
  (`interfaces/http.h`), which is the shape to reach for.
- [ ] **A reading the agent chooses.** The telemetry ring keeps what *this firmware* decided a
  reading is (`core/cycle/telemetry_ring.h`: battery, current, temperature, the wake's own
  record). An agent with a job type of its own will want a reading of its own, and the ring's
  record is sized to a retained slot.
- [ ] **Time, across a sleep.** `NetworkClock` is deliberately **not** carried across a deep sleep
  (`docs/ARCHITECTURE.md` §10): the monotonic clock it is kept against restarts at zero. An agent
  that schedules anything ("report at 03:00") needs a time that survives a sleep, which means an
  RTC-backed offset and a decision about what to do when it cannot be trusted. The supervisor's cap
  window had the same problem and solved it by counting rather than reading
  (`core/supervisor/supervisor.h`); that is the precedent.
- [ ] **A way to say "nothing changed".** Every wake fills `BackendKnowledge` from scratch. A unit
  that checks in hourly over a metered LTE-M link would rather send a delta, and nothing in the
  seam says what the server already knows.
- [ ] **The secret the agent will carry.** `server.token` is provisioned and read by nobody today
  (`core/settings/keys.cpp` says so in the key's own line). Rail R8 holds that it reaches no log,
  no journal, no console line and no wake record; the agent is the first thing that will put it on
  a wire, and the rail's test is where that has to stay true.

## Specification gaps the reports raised

Every item a phase 0–3 report recorded under *Deviations from the spec* or *Decisions taken without
asking* that is a question for `spec/` rather than for this repository. The specification is changed
where it lives (`twinoak.meshcore.standard-repeater-design-v2/software/spec`) and copied here with
`specsync.py`; these are for the reviewer to carry over.

- [ ] **The nRF52840's `DPIDR` is not in any Nordic document** — raised by task p1-03. The Product
  Specification says only "a standard Arm CoreSight SW-DP". `0x2BA01477` is Arm's value and what
  every debugger expects; the firmware prints the decoded fields and does not insist on the number.
  `docs/bench.md` §4 confirms it. Nothing in `spec/` needs to change; it is listed because the
  report called it a gap in a *document*, not in the specification.
- [ ] **`spec/SYSTEM.md` §10 item 1, the release key** — raised by tasks p0-02 and p2-01 and still
  open. Until Thomas makes it (`manifestref.py keygen`) and puts the private half in the release
  workflows' secret, a unit accepts no update and a tag fails at `sign_manifest.py`. This is the
  one open item that blocks a real release.
- [ ] **`spec/SYSTEM.md` §10 item 2, APPROTECT on production modules** — raised by task p1-04 and
  unanswerable without a module: `rak swd id` on the first board reads `INFO.VARIANT`, and
  `docs/bench.md` §4 asks for the answer to be written down. If modules arrive protected, the
  specification should say that opening them is part of assembling a unit.
- [ ] **`spec/SYSTEM.md` §10 item 3, the bootloader's settings page** — task p1-05 read both
  bootloaders' sources and decided: `bank_0 = BANK_INVALID_APP` before the first erase,
  `BANK_VALID_APP` with `bank_0_crc = 0` after the read-back. The specification could record the
  decision; `docs/bench.md` §5 is what confirms it on silicon.
- [ ] **`spec/SYSTEM.md` §10 item 5, the Walter's draw asleep and the interval the battery can
  pay for** — raised by tasks p3-01, p3-04 and p3-05. Every number of the interval table and every
  wake bound is a placeholder; `docs/bench.md` §§12, 15 and 16 are the measurements, and *What to
  bring back* is the list. The specification's open item stays open until they are taken.
- [ ] **`spec/LINK-PROTOCOL.md` §5.4 and a clock that cannot be trusted across a sleep** — task
  p3-05's `NetworkClock` is not carried across a deep sleep for exactly the reason §5.4 gives, and
  the radio is therefore given a time only in the wake that read one from the network. Worth a
  sentence in the protocol: a Walter that has no time is not a Walter that is broken.
- [ ] **Scoped floods** (`spec/SYSTEM.md` §10 item 4) — untouched by these piles, because nothing
  here sends a mesh request. It becomes phase 4's question the moment the agent does.

## Review of 01–06 (15 October) — the phase, after it landed

Read against all six reports, this file's handover, `docs/ARCHITECTURE.md`, S-12 and the phase-4
task files, with the code each report names checked on `main` (`fdcfac6`). Findings for the next phase are tagged `[p4-NN]` and are copied into `../phase-4/FOLLOWUPS.md` once p4-01 has merged, where a phase-4 session reads them. From
`logs/run-all.log`: 01 36 min then the session limit (WIP `b9e0cb8`), resumed and finished in
63 min; 02 120; 03 124; 04 118; 05 127; 06 86, then a stop with no report on `origin/main` — its
session had ended its turn waiting for CI with PR #21 open, which was merged by hand at 09:21 (a
driver problem, now fixed: the nudge, and RUNBOOK §4's nine-minute waits). Every PR green on its
first round. No hardware.

- [ ] **[p4-04] The bound on a wake can be skipped.** `WakeGuard` fires only when `tick()` is called —
  from `main/main.cpp`'s loop and four loops in `core/cycle/wake_cycle.cpp`. Nothing in
  `components/` or `main/` uses `esp_timer`, the RTC watchdog or `xTimer`, and the task watchdog
  watches only the idle tasks (`sdkconfig.defaults:46-51`). A step that blocks for ever (the modem
  library's `_returnAfterReply` waits on a condition variable with no timeout of its own) keeps the
  unit awake until the battery is flat. Add a backstop in `platform/` that resets the board at
  `kRaisedBoundMax` plus a margin, and a `check_rails.py` entry for it in [06].
- [ ] **[p4-03] A refused SIM PIN is forgotten on a power cut.** p3-01 kept the refusal in RTC memory
  and left moving it to NVS to p3-03; p3-03 stored only the counter (`lte_pin_gen`,
  `settings.cpp:36,75`). The flag is still in `RetainedSlot::kUplink` (`policy.cpp:206-219`) and
  `policy.h:71-75` still points at p3-03. Each power cut can spend one more of the card's three
  tries; a flat pack at dawn can PUK-lock the SIM. Write the flag to NVS beside the counter, with a
  host test across two power cuts.
- [ ] **[p4-04] Retained memory is full, and task 05 plans to use it.** RTC SLOW has 36 bytes free and
  every slot costs 140 (report 05, *What RTC memory costs, again*); phase 4 task 05 restores
  `AgentLoop` state from retained storage. Do report 05's way out 1 first — size each slot to its
  record in `platform/retained.cpp`, about 440 bytes back. (Task 04's spec line also still names
  `components/hal/`; it is `components/interfaces/`.)
- [ ] **[p4-05] [Thomas] A server can loosen the battery policy through the seam.** Step 8 applies any
  non-secret key (`wake_cycle.cpp:804-812`); `wake.interval` overrides every band but `critical`
  (`interval_policy.cpp:270-273`). `wake.interval 5` in `saving` is 288 wakes and 72 attaches a day
  where the design is about one; `wake.radio off` stops the radio's battery-critical attention from
  waking the Walter. Decide whether `wake.interval` may make `saving` worse and which keys the seam
  may set at all (`swd.khz`? `server.url`?), and hold the answer in a test before the agent sends
  settings.
- [ ] **[p4-06] Each of task 06's two rail fixes still has an escape hatch.** Pins:
  `FakePins::report_undeclared` is a public `bool` that silences the destructor check
  (`fake_pins.cpp:76`) and is not grepped by `check_rails.py`. Simulated chip: an outside write fails
  only with `strict` on (`nrf52_chip.cpp:189`); the destructor checks only that a region was
  declared; the global `outside == 0` check runs once, in doctest's file order, before
  `test_rail_sweep.cpp` and `test_rail_updates.cpp`; `strict = false` is set in
  `test_nrf52_core.cpp` and `test_nrf52_flash.cpp`. So report 06's "at every point of the run" is not
  fully true for R4. Fail any outside write in the destructor whatever `strict` says, and gate both
  flags behind a grep that allows exactly one file.
- [ ] **[p4-03] The HTTPS client sends the caller's own headers to every redirect hop**
  (`client.cpp:171-178`), another host included; only `options.authorization` is dropped across
  hosts (`:181-193`), and `sameHost` ignores the port (`url.cpp:142`). Harmless with today's Range
  and If-Range; the agent's calls will add more. Refuse credential-like names in `options.headers`,
  with a test, and compare the port.
- [ ] **[p4-05] A download resumed in a later wake does not check that the file is the same.** The
  If-Range validator lives for one download (report 02, *Deviations*), so a file replaced between
  wakes is joined to the old bytes; check 4 then erases the slot — safe, but a whole re-download.
  Keep the ETag in the store's progress record when the firmware jobs are mapped onto the store.
- [ ] **[reviewer] Documents out of step:** `docs/ARCHITECTURE.md` §12 says "`journal` one line a
  wake" where reports 04/05, `bench.md` and `field.md` say the opposite; two `kWakeBudgetMs` with
  different values (240 s in `uplink/policy.h:172`, 150 s in `cycle/wake_cycle.h:153`) — report 06
  mixes them; `kRegistrationDeadlineMs` 2 min inside a 150 s uplink step leaves 30 s for data, time
  and identity (a bench number, but say it); report 05 contradicts itself (128 entries in 1 200 wakes
  vs 810 in 2 400); report 06 has no *CI* section.
- [ ] **[Thomas] Report upstream to `dptechnics/walter-modem` 1.5.0:** `begin()` cannot be retried
  after a failure; a refused `AT+CPIN` is resent up to the attempt count (worked around with
  `CMD_ATTEMPTS=1` and a `static_assert`); `_convertDuration` gives wrong PSM/TAU values; the `+CCLK`
  parse ignores `sscanf`'s result; only socket 1 is reachable; a failed dial does not say which step.
- [ ] **[bench] Open items the reports raised:** SYSTEM §10 items 1 (the release key — [Thomas]), 2
  (APPROTECT), 3 (the bootloader settings page), 5 (sleep draw, interval table); a sentence for
  LINK-PROTOCOL §5.4 that network time is not carried across a deep sleep; scoped floods (phase 4's
  question); bench §13 item 3 (a host whose chain ends at a root this firmware does not carry must
  be refused — a 200 there is the worst finding possible); otadata *pending-verify* → *aborted*;
  cold attach time; that the modem keeps its `SQNSPCFG` profiles in NVM, which the trust record
  assumes and never reads back. 107 constants in *What to bring back*.
- [ ] **[Thomas] Housekeeping:** delete `origin/task/p3-03-settings-and-console`,
  `task/p3-05-the-wake-cycle` and `task/p3-06-the-rails-and-the-bench`. p3-01's WIP commit
  `b9e0cb8` carried a `run-all.ps1` change the session did not make (the driver fix) — intended.

### Fine as it is

- **TLS:** one `tlsConfigProfile` call (`walter_tls.cpp:138`), always full chain and host name; no
  weaker level and no `http://` anywhere, held by a test and `check_rails.py`; the trust hash covers
  PEM, slot and profile number; `ensure()` before every socket.
- **Download path (02):** the modem's TLS sockets rather than its HTTP client, with the evidence
  cited (the HTTP path gives no response headers and reads to `\r\nOK\r\n`); redirects https-only,
  at most three, `Authorization` dropped across hosts, root chosen again per hop.
- **Sleep (04):** no USB wake on the S3 (no `SOC_PM_SUPPORT_USB_WAKEUP`), sourced; the board sleeps
  itself, not via the library; ext1 with the RTC pull-up; the hold/release order cited; sleeping
  during a trial rolls the update back, and is refused — a good catch.
- **Interval policy:** hysteresis tested at ±20 mV with at most one change; only interesting events
  are journalled.
- **Thresholds and secrets:** no protective threshold is a key; secrets cannot be set over the seam;
  R8 searches a whole day, every command, the journal and every retained record.
- **Seam:** the six asks are "only what this firmware can already do"; R14 greps and counts 2 845
  requests with no mesh request among them.
- **The 39-point sweep:** 33 on the new image, 6 on the old, never half an application.
