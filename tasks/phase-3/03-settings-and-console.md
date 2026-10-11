# 03 — Settings and the console

**Depends on:** 01 (and every earlier task's console commands).
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §1, §11; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-12, S-13; the agent firmware at `51faafb427cba41b0de9983736b6621bc155f829` — `docs/console.md`, `src/console/`, `src/settings/` and their tests (read with `git -C ../meshmaster-agent-firmware show <commit>:<path>`): the model for the grammar, the echo, the redaction and the provisioning session.
**Size:** one session.
**Model:** claude-opus-5

## Goal

A person with a USB cable can make a Walter theirs — the APN, the server, a
token — and can see everything it knows without seeing anything secret. And
the console, which has grown a command at a time since phase 0, becomes one
thing: one grammar, one help, one document, one test that replays the session
the README tells a person to type.

Someone who has used the exit node's console should feel at home in this one.

## Deliverables

- **`components/hal/KeyValue`** and its ESP-IDF implementation on NVS, one
  namespace, in the partition phase 0 placed above the application slots.
  Thin. `SimKeyValue` for the host.
- **`components/core/settings/Settings`** — a table, one row per key: name,
  type, range or pattern, fallback, whether it is secret, one line of help,
  and what must happen when it changes. Typed getters; `set` validates before
  it stores. The keys:

  | Key | Value | Fallback while unset |
  |---|---|---|
  | `lte.apn` | 0..63 characters | empty: the network's default |
  | `lte.pin` | 4..8 digits; **secret** | none: no PIN is sent |
  | `lte.psm` | `off`, `on` | `off` (task 01) |
  | `server.url` | `https://host[:port][/path]`, the scheme required, ≤128 | none: no backend call is made |
  | `server.token` | printable, ≤160; **secret** | none |
  | `fetch.url` | `https://…`, a release base for the bench's `fetch` | none |
  | `wake.interval` | `auto`, or minutes 5..10080 | `auto`: the battery decides (task 04) |
  | `wake.radio` | `on`, `off`: may the radio's line wake the Walter | `on` |
  | `swd.khz` | 25..1000 | 100 (phase 1, task 03) |
  | `log.level` | `error`, `warn`, `info`, `debug` | `info` |
  | `unit.name` | 0..32, a label for a person | empty |

  Keys that earlier tasks introduced as build constants "until settings exist"
  move here: search the reports of phases 1 and 2 for that phrase and list in
  this task's report what moved and what stayed a constant, and why.
  **Thresholds that protect the hardware stay constants** — the battery floors
  for a power cycle and a flash, the supervisor's caps, the proving period: a
  typo at a console must not be able to remove a safety margin.
- **Secrets**: `get` prints `set` or `unset`, never the value; `set` echoes
  the value as asterisks; the log, the journal, `dump` and any status line
  never contain one. A changed `lte.pin` clears the "this PIN was refused"
  memory of task 01. NVS is not encrypted unless the owner turns flash
  encryption on — say so in `docs/console.md`, as the agent firmware does.
- **The console, whole** (`components/core/console/`): a pure parser from a
  line to a command — bounded line length, no control characters, the value of
  `set` being the rest of the line, trimmed — and a command table that every
  earlier task's commands are registered in, with one line of help each.
  Grammar and behaviour as the agent firmware's: a `>` prompt, the board
  echoes, a refusal is one line beginning `error:` that names what was wrong.
  The commands, grouped as `help` prints them:
  - `help`, `status`, `get [key]`, `set <key> <value>`, `erase confirm`,
    `reboot`, `reboots`, `dump`;
  - `pins`, `rak …` (reset, kill, hello, ping, status, time, cli, power, text,
    swd, flash-test, update, role, watch, ladder — whatever phases 0–2 made);
  - `image …`, `trust`, `trust tls`, `self …`;
  - `lte …`, `http get`, `fetch`;
  - `journal` — the last entries in words.

  Where two earlier tasks made commands that overlap or disagree in style,
  unify them and list the changes in the report; `docs/bench.md` is updated in
  the same commit so that no bench step names a command that no longer exists.
- **`status`**, one screen: firmware, descriptor (role, board), why it booted;
  the radio — alive or not, its version and role, its clock state, the link's
  counters; the battery as the radio last reported it; the images (golden,
  candidate, on trial or confirmed); the uplink's last attempt; when the next
  wake is and why. No secret, no IMEI.
- **Dangerous commands ask for a word**, as `erase confirm` does: anything
  that flashes the radio from the console outside the update machine, and
  `rak kill` beyond a few seconds. A command that could brick a radio is not
  made impossible on the bench, but it is never one typo away.
- **`dump`**: a log ring of a few kilobytes in RAM, whole lines only — the
  same bytes the port printed. Lines the radio sent as text (a scanner's
  NDJSON) are not logged; nothing the mesh said is (S-12).
- **`docs/console.md`**, complete: the commands, the keys, what the port
  prints besides replies. **`docs/provisioning.md`**: the session, line by
  line, that takes a Walter from the factory image to a provisioned unit — and
  the same session is a host test, so the document cannot rot.

## Tests (host)

- The parser: every command shape; every refusal; a line too long; a control
  character; a value with spaces; unknown key; out of range; a URL without
  `https://`.
- Each key: stored, read back, fallback, range, and the action on change.
- Secrets: after a session that sets both and then runs `get`, `status`,
  `dump`, `journal` and `lte`, the captured output contains neither value.
- `erase confirm` forgets the settings and **only** the settings: the images,
  the journal, the bad list and the supervisor's caps are still there. Without
  the word it refuses and says what the word is.
- `help` lists every registered command, and every registered command has a
  help line and appears in `docs/console.md` (a test reads the document).
- The provisioning session of `docs/provisioning.md`, replayed.
- A corrupt or empty key-value store at boot: fallbacks, a log line, no crash.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] One parser and one command table; no command is parsed anywhere else (say how that was checked).
- [ ] The report lists what moved from build constants to settings and which protective thresholds deliberately did not.
- [ ] The secret test covers every command that prints anything.
- [ ] `docs/console.md` and `docs/provisioning.md` exist and are held to the code by tests.

## Not verified here (Thomas, with a Walter)

Add to `docs/bench.md`, "12. Provisioning": the session of
`docs/provisioning.md` typed at a real console; a power cycle; `get` shows
the same; `status` fits a terminal window.

## Out of scope

Settings sent by a server. A second console over the radio. Flash encryption.
