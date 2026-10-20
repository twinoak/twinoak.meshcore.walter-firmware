# 01 — A bench a session may use

**Depends on:** phases 4 and 5.
**Spec:** [`tasks/RUNBOOK.md`](../RUNBOOK.md) §2 and *Phase 6* (what a session may now do, and only through this tool); [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5, S-5a, S-10, §7; `docs/bench.md` (the walk, its ⚠ and ★ marks, *What to bring back*); `docs/provisioning.md` (which settings are secrets); the hardware repository's `hardware/pcb/BENCH-BREADBOARD.md` §6 caveats 1–4 and 10 (back-feed, reset spacing, the S3's power-up glitch, the USB console that vanishes in deep sleep); the repeater firmware's `tools/bench/` at its newest `main` (`bench.py`, `config.py`, `lock.py`, `ports.py`, `runs.py`, `walk.py`, `test_bench.py`) and its `docs/bench.md` §0a–§0b — read with `git -C ../twinoak.meshcore.repeater-firmware show main:<path>`, never written; this repository's `tools/walterctl.py`, `host_test/console_host/`, `components/core/console/commands.cpp` (every command and its `confirm` form), `components/core/settings/keys.cpp` (which keys are secret), `partitions.csv`, `docs/release.md` (the `ota` and `factory` images), `tools/requirements-verify.txt`.
**Size:** one session, long.
**Model:** claude-fable-5-1

## Goal

Until now a session could build and run host tests, and everything else was Thomas's evening. This
task gives sessions the rig — through one tool, `tools/bench/`, which finds the Walter by its own
serial number and nothing else, puts an image on it without losing what Thomas typed into it, drives
its console a line at a time, and **refuses, before a port is opened, every line `bench.local.json`
does not allow** — the ⚠ steps, the modem, the server, and every secret, always. The repeater
firmware built the same thing for the radio's USB in its pile 3.3; this is its twin for the Walter,
and the two take the same lock per radio.

## Deliverables

- **`bench.example.json`** (committed) and **`bench.local.json`** (git-ignored, this machine's):
  | Key | What |
  |---|---|
  | `walter_usb` | the Walter's USB serial number as its USB-Serial/JTAG port reports it (the S3's MAC; how to read it on Windows in the `_about`). The only way the tool finds the Walter: never "the only COM port" |
  | `walter` | `keep`, or `blank` — only `blank` lets the tool write the **factory** (merged) image, which erases NVS: the SIM PIN, the token, every setting. Otherwise images go on without touching NVS |
  | `radio` | `keep`, or `disposable` — only `disposable` lets a session send `rak provision … confirm`, `rak flash-test confirm`, `rak update …`, `rak role …`, `image erase`: the ⚠ lines that can cost the radio its identity |
  | `rf` | what is on the LoRa port: `dummy-load`, `antenna` or `nothing`. A **repeater** image goes onto the radio only on `dummy-load` (the repeater firmware's own rule: a test repeater on an antenna is a stranger on the mesh); the scanner, which never transmits, on any |
  | `sim` | `true` if a SIM is in and `lte.pin`/`lte.apn` are set by Thomas: only then `lte up`, `lte time`, `lte id`, `fetch`, `trust tls`, and `http get` **of `server.url`'s host only** (a session never fetches an arbitrary URL over a data plan) |
  | `server` | `true` if `server.url`/`server.token` are set by Thomas: only then `agent probe`, `agent wake`, `cycle now` |
  | `rak_usb` | optional: the radio's USB serial when its USB-C is on this machine too — to see the UF2 drive after §4a. Only valid once the radio has its base: the Adafruit bootloader and the repeater report the FICR `DEVICEID` as `%08X%08X` of `DEVICEID[1]` then `[0]` (upper case), RUI3 does not. The tool checks that it equals the `DEVICEID` `rak swd id` reads, and refuses to run if it does not |
  With no file, or no port with `walter_usb`, every command exits **3, *no bench*** (a task of
  phase 6 then stops; see the phase README).
- **The refusals, a table in `tools/bench/policy.py`**, each line of `commands.cpp` named with what
  allows it — and these refused **always**, in every configuration: `erase confirm` (it wipes the
  secrets; Thomas's), any `set` of a secret key (`keys.cpp`'s secret flag, read from the source, not
  a list typed here), `rak kill` (§6 needs the kill emulator and a person), **`rak ladder`** (it climbs
  the supervisor's ladder now, and its power-cycle rung drives the kill line), `self switch`/`self
  confirm`/`self reject` outside `bench flash`, `sleep …` outside `bench walk` steps that say so.
  **`rak cli <line>`** runs a line on the radio's own console with full privileges (passwords, radio
  settings, `erase`), so it is refused unless the inner line is in a **read-only allow-list** in
  `policy.py` — taken from the repeater firmware's `docs/console.md` read-only commands (`ver`,
  `board`, `clock`, `get` of a non-secret setting such as `name`, `radio`, `public.key`,
  `dutycycle`, `path.hash.mode`; `sensor list`; `sensor get <name>`), each named, tested, and
  everything else refused always. `rak watch reset-caps` needs `"radio": "disposable"`.
  A refusal names the key in `bench.local.json` that would allow it, or says *Thomas's*. The table
  is tested against `commands.cpp`'s command list: a command added to the console and missing from
  the table fails the test (no line is allowed by omission).
- **The commands** (`python tools/bench/bench.py …`, standard library + pyserial + esptool, pinned in
  `tools/requirements-verify.txt`):
  - `status` — finds the port, reads the banner and `status`, `get` (secrets as *set*/*unset*,
    never their values — check that the console itself masks them, and treat a value it did not
    mask as a finding, not as something to print), the version, and the lock;
  - `flash <image>` — **without touching NVS, the journal or the image store**: the factory image
    only with `walter: blank`; otherwise the application through a path that leaves the partitions
    of `partitions.csv` other than the application slots untouched — the firmware's own A/B update
    over the cable (`walterctl.py push --as self` and `self switch`, with its one-boot trial), or
    esptool writing the inactive application slot and the OTA data; choose, say why in the report,
    and test it: `get` read before and after must be identical, *set*/*unset* included, or the
    command says so in capitals and exits 1;
  - `console "<line>"` and `session <file>` — a line at a time, the reply to the prompt, the policy
    applied to every line before the port opens;
  - `record` / `restore` — the non-secret settings before a run and back after it (`swd.khz`,
    `wake.interval` and the rest a walk changes); restore never sets a secret;
  - `walk <step>` — the frame tasks 02–04 fill: the step's lines in `docs/bench.md`'s order, each
    printed with what was sent and what came back and *ok*, *differs* or *needs a person*; a result
    file under `out/bench/walk-<step>-<date>-<time>-<commit7>.md` with every blank `→ ______` the
    step measured filled in; `--tick` ticks the *ok* lines in `docs/bench.md` as
    `[x] … (bench, <date>, <commit>)` and writes each *differs* into `tasks/phase-6/FOLLOWUPS.md`
    tagged with the task that owns the line, exactly as the repeater firmware's `walk.py` does.
  - Exit codes: 0, 1 failure, 3 no bench, 4 lock held, 5 refused by the policy.
- **The lock**, shared with the repeater firmware's bench: the same file in the same place
  (`lock.py` there says where — under the user's local application data — and what it holds), keyed
  by the radio's **canonical ID**: upper-case `%08X%08X` of `DEVICEID[1]` then `DEVICEID[0]`, which is
  what that bench's `serial` is (read with `rak swd id` at the start of a command that touches the
  radio; `rak swd id` prints lower case today — the tool upper-cases it, and says so in the report),
  so that a repeater bench run and a Walter bench run never drive one radio at once. A radio whose
  port is closed has no readable ID: then the tool takes a lock for the whole unit (keyed by
  `walter_usb`) and refuses to start if **any** lock of the repeater's bench is held. And a lock of
  its own for the Walter. A held lock is exit 4 and says who holds
  it. Do not copy their code into this repository without saying so; a shared format is the point.
- **The S3's own manners** (caveats 3, 4, 10): after any flash or reset of the Walter, expect the
  radio to have been reset by the GPIO glitch, and wait before the next `rak` line (a named constant,
  *to be measured* in task 02); never two `rak reset`s within a second (the console already refuses;
  the tool does not retry a refusal); after a sleep the USB port vanishes and comes back with the
  same serial number — wait for it by that number, with a timeout, never by "the next port".
- **`tasks/RUNBOOK.md`** already says (§2, *Phase 6*) that a session may use the rig only through
  this tool. Check that it says what the tool does; adjust the wording if the tool differs, in this
  PR.
- **Docs:** `docs/bench.md` gains §0a *The bench a session may use* and §0b *What a session walks,
  and what stays yours* (the repeater firmware's two sections are the model; a table of the steps and
  which lines are a session's), and the legend for `(bench, …)` ticks; `README.md` one paragraph.
- **Tests (host)** against `console_host` (the real console over a pipe) and a fake port: every
  refusal before a port is opened (the fake port counts opens); the policy table complete against
  `commands.cpp`, and **each command's class asserted** — `rak ladder` and `rak kill` always refused,
  `rak cli` refused for a line outside the allow-list (`rak cli password x`, `rak cli set freq 869`,
  `rak cli erase`) and allowed for one inside it; `flash` keeps `get` identical (against a fake that loses NVS, the tool must say so
  and exit 1); the lock shared with a lock file in the repeater firmware's format; a Walter that
  vanishes and comes back after a sleep; no secret value in any output, any result file, any log
  (a canary secret in the fake's NVS, grepped for).

## Acceptance criteria

- [ ] With no `bench.local.json` every command exits 3 and says why; with one, `status` finds the Walter by its serial number only (tests; and, if the rig is attached, once for real, in the report).
- [ ] Every ⚠ line and every secret is refused before a port opens unless (and only if) the config allows it (tests); the policy table covers every console command (test).
- [ ] `flash` leaves `get` identical, secrets' *set* state included (test; and once on the rig if it is attached, in the report).
- [ ] No secret value appears anywhere the tool writes (test with a canary).
- [ ] `tools/verify.ps1` green (the tool's tests are part of it).

## Not verified here

What the rig has to show is walked by tasks 02–04. If the rig is not attached when this task runs,
say so; the tool is still done.

## Out of scope

The walks themselves (02–04). The radio's USB bench (the repeater firmware's `tools/bench/`, read,
never changed).
