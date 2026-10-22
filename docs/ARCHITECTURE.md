# How this firmware is put together

The design the task piles build, written before a line of this repository compiled. Numbers that
came from a datasheet or a specification were marked *(verify)*: the task that first used one read
the document, cited it in the code, and corrected this file if it was wrong. **No *(verify)* mark
is left** — every one was settled by the task that needed it, and task p3-06 held this file against
the code it describes (`tasks/phase-3/reports/06.md` lists the differences it found). What a
document could not settle is in [`bench.md`](bench.md) instead, with the measurement that will.

## 1. Layout

```
CMakeLists.txt, sdkconfig.defaults, partitions.csv, idf_component.yml + lock
main/                   app_main: construct the real things, hand them to the core, run
components/
  core/                 everything that is logic. No ESP-IDF include, no FreeRTOS, no heap after start-up.
    board/              the pin map and RadioPins: the seven pins and their rules (§2) - task p0-03;
                        Slot, TargetSelect and the selection guard - task p1-06
    health/             why the last boot happened: the reset reason, a boot counter, the last five - task p0-03
    link/               the box link: codec, client (the UART, retries, events, the session,
                        telling a repeater from a scanner), the protocol's numbers in words
    swd/                the debug port: wire protocol and DP (task p1-03), MEM-AP (task p1-04)
    nrf52/              identification, access-port protection, the flash controller, core control,
                        the region guard; regs.h names every register, the forbidden ones included
                        (task p1-04). provision_door.{h,cpp} and provision_grant.h (task p5-02,
                        S-5a): the one door - the CTRL-AP's erase and soft reset, the UICR and the
                        base's flash - behind a grant that only a typed console line can make;
                        the only file but regs.h that may name the forbidden registers (rail R3),
                        included by core/provision/ alone (rail R3b)
    provision/          `rak provision` (task p5-03, S-5a): the door's one caller - the sequence
                        that gives a factory radio its base, step for step, refusing before the
                        first write and starting from erased after it - and the variant table it
                        acts on (variant.h: INFO.VARIANT's build code -> legacy or improved
                        APPROTECT, with its sources). The only directory that may include the
                        door's header (rail R3b)
    flasher/            an image and a region in; erases, writes, verify out: the board profiles,
                        the bootloader's settings page, the flasher (task p1-05)
    image/              the descriptor (task p0-02); the manifest, the signature line, the trust
                        table, the choice of image, the file's streaming checks and the verdict
                        with its token (task p2-01); the store's records, its index and the framed
                        transfer that loads an image over USB (task p2-02).
                        crypto/ holds SHA-256 (task p1-05), SHA-512 and Ed25519 verification
                        over the vendored TweetNaCl extract (task p2-01)
    third_party/        vendored, verbatim, each with origin, version, licence and hash in its
                        README.md: the TweetNaCl verification-only extract, jsmn (task p2-01)
    update/             the journal (a fixed-size, append-only ring in its own partition), the
                        radio's update as a state machine with its health check and rollback, the
                        battery as the radio reports it (task p2-03); the Walter's own update -
                        the other slot as a transfer target, the switch, the one-boot trial with
                        its three conditions, and the uplink condition's seam (task p2-04); the
                        battery's last good reading, for the rungs that are reached over a dead
                        link (task p2-05)
    supervisor/         the radio's heartbeat and the ladder of remedies: the four kinds of
                        silence, the five rungs, the daily caps in retained storage and the
                        journal, boot-loop detection, the incidents it keeps (task p2-05)
    uplink/             the way out: the failure taxonomy and its backoff in wakes, the window a
                        time must be inside to be the network's, the 3GPP power-saving timers,
                        and the policy - the whole sequence of getting onto the LTE-M network,
                        every step's deadline and the budget one wake may spend (task p3-01)
    net/                HTTPS (task p3-02): the URL (https only), HTTP/1.1 as bytes - the request
                        writer and the response parser - over a TLS socket, the roots this
                        firmware trusts and the loader that writes them into the modem once, the
                        client with its rules (redirects, statuses, the bound), the download in
                        ranges, resumable, budgeted. update/fetch.h is the three steps of a
                        release over them, update/tls_check.h the trial's handshake
    power/              what the battery allows; the check-in interval (task p3-04):
                        interval_policy.h is a pure function of the readings this wake has, the
                        band the last one ended in and the settings -> a band, an interval with
                        hysteresis, and what the wake may do. docs/power.md is its table
    settings/           the settings a person provisions over USB (task p3-03): keys.h is the
                        table - one row per key with its type, range, fallback, whether it is a
                        secret, one line of help and what happens when it changes - and
                        settings.h the store over interfaces/key_value.h, which reads every key
                        into RAM once so that the pointers the uplink and the trial keep stay put
    console/            the line parser, one command table with a line of help each (commands.h,
                        task p3-03), the 4 kB ring `dump` prints (log_ring.h) and the commands'
                        text (task p0-03: help, status, reboots,
                        pins, rak reset, rak kill, reboot; task p1-02: rak hello, ping, status, id,
                        time, power, env, cli, events, text; task p1-03: rak swd connect; task
                        p1-04: rak swd id, rak swd mem; task p1-05: rak flash-test; task p2-01:
                        trust; task p2-02: image list, verify, erase, bad, receive; task p2-03:
                        rak update, rak update --first-install, rak update status, rak role
                        scanner|repeater; task p2-04: self status, receive, switch, confirm,
                        reject; task p2-05: rak watch, rak watch reset-caps, rak ladder; task
                        p3-01: lte, lte up, lte down, lte time, lte id; task p3-02: http get,
                        fetch, trust tls; task p3-03: get, set, erase confirm, dump, journal,
                        and the word `confirm` on rak flash-test and a long rak kill; task p3-04:
                        wake, power, sleep now, sleep <seconds>, stay; task p3-05: cycle now,
                        cycle dry; task p5-03: rak provision check, rak provision <radio>
                        confirm)
    version/            what this build is, as one string: the banner, `trust`, the descriptor
    cycle/              one wake, start to finish, against interfaces. Task p3-04: wake_reason.h
                        is what a wake *means* - the timer, §8's attention inside its repeat
                        window, a spurious wake and its rate limit, a person with a cable, a
                        power-up, a crash - with the last ten wakes in retained storage;
                        wake_guard.h is the bound on a wake (a wake that does not reach sleep is
                        ended by reset); sleep.h is **the one way out of a wake** - journal,
                        SLEEP_NOTICE, the modem down, the pads held, deep sleep - and
                        duty_cycle.h composes the four. Task p3-05: **wake_cycle.h is the wake
                        itself** - the ten steps in order, each with a precondition that is data,
                        a bound and an outcome that is written down (docs/cycle.md);
                        telemetry_ring.h keeps the readings of every wake until somebody takes
                        them; network_clock.h is the time of day once the network has given one,
                        and the only thing that gives the radio a time of its own accord;
                        null_backend.h is the backend that does nothing, which is this
                        firmware's default
  interfaces/           the interfaces core/ needs: pins, a clock, retained storage (whose slots the
                        board, health and the supervisor hold a record each in), text out, a restart
                        (task p0-03); a byte stream and the time of day (task p1-02); a flash
                        region (task p2-02); the Walter's own two application slots and their
                        rollback states (task p2-04); the uplink - the steps of getting onto the
                        network and the four things worth asking once it is there, each with a
                        deadline (task p3-01); a TLS connection whose chain is verified against
                        one named root with the host name checked, and the modem's certificate
                        slots and profiles; one HTTP request with the body as a stream of
                        pieces (task p3-02); a key-value store of short strings that survive a
                        power cut, which is NVS on the board (task p3-03); and a sleeper -
                        arm the sources a sleep may be woken by, go into deep sleep (which does
                        not return: a wake is a boot), say which source woke this boot, and
                        whether a USB host is on the port (task p3-04); and **the backend** -
                        one exchange of what this unit knows for a closed set of requests it
                        already knows how to carry out, which is the seam the MeshMaster agent
                        goes behind (task p3-05, docs/cycle.md; spec/SYSTEM.md §8).
                        ("hal/" when this file was written; ESP-IDF owns a component of
                        that name - task p0-01)
  agent-core/           **not ours**: twinoak.meshmaster.exitnode-firmware's lib/agent-core/ at
                        one commit, a checked copy with a MANIFEST.txt, never edited here - the
                        MeshMaster agent's job logic on three interfaces and an optional fourth
                        for what a sleep keeps (§13, docs/upstream.md,
                        task p4-01). Its own CMakeLists.txt registers it as the component
                        `agent-core`; ArduinoJson is its one dependency, from the registry
  agent/                **ours**: this firmware's half of the library's seams (§13). Task p4-02:
                        link_mesh.h, the library's mm::IMesh as one request of
                        spec/LINK-PROTOCOL.md §6 per call over core/link/'s client - the reply
                        awaited as EV_MESH_REPLY with REPLY_GET as the fallback, the path cached
                        and read back with PATH_GET after a flood's reply, a password copied into
                        one buffer and wiped, and the *local door*: a job for the unit's own
                        repeater served over §5.7's CLI and §5.8's LPP_GET with nothing on the
                        mesh; mesh_probe.h, `rak mesh status` behind the console's seam
                        (core/console/mesh_probe.h). Task p4-03: modem_uplink.h, the library's
                        mm::IUplink as the seven calls of meshmaster.dk's agent API over
                        core/net/'s HTTPS client - one request per call, `Authorization: Bearer`
                        read from `server.token` for the call and wiped (the transport's copy
                        of the head too), `X-Agent-Version` this firmware's own, the reply
                        bounded and parsed by the library's own parser, the `Call` record as
                        ServerPolicy reads it, no redirect followed, the wake's deadline
                        refusing a call it could not finish; uplink_probe.h, `agent url` and
                        `agent probe` behind the console's seam (core/console/uplink_probe.h).
                        Logic, like core/: no ESP-IDF include (the same build step), no heap
                        (the same check), tested on the host against a simulated RAK and a
                        scripted server. Task 04 adds the platform here
  platform/             ESP-IDF implementations of interfaces/ - thin: the GPIO driver, esp_timer,
                        RTC memory, the USB console, the reset reason (task p0-03), the radio's
                        UART (task p1-02), the `rakfw` and `journal` partitions behind a flash
                        region (tasks p2-02, p2-03), the OTA slots behind esp_ota_* (task p2-04),
                        the settings in the `nvs` partition behind a key-value store (task p3-03),
                        deep sleep with its two wake sources - the timer and ext1 on the radio's
                        line - and the USB Serial/JTAG connection monitor (task p3-04)
  modem/                the Walter's LTE modem behind interfaces/' uplink: the one component that
                        names a WalterModem symbol, on UART2 because UART1 is the radio's (task
                        p3-01); and its TLS over socket 1 and its certificate slots, with the one
                        call in this firmware that configures a TLS profile (task p3-02)
host_test/              a CMake project of its own: core/ + fakes + simulators + tests, built with the host compiler
  fakes/                fake pins that check the pin rules, a clock whose delays take no time, retained
                        storage as a map, a settings store as a map that can be empty, corrupt,
                        read-only or missing (task p3-03); two application slots with the
                        bootloader's rules at a
                        reboot - pending becomes aborted, new becomes pending - so the Walter's own
                        trial, confirmation and rollback run through a simulated reset (task p2-04)
  console_host/         the console on stdin and stdout over a simulated flash, so that
                        tools/walterctl.py is tested before a board exists (task p2-02)
  sim/                  the scripted RAK on a fake UART (task p1-02), and over it the RAK's side
                        of §6 as a simulator - sim_rak.h: one peer, one exchange, the five
                        sends, the reply record and its event, the counters, the 120 s job
                        timeout, with any mm::IMesh behind it as the peer's repeater - and
                        mesh_world.h, the whole road from LinkMesh to that repeater (task
                        p4-02); a scripted HTTPS server
                        behind a fake TLS socket that refuses a chain not ending at the root
                        asked for, drops after N bytes and stalls, and the firmware's own
                        HTTP/1.1 over it (task p3-02); the simulated SWD target at
                        the wire level (task p1-03); the simulated nRF52840 behind it - its two
                        access ports, its flash, FICR, UICR, NVMC and core (task p1-04); a flash
                        region that only clears bits and can lose power at any operation
                        (task p2-02). host_test/core/update/update_bench.h joins them into one
                        world - the store's and the journal's flash, the chip, a radio that boots
                        whatever the chip holds - that outlives a Walter built over it, so a
                        reset in the middle of an update is a destroyed and rebuilt Walter
                        (task p2-03)
  checks/               no_allocation.h: the check that a component's objects take no heap after
                        start-up, applied per component by its own test
  rails/                **the suite's own statement about the rails** (task p3-06): the assertions
                        that are about every test rather than about one - the pin rules' tally, the
                        simulated flash's, the verdict's tokens as types, a day of wakes searched
                        for a secret, a month asked for both updates, the caps over a quarter, and
                        the failure-injection sweep that runs four suites' power-loss loops
                        together, download to commit
  fixtures/             the spec's fixtures turned into tables the tests walk (the descriptor's, task p0-02);
                        command_lines.h, every command of the console's table with something to
                        type after it, which two rails walk
  agent-core/           **not ours either**: the agent library's four fakes (a stock repeater, a
                        scripted meshmaster.dk, a clock that only moves when a test says, a store
                        that outlives a sleep), both contract fixture sets and **the library's own
                        two suites**, copied at the same commit as the component and at upstream's
                        own paths, so that all 93 of its cases run here unmodified (§13, task
                        p4-01). GoogleTest, pinned, for these binaries and nothing else
  shim/                 `support/FakePlatform.h`, which shadows the library's platform fake with
                        one whose clock is tied to the link's fake clock (task p4-02; shared
                        since p4-03): what every binary that runs one of the library's suites
                        over this firmware's code puts before the library's own test support
  agent-link/           the runner's scenarios, again, through the framing (task p4-02): shim/
                        holds a `support/FakeMesh.h` that shadows the library's - the same
                        simulated stock repeater, with sim_rak.h and LinkMesh put between the
                        runner and the repeater - so that test_runner.cpp compiles a second time,
                        unchanged, with every send a frame and every reply an event
                        (`agent_core_runner_link_tests`); and test_link_mesh.cpp, what the
                        scenarios do not say (`agent_link_tests`). GoogleTest, because the
                        repeater behind the RAK is the library's fake
  agent-uplink/         the loop's scenarios, again, through HTTPS (task p4-03): shim/ holds a
                        `support/FakeUplink.h` that shadows the library's scripted meshmaster.dk
                        with one that keeps its every knob and record behind ModemUplink, the
                        HTTPS client and the scripted server - every call a request over the
                        firmware's own HTTP/1.1, every fault a real status - so that
                        test_loop.cpp compiles a second time, unchanged
                        (`agent_core_loop_uplink_tests`): the loop does not know which uplink it has
  agent/                the tests of components/agent/ that need no simulated repeater: `rak mesh
                        status` against the scripted radio and the no-allocation check (p4-02);
                        the uplink against the scripted server answering the contract fixtures'
                        bytes - the seven calls, every status, the token read back from every
                        buffer, the bound, the redirect, the deadline - and `agent url` and
                        `agent probe` on the console (p4-03)
tools/                  verify.ps1 and verify.sh (the definition of "verified"), check_rails.py
                        (**every rail, one entry each**, run by verify.sh), check_partitions.py,
                        the trust-table and TLS-root generators, release/ (the release tooling),
                        walterctl.py (the laptop's end of `image receive`), cmake/ (the no-ESP-IDF-
                        include check); sync-agent-core.ps1 (the agent library's copy and the
                        -Verify of it), upstream-check.ps1 and new-sync-task.ps1 (the watch and
                        the one command for a red bump - docs/upstream.md, task p4-01)
keys/                   the public halves of the keys a release may be signed with - empty today (task p0-02);
                        tls/: the TLS roots a server's chain must end at, each with its header (task p3-02)
spec/                   a copy of the system specification and its fixtures - never edited here
docs/                   this file; bench.md (the one ordered bench walk and the numbers to bring
                        back); field.md (what a unit on a mast does, for the person who owns it);
                        release.md, console.md, provisioning.md, update.md, supervisor.md,
                        power.md, cycle.md, upstream.md (keeping up with the agent core)
tasks/                  the piles; sync/ is the one kind of task a bump of the agent core makes
```

The rule the layout enforces: **if it can go wrong in the field, it is in `core/` and has a host
test that makes it go wrong.** `platform/` is the part no session can test; it is kept so thin that
reading it is the test.

## 2. The board: seven pins and their rules

From the hardware repository's `INTERCONNECT.md` (frozen) and `BENCH-BREADBOARD.md`:

| Net | D-sub | Walter GPIO | Direction | Rule |
|---|---:|---:|---|---|
| `LORA_nRESET` | 3 | 1 | open-drain | **output-low to assert, input to release. Never output-high.** The nRF52 has its own pull-up |
| `LORA_SWDCLK` | 4 | 2 | out | idle low; must not toggle while nRESET is held low (reserved for the dual adapter's selector) |
| `LORA_SWDIO` | 5 | 4 | bidirectional | input unless a transfer is driving it |
| `LORA_PWR_KILL` | 6 | 5 | out | **high = the LoRa 3.3 V rail is off.** Floating or low = on. Default at every boot and in sleep: not driving |
| `spare` | 7 | 6 | — | input, unused |
| `WALTER_TX` → radio RX | 8 | 7 | out | UART idle high while the radio is powered (pulled up, not driven, when the UART is not in use); input (not driving, not pulling) when it is not |
| `WALTER_RX` ← radio TX | 9 | 8 | in | an RTC-capable pin: a start bit can wake the Walter from deep sleep |

The D-sub column is the LTE box's connector (`INTERCONNECT.md`, "D-sub pinouts"); pins 1 and 2 are
ground and the raw battery. `core/board/pin_map.h` carries the same three columns and a host test
reads this table and fails if they disagree (task p0-03).

Each line has 470 Ω in series and about 2.2 nF to ground (two bulkhead filters): a time constant
near 1 µs.

**Rules, each enforced in one place (`core/board/radio_pins`, the only object that touches these
pins) and tested against fake pins that check them on every transition:**

1. **Safe state.** At boot, before anything else, and whenever this firmware has nothing to say to
   the radio: nRESET input, SWDCLK and SWDIO inputs, KILL not driving, TX idle high if the radio
   is believed powered — through the chip's weak pull-up, not driven, so that a radio that is
   unexpectedly dead sees microamps, not milliamps — and a plain input if not. TX is driven only
   between `uartEnable()` and `uartRelease()`.
2. **Reset is a pulse of output-low** (open-drain) of 100 ms, then input. **Two resets are at
   least one second apart**, measured from the end of one pulse to the start of the next — two
   inside half a second put the Adafruit bootloader into UF2 mode, where it stays. A reset that
   comes too soon *waits*; it is never refused and never shortened. The spacing is enforced across
   deep sleep too: the time of the last reset is kept in retained storage, and when the clock
   cannot be trusted against it (no record, a power cut, a clock that restarted) the worst is
   assumed — a reset ended just now — which also covers the one the power-up glitch (rule 5)
   gave the radio. There is no operation that drives nRESET high: the only handle on the pin can
   assert low and release.
3. **Before the kill line goes high, every other radio-facing pin is a plain input**, TX included
   and its pull-up off. The series resistors limit back-feed into a dead radio to a few milliamps;
   the firmware's job is to make it zero. Whoever held the UART or the SWD lines loses them (their
   operations return false from then on). After the rail is back, wait for it to settle before
   driving anything: the rail is *off* while the kill line is driven, *unknown* for the settle
   time after it is released — and for the settle time after boot, because a Walter that reset in
   the middle of a kill left it to float back on just before — and *on* after that.
4. **A kill is bounded**: at least long enough for the rail to collapse, at most a configured
   maximum (`kKillTiming`: 1 s, 30 s, 200 ms to settle — placeholders until the bench measures the
   rail, `docs/bench.md` §6); the kill line is driven high or not at all, never low; the firmware
   cannot leave the radio off by crashing — the line floats to *on* when the Walter resets, by
   hardware. Nothing acquires SWD while the rail is believed off, and the SWD clock cannot toggle
   while nRESET is asserted (a reset is refused while SWD is held; the holder releases first).
5. **Across deep sleep the pads are held** in the safe state, so that waking does not glitch the
   reset line: `safe()`, then the holds on; at wake the same state configured again, then the
   holds off. (A *power-up* of the ESP32-S3 does glitch GPIO1–14 low for about 60 µs, and that
   reaches nRESET: a Walter power cycle reboots the radio. That is expected, documented in the
   hardware repository, and the supervisor does not mistake it for a fault.)
   **The wake pin is the one exception** (task p3-04). When the radio's start bit is to wake us,
   `WALTER_RX` is given the chip's weak pull-up — so that a radio which is off, resetting or
   being flashed cannot hold the Walter awake with a line that has drifted low — and that one pad
   is left out of *our* holds, because ESP-IDF's `ext1_wakeup_prepare()` routes it to the RTC
   mux, enables its input and holds it itself as part of arming the source, and a hold put on
   first would block those writes (`sleep_modes.rst`, *External Wakeup (ext1)*: "please call
   `rtc_gpio_hold_dis` to disable the hold function to perform any pin re-configuration"). At the
   wake the pad is routed back with `rtc_gpio_deinit()` before the UART can have it again, and
   the order in `main/app_main` is: `safe()` (constructing `RadioPins`), `releaseHolds()`,
   the sleeper's `begin()`, `safe()` once more. The pull-up is dropped again by `safe()`, which
   every kill calls first (rule 3), so it cannot survive into a moment when the rail is off.
6. **The one exception to rule 4's "the clock cannot toggle while nRESET is asserted" is the
   dual adapter's selector, and this firmware never takes it** (task p1-06). The dual adapter —
   designed, not built: the hardware repository's `DUAL-RAK4630-FEASIBILITY.md` §3 at commit
   `b798aab` — carries two flip-flops behind a gate that computes `SWDCLK AND NOT nRESET`, so
   holding nRESET low and clocking two bits in on SWDIO says which radio the debug lines, the
   reset and the UART reach. FF2's bit first (the UART's), FF1's second (the debug lines'); the
   window is 100–500 µs, and the adapter's own ~2–5 ms RC and Schmitt buffer keep that reset away
   from the radios. `RadioPins::selectorProgram()` is the only operation in this firmware that may
   emit it, and the reset-spacing rule of rule 2 is applied to the reset it implies all the same —
   nothing here can measure the adapter's filter, and a bootloader left in UF2 mode is how a radio
   is lost.
   **Where it is switched off:** `main/app_main` constructs a `core::board::SingleTarget` — slot 0
   is the one radio, selecting it moves no pin — and nothing constructs `core::board::DualTarget`.
   `tools/check_rails.py` greps that neither `DualTarget` nor `selectorProgram` is named outside the
   two files that define them, and `host_test/core/board/test_slot_single.cpp` fails if a single-radio
   unit's wires ever carry the sequence (the fake pins' `swdclk-while-reset`). The dual adapter's
   own tests declare a selector on the bench, and then the fake pins check the window's length
   instead (`selector-window`, 100–500 µs).

Everything that faces a radio takes a **slot** (`core/board/slot.h`): `Slot`, `TargetSelect`
(`SingleTarget`, `DualTarget`) and `SlotLock`, the guard that keeps one selection at a time —
an operation selects its slot and holds it, so two operations on different radios cannot
interleave. With one slot the behaviour is unchanged, and a slot that does not exist is refused
before anything is steered.

The fake pins (`host_test/fakes/fake_pins`) know these rules by name — `reset-driven-high`,
`reset-spacing`, `reset-pulse-length`, `swdclk-while-reset`, `driven-while-killed`,
`kill-driven-low`, `kill-bounds`, `selector-window` — and fail the running test at the transition
that breaks one. **They are the suite's rules and not one test's**: a violation no test declared
with `allow()` fails that test at the fake's destructor whatever `strict` said, and
`host_test/rails/test_rail_pins.cpp` prints what the whole run did to a set of simulated wires and
holds the undeclared count to zero (task p3-06, rails R1 and R2). `tools/check_rails.py` greps that
no GPIO call exists outside `components/platform/`, that the seven pins are named nowhere but
`core/board/`, that nRESET reaches the driver only through `ResetLine` — which can assert low and
release and has no other mode — and that the dual adapter's selector is named nowhere but the two
files that define it (rule 6).

## 3. The link, from this end

`spec/LINK-PROTOCOL.md`. Two layers: the codec, which is the protocol as bytes, and the client,
which is the protocol as calls.

**The codec** (`core/link/codec/`, task p1-01) touches no UART and allocates nothing:
CRC-16/CCITT-FALSE and COBS, the packet, a `FrameReceiver` that turns bytes in any chunking into
packets and counts what it drops, a `FrameWriter` that puts §2's one `0x00` in front of a frame
after 100 ms of idle, a `Reader` and a `Writer` for the little-endian scalars, one struct per
payload with an encoder for every request this end sends and a decoder for every response and
event it receives, and one header (`codec/wire.h`) that is the specification's own tables of
numbers. A decoded message's variable-length fields are *views* into the receiver's buffer, valid
until the next frame is read, so keeping one means copying it. A decoder refuses a value that
selects the layout of what follows it (`path_mode`, a reply record's `state`) and passes on one
that is only data (a status, a role, a capability bit), because §5.1 says a minor version adds
those and that unknown ones are ignored. It is tested against `spec/fixtures/link/link-v1.json`,
which the radio firmware's own codec is tested against too; neither sees the other's source, and
if both pass every case they agree on the wire.

**The client** (`core/link/client/`, task p1-02) owns the UART (115200 8N1 on GPIO7/8) and gives
the rest of the firmware typed calls: *hello*, *ping*, *status*, *set time*, *run this console
line*, *read power*, and the mesh client's calls — one per request type of §4, each answering with
the decoded response or the reason there is none (link down, refused with which status, malformed,
a capability the radio does not have, the UART not ours). Underneath: one request at a time, a
`seq` different from the last, a response awaited for one second in slices of a tick, the same
frame sent again up to twice more, then "the link is down" — reported, not acted on; acting is the
supervisor's. A response of another type or `seq`, and one that arrives with nothing in flight, are
counted and ignored. A decoded answer's variable-length fields are views into the client's own
response buffer, valid until the next call.

**Its two pins are taken and given back as one thing with the UART.** `core/link/client/port.h` is
the only caller of `RadioPins::uartEnable()` and `uartRelease()` and the only caller of the byte
stream: the peripheral is routed to the pads after the board logic has enabled them and away before
it releases them, and every read and write asks the board whether the UART is still enabled — a
`rak kill` takes the lines without telling anyone (§2 rule 3), and the port then closes the stream
itself and counts the loss. So "TX is driven only between `uartEnable()` and `uartRelease()`" is a
property of the code's shape and not of everybody's care.

It keeps the radio's `boot_id` and treats a change as "everything I knew about that radio's state
is gone"; a changed `boot_id` or an `EV_BOOT` is raised to whoever subscribed. It sets the radio's
clock after every restart it sees, before anything else — from `interfaces::WallClock`, which
answers "I have no time to give" until phase 3's modem fills it. `EV_BOOT`, `EV_MESH_REPLY` and
`EV_ATTENTION` are decoded and queued as they arrive — a small bounded queue, its overflow counted,
their sequence numbers watched for gaps — whenever the client reads the port, which the main task
does every time round.

If `HELLO` gets no answer three times and lines beginning with `{` arrive — or one answers an
`info\n` probe — the radio is running NoiseScope: the client says *scanner (text)*, sends no more
frames, and offers the lines to whoever subscribed until the radio is reset. It never mixes the two.

Two things the mesh client asked of it (task p4-02): a `CLI` request may be flagged *sensitive*,
and is then wiped from the request and frame buffers after the exchange exactly as `SEND_LOGIN`
is; and an event can be taken out of the queue *by type* — the mesh client takes its
`EV_MESH_REPLY` and leaves `EV_BOOT` and `EV_ATTENTION` for the console and the supervisor. And one
thing S-12 asked of it once replies started to carry what a repeater echoes: the receive side is
wiped too — the receiver's buffers once a packet has been copied out, the read chunk once it has
been fed, the last response at the start of the next exchange, a vacated queue slot — so that
nothing the radio sent outlives its use in a buffer nobody reads.

## 4. SWD

Three layers, each tested against the layer below it in simulation:

**The wire** (task p1-03, `core/swd/wire`; Arm's ADIv5.0 to ADIv5.2, IHI 0031G, chapters B4
and B5, cited line by line). Bit-banged, clocked entirely by us, so only the minimum half-period
matters, not jitter. **125 kHz to start** — a half-period of 4 µs, the slowest clock the nRF52840
Product Specification allows (*Debug and trace > Electrical specification*, f_SWDCLK 0.125 to
8 MHz) and four time constants of the filtered lines, which are **predicted to carry about
300 kHz and no more**; the half-period is a parameter, bounded below at 1 µs. Line reset (at least
50 clocks with SWDIO high, then at least two idle cycles low, B4.3.3), the JTAG-to-SWD sequence
`0xE79E` LSB first between two of them (B5.2.2), then a read of `DPIDR`, the only transaction the
reset state accepts. A transfer: eight request bits (start, APnDP, RnW, two address bits, even
parity, stop, park), a turnaround, three ACK bits (OK `001`, WAIT `010`, FAULT `100`, LSB first),
then 32 data bits and a parity bit in the direction of the transfer, with turnarounds where the
bus changes hands; eight idle cycles after every transfer so that the clock may stop (B4.1.1).
The target samples and changes SWDIO on SWCLK's rising edge (B4.3.1); we change it right after the
falling edge and **read the target's bit at the end of the low half, just before the next rising
edge** — SWDIO's turnaround is the tight spot through the filters, and that point gives the
target's driver a whole period less the clock's own delay (about 7 µs at 125 kHz, 2.6 time
constants at the predicted ceiling) instead of half of it. An acknowledge of all ones is *no
target*; any other value that is none of the three is a protocol error, after which the host backs
off for the length of a data phase before anything else (B4.2.5).

**The debug port and the memory access port.** `DPIDR` (the nRF52840's reads `0x2BA01477`: Arm's
DPv1 SW-DP, part `0xBA`, revision 2 — the Product Specification only says "a standard Arm
CoreSight SW-DP", so the bench confirms the number), power-up through `CTRL/STAT` with a bounded
wait for both acknowledges and a `powerDown()` that withdraws the request (on the nRF52840 that is
what ends debug interface mode), `SELECT` rewritten only when the port or the bank changes, an AP
read's posted value collected through `RDBUFF` so a caller sees the register it asked for; WAIT
retried ten times with idle cycles between; a FAULT reported with `CTRL/STAT`'s sticky flags and
cleared through `ABORT`; a parity error a failed transfer, never a value (task p1-03,
`core/swd/dp`). Memory access through AP 0 (task p1-04, `core/swd/mem_ap`; ADIv5 chapter C2):
`IDR` read once and held to class *Memory Access Port* (an IDR of zero is no AP at all), `CSW`
written for 32-bit accesses with single auto-increment as a debug master (`0x23000012`, read back
as `0x23000052` with the read-only `DeviceEn`; the Cortex-M4's `MasterType` bit is what lets a
debugger set `C_DEBUGEN`, DDI 0439B Table 8-6), `TAR`,
`DRW`; reads are posted, so the value comes one transfer later. **The auto-increment is guaranteed
only inside a 1 KB block** (C2.2.2: "the 10 least significant bits"; beyond that IMPLEMENTATION
DEFINED) — the Cortex-M4's own AHB-AP wraps at 4 KB (DDI 0439B Table 8-6) — so `TAR` is rewritten
at every 1 KB boundary, which is right for both. Every result carries the address it was for. The
nRF52840's AHB-AP reads `IDR 0x24770011` (the Cortex-M4's, from its TRM; the bench confirms).

**The nRF52** (task p1-04, `core/nrf52/`; every register in `regs.h` with its chapter of the
nRF52840 Product Specification or the Armv7-M ARM):

- a **session**: connect, power-up, then the control access port (AP 1; `IDR 0x02880000`,
  Nordic's) and its `APPROTECTSTATUS` *before any memory access*; then the memory access port and
  `FICR`; the power-up withdrawn at close;
- **identification** from `FICR` (`0x10000000`): `INFO.PART` (`0x52840`), `INFO.VARIANT` (four
  ASCII characters, first in the top byte — the build code the bench must write down), `CODEPAGESIZE`,
  `CODESIZE`, `INFO.FLASH`, `DEVICEID`. Anything but an nRF52840 with 4096-byte pages is reported
  and refused by everything below;
- **access-port protection**, bit 0 of `APPROTECTSTATUS` (0 = on). A protected chip answers on the
  debug port and refuses memory access. **The firmware detects it, says so, and stops**: memory is
  never attempted and every operation answers *protected*. Opening it needs `ERASEALL`, which
  destroys the bootloader, the SoftDevice, the node's identity and UICR; that is a bench procedure
  for a person with a debug probe (hardware repository, `BENCH-BREADBOARD.md` caveat 6), once per
  module. This firmware has no code for it. **Build code Fxx modules** (rev 3, the bench core is
  `AAF0`) ship protected unless `UICR.APPROTECT` is `0x5A` *and* their firmware writes
  `APPROTECT.DISABLE` at start-up (Nordic's APPROTECT note): on such a module **a reset closes the
  port again until that firmware has run**, so `resetAndHalt()` — which halts the core before its
  start-up code — re-reads the protection afterwards and reports *protected* when it is. The
  flasher (task p1-05) must halt a *running* core, or expect this;
- the **core**: `halt()`, `resetAndHalt()` (C1.4.1: `C_DEBUGEN`, `DEMCR.VC_CORERESET`, then
  `AIRCR.SYSRESETREQ`; `S_RESET_ST` and `S_HALT` say it was caught; the vector catch cleared after),
  `run()` (C1.5.4: `C_HALT` 0, then halting debug off), `systemReset()`, `coreState()` — through
  `DHCSR` (`0xE000EDF0`, key `0xA05F`), `DEMCR` (`0xE000EDFC`), `AIRCR` (`0xE000ED0C`, key `0x05FA`);
- the **flash controller** (`NVMC`, `0x4001E000`): `CONFIG` (`+0x504`: 0 read-only, 1 write, 2
  erase), `ERASEPAGE` (`+0x508`), `READY` (`+0x400`); 256 pages of 4096 bytes. `erasePage` and
  `writeWords` take a **`FlashRegion`** and refuse anything outside it before any NVMC access —
  there is no overload without one; a region must be page-aligned and inside the chip's flash, so
  no region can name UICR or FICR. `CONFIG` is returned to read-only after every operation and on
  every error path. `READY` is awaited one read a millisecond: 300 ms for an erase (typical 85 ms),
  50 ms after a block of writes (typical 41 µs a word; the bus stalls the access port while the
  controller is busy, which the debug port sees as WAIT and retries). A write only clears bits:
  erasing first and verifying after is the flasher's;
- **what does not exist in this layer**: a function that writes `NVMC.ERASEALL`, `NVMC.ERASEUICR`,
  the control port's `ERASEALL` or any address in UICR. They are named constants in `regs.h`, for
  the simulator and the reader; `host_test/core/nrf52/test_nrf52_forbidden.cpp` reads every source
  under `components/` and fails if one of the names appears anywhere but `regs.h` and the one door
  below, and `tools/check_rails.py` R3 greps the same exact list of two files before anything is
  compiled;
- **what a closed chip allows, and through whom** (task p5-02). `open()` on a chip whose
  `APPROTECTSTATUS` reads closed stops at *protected* with the debug port connected and powered up
  and the control access port's `IDR` checked: nothing of AP 0 has been touched and nothing will be
  (`identify()`, the core, the flash controller and `read()` all answer *protected*). The control
  access port — AP 1 — is readable and writable on such a chip, and the only code that speaks to
  it beyond `IDR` and `APPROTECTSTATUS` is the door: it writes `ERASEALL`, polls `ERASEALLSTATUS`,
  writes `RESET`, and then calls `open()` again — connect, power-up, `IDR`, `APPROTECTSTATUS`, and
  the memory port and `identify()` if the port now reads open — so that the layer's view of the
  chip is read from the chip, never assumed. `open()` is re-entrant for that: a second call on a
  held slot keeps the hold;
- **the one door** (`core/nrf52/provision_door`, task p5-02; `spec/SYSTEM.md` S-5a). What S-5
  forbids to everything automatic, S-5a allows to a person's typed `rak provision <radio> confirm`,
  and this is the only path: a `ProvisionDoor` over an open session, made with a `ProvisionGrant`
  that has no public constructor and is made by exactly one function from the typed text, in
  S-5a's grammar — the radio's `DEVICEID` (16 hex digits, case-free), `locked`, or
  `locked:<key16>`. **The door checks the grant against the chip itself** when it opens: it reads
  `APPROTECTSTATUS`; an open port needs a `DEVICEID` grant equal to FICR's, a closed port a
  `locked` grant; any other pairing, a malformed or an already-used grant refuses before a register
  is written, and the grant is consumed either way — one grant, one opening — so that a later
  caller (S-5a's MeshMaster job) cannot skip what the console checks. On it only: `eraseAll()`
  (CTRL-AP `ERASEALL = 1`, `ERASEALLSTATUS` polled every 10 ms until *Ready* or 5 s — tERASEALL is
  169 ms typical — then the session opened again), `softReset()` (CTRL-AP `RESET` 1 then 0, 10 ms
  each, then the debug port reconnected and the session opened again: S-5a's one allowed reset),
  `writeUicr()` (inside `0x10001000`–`0x10001307` only, never `UICR.APPROTECT`; NVMC `CONFIG = Wen`,
  each word, `READY` after each, `CONFIG = Ren` whatever happened), `writeApprotect(kind)`
  (`0x0000005A` for the improved kind, nothing for the legacy kind — the kind is the variant
  table's, the door never takes a value), `writeFlash()` (the base's runs through this layer's
  guarded `writeWords`, refusing any address inside the board profile's application region or the
  file system above it — the table's numbers, never a caller's) and `blankCheck()` (flash word 0,
  `NRFFW[0]`, `UICR.APPROTECT`). **The three writes refuse unless `eraseAll()` succeeded in the
  same opening.** Nothing else: no NVMC `ERASEALL`, no `ERASEUICR`, no partial erase — the
  control port's erase is the one S-5a needs and the one that works on a locked part. Who may
  open it: `core/provision/` (the sequence, task p5-03) is the only directory that may include
  `provision_door.h`, and the grant's header may be included by it and by `console.cpp`, where a
  parsed verb runs; rail R3b holds both lists by name and names the directories that must include
  neither — supervisor, update, flasher, cycle, a future agent;
- **the sequence** (`core/provision/provision.h`, task p5-03): S-5a step for step on the layers
  above, and the door's one caller. The console makes a grant from the typed word and calls
  `run()`; a later MeshMaster job (S-5a, *later*) would call the same with a grant of its own and
  get the same checks. Seven steps, each a name in the result: (1) *check* — the base in the
  store for the slot, **judged again** by §4a against the board's region (the store is flash) and
  its bytes held to its record's hash; the radio not held by an update with a run in hand or by
  the supervisor's ladder; the kill line not driven; HELLO on the link (and IDENTITY when it
  answers); the battery by the radio update's rule, an unknown reading accepted as
  `--first-install` accepts it, because a factory RUI3 radio answers no `POWER_GET`; the slot
  taken and held for the whole command; (2) *attach* — `DPIDR`, the control port's `IDR`,
  `APPROTECTSTATUS`; open: `FICR`, and the typed word must be this radio's `DEVICEID`; closed:
  `locked` when nothing answered HELLO, `locked:<key16>` with the first 16 hex digits of
  IDENTITY's key when something did — a radio with an identity is named before it loses it;
  then the door is opened with the grant and checks the port half again against the chip;
  (3) *variant* — the table (`variant.h`): A–D legacy, F improved, anything else refused — before
  the erase on an open radio, after it on a closed one (the identity is readable only then), in
  which case the command stops with the radio erased and says so; (4) *erase* — the door's
  `eraseAll`, the port read back, closed → the one soft reset, attach again, still closed → stop;
  blank check; the core halted; (5) *write* — the base's words (`core/image/base.h` `BaseWords`,
  one 4 KB window a pass over the stored file) through the door in runs of up to 256 words, flash
  first, then UICR, then `writeApprotect` with the table's kind; (6) *verify* — every word read
  back and compared, `UICR.APPROTECT` through the door's blank check against the table's value, a
  difference naming the first address; (7) *reset* — the halt released, the session closed, one
  pin reset through `RadioPins` (open drain, the spacing waited out), `kBootloaderSettleMs`, attach
  again, the port open or closed after it, which is reported and is §10.2's question, not a
  failure. **No journal**: a failure after step 4 leaves the radio erased, which is what the
  command starts from, so it is run again; before step 4 nothing was touched. Before the erase the
  sequence tells the supervisor the slot *awaits its first install* (§8), so that a Walter that
  restarts in the middle, and one that finishes, leaves the silent radio alone.

**The simulator** (`host_test/sim/`) is the other half of this section: a wire-level SWD target
(`swd_target`, task p1-03) that stands between `RadioPins` and the fake pins as a `Pins` of its
own, sees every rising edge of SWCLK, samples and drives SWDIO on it as a target does, and decodes
our bit stream — so a wrong parity bit, a stop or park bit wrong, a line reset one clock short, a
missing turnaround, SWDIO driven while the target drives it, a request before any line reset or
one other than `DPIDR` in the reset state, or the clock stopped under eight idle cycles after a
transfer, fails the test with a sentence naming the section. Its debug port has `DPIDR`, `ABORT`,
`CTRL/STAT` with the power-up handshake and the sticky flags, `SELECT`, `RESEND`, `RDBUFF` and the
posted AP read; access ports plug in behind a register-level interface that can answer with an
error (which sets `STICKYERR`, as the hardware does) or ask for a WAIT (the bus behind the port
is busy). It can be told to answer WAIT N times, to carry a sticky error, to return a bad parity
once, to go silent, to not be there (no pull-up: the line keeps what was last driven), to
acknowledge a power-up late or never, and to start in JTAG so that only the switch sequence
reaches it.

**The simulated nRF52840** (`host_test/sim/nrf52_chip`, task p1-04) plugs into it as AP 0 and
AP 1: an AHB-AP with `CSW`/`TAR`/`DRW`/`IDR` whose auto-increment wraps inside 1 KB — the strict
reading of ADIv5; a knob makes it the Cortex-M4's 4 KB — so that a host that forgets to rewrite
`TAR` lands on the wrong word and a test sees it; a control access port with `RESET`, `ERASEALL`,
`ERASEALLSTATUS`, `APPROTECTSTATUS`, `IDR`; and the memory map: 1 MB of flash in 4 KB pages that
**only clears bits on write and needs `CONFIG` right** (a write with it wrong is nothing, and a
failed test), 256 KB of RAM, `FICR` with the identification registers, `UICR` (read-only to the
firmware), an `NVMC` whose `READY` takes simulated time (41 µs a word, 85 ms a page) and stalls
the bus meanwhile, and `DHCSR`/`DEMCR`/`AIRCR` with a core that is running or halted, a vector
catch at reset, and a Local reset's exact effect on each register. It records every flash erase
and write, every `CONFIG` write and `READY` read. It can be told to be protected, to re-engage
protection at a reset (the Fxx module), to be another part or page size, to report `READY` late
or never, to lose power at the Nth flash operation (leaving a page half-erased or a word
half-written), and **which region alone may be touched** — and since task p3-06 a chip that was
erased or written by a test which never declared a region fails that test at its destructor, so
"the write log is asserted at the end of every test that flashes" is a property of the simulator
and not of each test remembering to (rail R4; `host_test/rails/test_rail_flash_region.cpp` prints
what the whole suite did to simulated flash). **Its tripwire**: a write to `NVMC.ERASEALL`,
`NVMC.ERASEUICR`, the control port's `ERASEALL` or any `UICR` word fails the running test at once.
**Since task p5-02 it also knows what the door does, and the tripwire stays armed by default.**
From the Product Specification's *Access port protection* and *CTRL-AP*: the two kinds of
protection by `INFO.VARIANT`'s build code — hardware-only on Dxx and earlier (*legacy*: open while
`UICR.APPROTECT` reads `0xFF`, read at every reset) and hardware-and-software on Fxx and later
(*improved*: enabled by default, open after `ERASEALL` until a pin, power or brownout reset, and
after such a reset only when `UICR.APPROTECT` is `0x5A` *and* the flash holds firmware built to
write `APPROTECT.DISABLE` — a knob, because whether the RAK's bootloader was is the first board's
to say); three factory states (open with RUI3-like contents; locked by firmware, legacy; locked by
default, improved); the control port's `ERASEALL` (flash, UICR and RAM to `0xFF`, `ERASEALLSTATUS`
busy for a set number of reads or for ever) and `RESET` (a soft reset: legacy re-reads UICR,
improved keeps the port; a knob makes the part drop its debug port with it); a pin reset — the
reset line through `RadioPins` now reaches the chip — and a power-on, each re-reading the
protection as the kind says and resetting the debug port; **resets counted by kind** (CTRL-AP
soft, `SYSRESETREQ`, pin, power-on) so that S-5a's rule — no pin, power-on or brownout reset
between the erase and the last UICR write — is a test's to hold; and UICR written like flash,
with `CONFIG = Wen`, each word at most twice between erases. Only a test that calls
`allowProvisioning()` — a test about the door — gets the erase and the UICR writes performed; every
other test of the tree still fails at the first of either, with the four sentences unchanged.
Everything in this section and the next is developed against it.

## 5. The flasher

Input: an image (its bytes behind an `ImageSource`, its load address, its SHA-256) and the board
profile. The region is **this firmware's own table, per board** (`core/flasher/board_profile.h`,
task p1-05): for `rak4630-single`, `0x26000`–`0xED000` — from the end of the S140 6.1.1
SoftDevice to the start of the file system, the `FLASH` line of the linker script MeshCore's
RAK4631 build uses (`nrf52840_s140_v6.ld:8`) and the span the repeater firmware's release tooling
records as `app_region`. The manifest's `app_region` must lie inside it (`BoardProfile::accepts`),
never replace it. The settings page (`0xFF000`) and the bootloader (`0xF4000`) are in the profile
too, from the bootloader's `dfu_types.h`. Built against the simulated chip, as below.

1. Acquire the lines; connect; identify the chip — refuse anything but the profile's part and page
   size; **refuse a protected chip**, saying so. Read the SoftDevice's own information structure
   (`0x3000`: magic, size): the application must begin where the SoftDevice ends, or an image
   linked for `0x26000` runs nowhere — refused before anything else.
2. Check the image: not empty, page-aligned, wholly inside the region (padded with `0xFF` to a
   word). Then **scan** every page it covers and classify it: equal, reachable by clearing bits,
   or needing an erase. Nothing has been written; an image already on the chip ends with zero
   erases and zero writes.
3. **Halt the core — a running core, not reset-and-halt** (§4: on a build code Fxx module a reset
   closes the access port until its firmware has run; MeshCore runs no nRF52 watchdog that a halted
   core would trip), then wait for the flash controller to finish anything the application had in
   flight. For an image that *is* the application (load address = the region's start): **write
   the bootloader's settings page to "bank 0 holds nothing"** (`bank_0 = BANK_INVALID_APP`), the
   record the bootloader's own DFU writes before it erases bank 0.
4. Erase the image's first page — the vector table — before any other page changes, and write it
   last.
5. Every other page in order: skipped if equal; written without an erase when only blank words
   differ; erased and written otherwise — only the words that are not `0xFFFFFFFF`, in runs. Each
   page read back and compared; a mismatch retried once (erase, write, verify) and then a failure
   naming the address. Then the first page the same way.
6. The whole image read back in order, compared and hashed; the SHA-256 must be the one given.
   Then **the settings page**: `bank_0 = BANK_VALID_APP, bank_0_crc = 0` ("CRC checking is not
   used"), `bank_0_size` the image's — the record every UF2 and DFU install leaves, which both
   bootloaders a RAK4630 may carry start (`core/flasher/bootloader_settings.h` cites the lines) — unless
   a record they already accept for exactly this image is there. Word 0 of a record is written
   after words 1–6, so a torn write reads as an erased page, never as a code with a torn body.
7. A system reset through the debug port, the power-up withdrawn, the pins released to the safe
   state, and `RadioPins` told the radio was reset so that the next pulse waits out the spacing.

**Why the settings page is invalidated before step 4 and validated after step 6.** The stock
bootloader since 0.11.0 starts an application with an *erased* settings page when its first two
words look like a vector table; every bootloader starts one whose `bank_0` is valid with the CRC
check off — the state every RAK in the field is in — as soon as its first two words are not both
blank. Left in place, either record would accept the first page half-written. `BANK_INVALID_APP`
is refused by every version whatever the bytes say, so from before the first erase until after the
last verified write the radio has no startable application and sits in its bootloader; the final
record is written only after the read-back. Erasing the page instead would have been wrong: OTAFIX
and every stock release before 0.11.0 refuse an erased page for ever.

It **never** erases a page the image does not cover — the region handed to the nRF52 layer is the
image's own pages, and the settings page is reached only through one function whose region is that
page alone — never touches the file system (the node's identity and settings), the SoftDevice, the
bootloader's code, the MBR parameter page or UICR. Nor can it reach the door of §4: `flasher/` is
one of the directories rail R3b names as including neither the door nor its grant, and the base —
the MBR, the SoftDevice, the bootloader and the UICR words, everything the flasher's region is not
— is written by the sequence of task p5-03 through that door and by nothing here (S-5a). A power loss at any flash operation leaves a
radio with no startable application, an intact bootloader and identity, and a settings page that
is blank, invalid or valid; running the same flash again finishes it with no more erases than the
pages that still differ need. On a failure after the halt the flasher asks for a system reset on
its way out, so the radio runs its bootloader rather than sitting halted. `verify()` reads back and
compares, changing nothing; `flashTest()` is the bench's `rak flash-test` (`docs/console.md`). The
simulated chip evaluates both bootloaders' `bootloader_app_is_valid()` after every interruption, and
refuses any erase or write outside the image's own pages and the settings page — the region the
bench declares from the board profile.

## 6. Images

**The store** (task p2-02, `core/image/store.h`): the `rakfw` partition of §11, holding per radio
slot a **golden** image (the last one that passed its health check), a **candidate** (verified, not
yet proven), one **scanner** image, and — since task p5-01 — the board's **base**, which is not an
image at all (below). Each is a header — what it is, its size and SHA-256, a copy of its descriptor,
where it came from — and the bytes. A record becomes valid by a final small write after everything
else is in place, so a power loss leaves the previous valid record or none, never half of one.

```
rakfw (0x630000, 6 MB), sectors of 4 KB
  sector 0, 1     the index, two copies: which physical area each of the three roles is, the bad
                  list, a sequence number; the newer valid copy wins
  six areas       slot 0's three and slot 1's three, 200 sectors each
    + 0           the header sector: the record (512 B) and a transfer's progress records (64 B)
    + 4 KB        the image's bytes, 199 sectors - 815 104 B, the largest application region of
                  any board profile this build knows, rounded up to a sector
  two base areas  slot 0's and slot 1's, 161 sectors each
    + 0           the same header sector
    + 4 KB        the base's bytes, 160 sectors - 655 360 B of Intel HEX text, which is ~2.75
                  times the flash bytes it describes: a RAK4630's base is 192 KB of flash, so
                  ~528 KB of text
  6 242 304 B of 6 291 456 used; 49 152 left
```

The base areas come after the six, so that the six are where they were before a base existed.

Laid out for **two** radio slots from the start (§2 rule 6, task p1-06), so a dual unit needs no
new partition table. The arithmetic is a `static_assert` against the partition's size and a host
test that reads the `rakfw` line of `partitions.csv`.

- **A record is made valid by one small write** — its magic word, after the bytes have been read
  back out of flash and hashed — and **invalidated by one small write** before anything of the
  area is overwritten. On NOR flash a write only clears bits, so zeros over the magic destroy it
  with no erase; a torn magic is not the magic either way. `store.h` has the table of what a power
  loss on each side of each write leaves, and the host tests run a store and a promotion once per
  flash operation with the power cut at that one.
- **A promotion moves a pointer, not megabytes**: the index's role-to-area map is rewritten (two
  copies, sequence numbers, the newer valid one wins) and the area that held the old golden image
  becomes the candidate area, its record destroyed afterwards. Seven flash operations, whatever the
  image's size, and **there is never a moment with no golden image once there has been one**.
- **A write resumes.** A progress record goes down every 64 KB, so a transfer an interruption cut
  short continues from the last of them rather than starting over; a resume whose hash is another
  file's starts at zero.
- **The bad list**: up to eight hashes of candidates that failed a health check (task p2-03), in
  the index record; an image on it is refused at the door, before a sector is erased. It is about
  images: a base is not health-checked, so it is not held to the list.
- **The base is in the store and is not an image** (task p5-01, `spec/FIRMWARE-MANIFEST.md` §4a,
  `spec/SYSTEM.md` S-5a): the MBR, SoftDevice S140 6.1.1 and bootloader a factory radio has to be
  given before any image of ours can run on it, as one Intel HEX file per board. It arrives the way
  an image does — the same transfer, the same signed manifest — and differs in every other way,
  all in one direction: **its door takes an `AcceptedBase`** and nothing else, it is never promoted
  (its area is not in the index at all, so there is no pointer to move), never health-checked,
  never flashed by an update, and the one thing that writes it to a radio is a person's
  `rak provision` (task p5-03). Its record carries what §4a found in the file — the bootloader's
  address, the flash byte count, the UICR registers, which SoftDevice — where an image's carries
  its 96-byte descriptor, because a base has none.
- **Wear**: every store erases the sectors its image covers and the area's header sector, once
  each; a promotion alternates the two areas a candidate and a golden image take turns in, so each
  sees half. A thousand images of 20 KB cost 500 erases per sector (measured), which at a NOR
  flash's 100 000 cycles is 200 000 updates — weekly, four thousand years.

**What may enter the store**: only what passed all six checks of `spec/FIRMWARE-MANIFEST.md` §5 —
the signature by a key compiled into this build, the manifest, the right image for this radio, the
file's hash, the one descriptor inside it, and the region. The trusted keys are a small table
(room for three) generated from `keys/release-keys.txt`; a build with none accepts nothing, which
is the state today because the release key does not exist yet (`keys/README.md`).

**The verdict** (task p2-01, `core/image/verdict.h`): `ImageVerdict` runs the six checks in §5's
order over a manifest's bytes exactly as delivered, its signature line and one file behind an
`ImageSource`, and answers either with the number of the check that failed and a sentence, or with
an `AcceptedImage`. It runs in two phases, because the store needs the first of them before the
file is anywhere: `judgeManifest()` is checks 1, 2, 3 and the half of 6 that needs no file, and
yields an **`ExpectedFile`** — what an accepted manifest promises about the one file wanted;
`judgeFile()` is checks 4, 5 and the rest of 6 over the bytes, and yields the **`AcceptedImage`**.
Neither type has a public constructor, and they are the *only* arguments the store's two write
doors take: `beginWrite(which, slot, ExpectedFile)` and `commit(AcceptedImage)`. So nothing can
make the store erase a sector without a signed manifest naming that file for that slot, and bytes
in an area are not an image until the six checks have passed **over the bytes as they lie in the
Walter's own flash** — which is what is read back and judged, never what a sender said it sent.

Underneath, in `core/image/`: the signature
line parsed strictly (`ed25519 <16 hex> <base64 of 64 bytes>`, one line, anything else is "no
signature"; the key chosen by id, an unknown id refused without trying the others); Ed25519 as
RFC 8032 over a streamed SHA-512, with `S < L` checked before the arithmetic (TweetNaCl does not);
the manifest parsed by a vendored no-heap tokenizer (jsmn) into fixed-size structures with named
bounds — eight images, six files an image, 1024 tokens, the descriptor's string sizes — and
validated rule for rule as `manifestref.validate` does; the image chosen by the slot's role,
board and band and the unit's version and link majors (S-7: a list); the file's size, SHA-256
and the one descriptor in it checked in one pass over the bytes as they come; and for an `swd`
file, the manifest's `app_region` held inside the flasher's board profile and the file inside
both. The whole of it is logic with no allocation, held to RFC 8032's vectors, the spec's
fixtures and manifests signed by the spec's own reference code in `host_test/core/image/`.

**Loading one over USB** (task p2-02, `core/image/receive.h`), until phase 3 gives the Walter an
uplink: `image receive <golden|candidate|scanner|base> [@slot]` hands the console's port to a framed
binary transfer and takes three files — `manifest.json`, then its `.sig`, then the image. The
framing is **the box link's own** (`spec/LINK-PROTOCOL.md` §2: COBS, CRC-16, a type, a sequence
number, one `0x00` ending a frame) with this transfer's own six type numbers and §3's convention
that a response is the request with bit 7 set; no new framing code exists. A chunk that arrives
twice is acknowledged again and written once, one out of order is answered with the number wanted,
one that is lost is the sender's to repeat, and a transfer whose other end goes quiet for 20
seconds ends by itself. **The laptop is not trusted**: it delivers bytes, the six checks run here.
`tools/walterctl.py push --port COM7 --as candidate <release-dir>` is the other end, and it is
tested through a pipe against the console itself (`host_test/console_host/`) so that it works
before a board exists.

**The console**: `image list` (per slot: role, board, version, size, hash, where it came from,
which area; and the base, with its bootloader's address and its SoftDevice), `image verify <which>`
(read it back out of flash and hold it to its record's hash - and, for the base, check 5p again
over the same bytes), `image erase <candidate|scanner|base>` (never golden), `image bad`,
`image receive <which>`. `docs/console.md` has them.

**The trust table** is `trust_keys.h`, generated at build time by `tools/gen_trust_table.py`
from `keys/release-keys.txt` through the release gate's own loader, so the firmware and the
release tooling cannot disagree about which keys exist; `trust` on the console prints the ids. A
build made with `-DTWINOAK_TRUST_TEST_KEY=1` (`tools/verify.ps1 -TestKey`) also trusts the
specification's public test key: its version is stamped `+TESTKEY`, it warns at every boot,
`build_release.py` refuses to release it by the stamp and by the key's bytes in the image, and a
normal build is held on every verified commit to carry neither - nor any signing code
(`tools/verify.sh`, the linker map).

**The descriptor** (§4) exists from task p0-02: `components/core/image/descriptor.h` builds,
parses and finds those 96 bytes, with no ESP-IDF include and no heap, tested against the spec's
fixtures; `core/image/this_image.h` is this firmware's own, built at compile time and referenced
by `main/` so the linker keeps it. The release tooling finds exactly one of it in each published
file and holds it against the manifest entry (`docs/release.md`). The store keeps a copy of a
stored image's 96 bytes in its record, read back with the same parser, so that `image list` and
the update's health check (task p2-03) say what the image itself says.

## 7. Updates

**The radio's** (`spec/SYSTEM.md` §6; task p2-03, `core/update/radio_update.h`, `docs/update.md`):
a state machine over the store, the flasher, the link client, the journal, a clock and the battery,
driven one `step()` at a time so that phase 3's wake cycle can run it in slices. *check* - the
preconditions, each a named refusal (a candidate that verifies and is not on the bad list, for this
slot's role, board and band; a golden image to return to, unless a person said *first install*;
the link up or classified down; the battery above `kBatteryMinMv` and not falling; no other run
journalled as unfinished and the Walter's own update not on trial, S-7) → *flashing-candidate*
(the flasher) → *booting* (`EV_BOOT` or `HELLO` inside `kBootWindowMs`) → *proving*, the health
check held against the descriptor the store kept of the image flashed: `HELLO`'s version, role,
board and link major; `TIME_SET` taken (or, with no time to give yet, `TIME_GET` answering and the
step said to be skipped); `STATUS`'s *radio initialised*; a `PING` every few seconds through
`kProvingPeriodMs` with the `boot_id` unchanged; `POWER_GET` where the role has it → *committing*
(the store's promotion) → *done*. Any failure: the candidate's hash on the store's bad list
**first**, then the golden image flashed back, booted and proved by the same check → *restored*;
if it does not come back either, *stuck*, and nothing is retried. A switch to the scanner image and
back (`rak role`) is the same machine with another source, a smaller proof and no promotion.

**The journal** (`core/update/journal.h`): the `journal` partition of §11 as a ring of 64-byte
entries - a sequence number, an owner, 48 bytes of payload, a CRC-32, and a magic word **written
last as its own write**, so a torn entry never validates and the one before it governs. Every
state is journalled before its action starts; `resume()` at boot reads the newest entry and takes
the run up there: mid-flash the flash again (the flasher's scan makes that cheap), booting or
proving **the proof over from the start**, committing the promotion (or the evidence that it
happened), rolling back the rollback. It never assumes. The same ring takes the Walter's own
update's entries (task p2-04) and the supervisor's (task p2-05) under their own kinds; a run's first
entry is protected from the wrap. Tested with the Walter reset after every entry of the good path
and of a rollback, and with the power cut during every write of every entry.

**Its own** (task p2-04, `core/update/self_update.h`, `docs/update.md` *The Walter's half*):
ESP-IDF's two application slots with rollback, behind `interfaces/own_update.h`. A release of this
firmware's `ota` file goes into the other slot through the same six checks as a radio image -
the transfer of §6 with another `TransferTarget` - and is refused at the door when its hash is on
the bad list, when it does not fit the slot, when its `load_address` is not a slot's offset, or
when it speaks another link major than the radio without saying it tolerates the radio's.
`self switch` journals *switching to <version>, from <version>* with whether the radio answered
`HELLO`, sets the boot pointer (the slot's state *new*), sends `SLEEP_NOTICE` and restarts. The
bootloader turns *new* into *pending-verify* and aborts a slot still pending at the next reset, so
**a trial is one boot** whatever ends it. The new image's trial: inside `kTrialWindowMs` it must
(1) reach its main loop with the pins safe, (2) exchange `HELLO` with the radio - unless the radio
was journalled as known down before the switch - and (3) complete one exchange over the uplink
(`core/update/uplink_check.h`; until phase 3 the firmware's implementation says *no server
configured* and the condition does not apply, which is what keeps a factory image from rejecting
itself). All three: `esp_ota_mark_app_valid_cancel_rollback`, *confirmed*. Otherwise: the hash on
the bad list, *rolled back: <the first missing condition>*, `esp_ota_mark_app_invalid_rollback_and_reboot`.
The image that boots after any restart reads the journal: the old image finding *switching* or
*on trial* writes the rollback entry itself (*never reached its main loop*, *restarted during its
trial*, or *the restart came before the boot pointer: nothing was tried*). The radio's update
reads *switching* and *on trial* as "unconfirmed" and refuses to start (S-7).

**Never both in one wake** (S-7). When the link's major version changes, the Walter goes first and
speaks both majors until the radio has followed; the order is enforced here, not left to whoever
starts the update: a Walter image whose manifest `link.major` is not the radio's is accepted only
if its `requires.link_major` names the radio's (it speaks the new and tolerates the old), and a
radio image's `link.major` must be one the running Walter speaks.

## 8. The supervisor

(task p2-05, `core/supervisor/supervisor.h`, `docs/supervisor.md`.) The radio is healthy when it
answers `PING` (§5.2: it proves the firmware's main loop is turning, which a reset line cannot).
When it does not, **what the silence means is worked out first** (`spec/LINK-PROTOCOL.md` §9): the
scanner is running (text lines: reported, left alone), this Walter has only just powered up (its
own glitch reset the radio — §2 rule 5 — and it is still booting: wait), an update is in progress
(not ours to disturb, S-7), or the radio really is not answering. Only the last is an incident.

Then a ladder, each rung tried only after the one before failed, each with a wait before the radio
is judged again, and each with a cap:

1. ask again (the link's own three attempts, then three more after a pause);
2. a reset pulse (`RadioPins::resetPulse()`; §2 rule 2's spacing is waited out, never violated);
3. a power cycle through the kill line (`RadioPins::killCycle()`; §2 rules 3 and 4);
4. what is on the chip: the flasher's **read-only** `verify()` of the golden image against the
   flash. It matches and the bootloader would start it — the firmware is intact, so flashing would
   change nothing: rung 5. It differs — the golden image goes back through the update machine's
   rollback path (`Kind::kRoleRepeater`, with `link_known_down` set), journalled as one run.
   Protected, or unreadable — rung 5 with that reason, and nothing erases a chip to get past it;
5. stop, record, report at the next check-in. A radio that does not answer after the golden image
   is back is a hardware fault and is left alone — until a person, the server, or a long quiet
   period (`kQuietPeriodMs`, measured in days), after which the ladder may be tried once more.

Caps are per day and survive a restart of the Walter: the counts and the window they are counted
in are in retained storage **and** in the journal (`kind::kSupervisor`, every entry carrying the
counts after it), so a power cut that clears RTC memory does not forgive them. **A cap is forgiven
only when a whole day can be shown to have passed**; a clock that cannot be held against the
record — a deep sleep that restarted it at zero — re-anchors the window and keeps the counts. A cap
reached is a refusal that is itself journalled and reported.

**A radio that awaits its first install** (task p5-03, S-5a) is a fifth kind of silence and not a
fault: `rak provision` erased it and gave it its base, so it has a bootloader and no application
and answers nothing on the link — exactly what rungs 2 to 4 would answer with a reset, a power
cycle or a golden image. The sequence marks the slot in the supervisor's retained record and in
the journal (every entry carries the flag, as the counts are carried, so a power cut does not lose
it) before the erase; while it is set a silence is classified *awaiting first install* and left
alone, `Health::kAwaitingFirstInstall` is what the wake cycle sees (not *silent*, which it reads as
"the link is known down" for an update), and `rak watch` and `status` say why. **Only a committed
`rak update --first-install` newer than the mark clears it** — the supervisor reads the journal for
that at every `begin()` and `step()`, and the console asks it to the moment the command ends — and
a new `rak provision` sets it again. Marking also lifts a stop in force: the radio under it is a
new radio.

A radio that restarts by itself over and over (`kBootLoopBoots` `EV_BOOT`s inside
`kBootLoopWindowMs`, or an uptime that never gets past `kBootLoopUptimeS`) is a boot loop and takes
the ladder from rung 3 — a reset will not cure what a reset is already doing. A radio whose restart
followed the Walter's own power-up is not a fault. Before any rung that cuts power or flashes: the
battery must allow it, judged on `POWER_GET`'s **last good** reading
(`core/update/last_good_battery.h`) — the gauge is on the radio's I2C, so a dead radio is a dead
gauge, and the one flash that exists to unbrick a radio may not be refused for want of it. An
`EV_ATTENTION` (§8) is recorded with its reason, acknowledged by the next request, and handed to
whoever runs the wake.

What it reports is a compact record per incident — when, what was seen, each rung and its outcome,
how it ended — three of them in retained storage and all of them in the journal. **When the
supervisor runs is phase 3's** (§10): in these piles `rak ladder` is the only thing that climbs and
`rak watch` reads.

## 9. The uplink

The modem (`walter-modem`) behind an interface: power up, check the SIM, attach on LTE-M, read the
network's time, signal quality; power-saving modes; a taxonomy of failures (no SIM, not
registered, no data, no route) with a backoff for each. Every modem call has a timeout.

As built (task p3-01): `interfaces/uplink.h` is the *steps* - is the modem there, the SIM, the RAT,
the context, the radio on, attach, registration, packet data, and the four things worth asking once
it is up (the time, the signal, the identity, what power saving was granted) - each taking a
deadline. `core/uplink/` is everything that decides: the order, each step's deadline as a named
constant, **one budget per wake that the whole of it may spend**, the five named failures
(`modem-silent`, `no-sim`, `sim-locked`, `not-registered`, `no-data`) with a retry rule each, and
the backoff **counted in wakes** in retained storage - a Walter has no clock it can trust across a
deep sleep, so "every fourth wake" is a thing it can count and "in four hours" is not. `no-time` is
not among the failures: an uplink that came up without a time is up, and the time is reported beside
it. `components/modem/` is the thin part that calls the library.

Three things the modem's own documents and source decided, rather than this file:

- **UART2.** UART1 is the radio's (`platform/uart.cpp`) and the library's examples use it only
  because nothing else in them does; the modem's pins go through the GPIO matrix, so the port is
  ours to pick.
- **A deadline cannot cut a library call short.** Every blocking call waits on a condition variable
  with no timeout of its own; the library's command task gives up after
  `CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS`. So the policy checks the wall before and after each call,
  refuses to begin one it is already past, and reports an overrun when one comes back late.
- **The command attempts are set to 1** (`sdkconfig.defaults`), because the library retransmits a
  command that answered `+CME ERROR` - and three refused `AT+CPIN=` are a blocked SIM card.

A time is believed only inside a window (not before this firmware's date, not absurdly after it):
it is the time that sets the radio's clock, and `spec/LINK-PROTOCOL.md` §5.4 is explicit that a
clock which goes backwards makes a MeshCore node's own traffic look replayed. Reading it into
`interfaces::WallClock` - so that the link client has one to give - is the wake cycle's step 2 and
is task p3-05's; until then `main/` still answers "I have no time to give".

**HTTPS** (task p3-02): the modem's TLS, **over one of its sockets and not through its HTTP
client** — read at `walter-modem` 1.5.0, the modem's HTTP (`AT+SQNHTTPQRY`/`+SQNHTTPRING`/
`AT+SQNHTTPRCV`) hands back a response's status, content type and length and nothing else: no
`Location`, so a redirect cannot be followed (and a GitHub release download is one); no
`Content-Range`, `ETag` or `Last-Modified`, so a range cannot be checked or a changed file noticed;
and its receive omits the size (a "known bug" in the library) and then reads to the next `OK`,
which a binary image can contain. So `core/net/http1.h` writes and parses HTTP/1.1 itself - some
600 lines, every rule a host test, the parser fuzzed - over `interfaces/tls_socket.h`, whose one
implementation is `AT+SQNSD` in command mode with `AT+SQNSSCFG` naming a TLS profile. Every
request is one connection (`Connection: close`); a received `+SQNSRECV: <id>,<len>` is followed by
exactly `len` raw bytes, which the library reads by count, so the path is binary-safe.

**The chain is verified, and nothing in this firmware can ask otherwise.** The roots
(`keys/tls/*.pem`, ISRG Root X1 and USERTrust ECC; each with its subject, fingerprint, expiry,
source and the chains observed, held to the certificate at build time) are written into the modem's
certificate slots **once** - the loader keeps a hash of what it wrote in retained storage and
rewrites only what differs - and `WalterTlsStore::configureProfile` is the **one** call to the
library's `tlsConfigProfile`: it asks for the chain and the host name to be validated
(`URL_AND_CA`), TLS 1.2, and takes no argument that could ask for less. A host test and
`tools/check_rails.py` hold that the library's other validation levels are named nowhere in this
firmware and that the call is one. The modem validates against one slot per profile, so each root
has a slot and a profile and a host is held to one root (`core/net/trust.h`: `github.com` to
USERTrust, every other host to ISRG); a host whose chain ends elsewhere fails its handshake.

Above the socket: a **client** (`core/net/client.h`) that refuses anything but `https://` before
the modem is touched, follows at most three redirects and only to `https://`, never carries an
`Authorization` header to another host, reads 401/403 as *denied* and 429/5xx as *later* with
`Retry-After` capped at an hour, and cuts a body at the caller's bound rather than buffering it; a
**download** (`core/net/download.h`) in ranges of 64 KB, each with its deadline and three attempts,
resuming from what the sink holds, refusing a 206 whose `Content-Range` is not the range asked
for out of the size expected, restarting the sink from zero on a 200 (a server that ignores
ranges, or `If-Range` saying the file changed) rather than appending, never delivering more than
the manifest's size, inside a budget of bytes and seconds per wake; and the **fetch of a release**
(`core/update/fetch.h`): `manifest.json` and its `.sig`, small and bounded, checks 1-3 before a
byte of the image is asked for, the image into the store in ranges, checks 4-6 over the bytes as
they lie in flash, the record. The console's `http get`, `fetch` and `trust tls` drive them;
the wake cycle (task p3-05) will.

The self-update trial's third condition (S-6) is a TLS handshake with the configured server
(`core/update/tls_check.h`), its chain verified; no server is configured until task p3-03, so the
condition does not apply, as before.

## 10. One wake

The Walter sleeps deeply and wakes for one of three reasons: its timer, a start bit from the radio
(attention), or USB being plugged in (a person; stay awake). **As built (task p3-04), the third
is not a wake source**: `esp_sleep_enable_usb_wakeup()` answers `ESP_ERR_NOT_SUPPORTED` on the
ESP32-S3 — `sleep_modes.c` guards it on `SOC_PM_SUPPORT_USB_WAKEUP`, which the S3's `soc_caps.h`
does not define — and the one USB wake ESP-IDF has is light sleep's. So the rule became: a person
presses reset, and a boot that finds a USB host stays awake, while the host is there and for a
minute after the cable goes. The other two are the sleep timer and **ext1** on GPIO8 at a low
level (a start bit; the pad's pull-up held through the sleep), ext1 rather than ext0 because ext0
keeps the whole RTC peripheral domain powered and the sleep current is what the task exists for.

One wake is one pass of a fixed sequence, each step bounded in time and energy:

```
why did I wake → is the radio alive (supervisor) → set its clock → read power
     → may I use the modem (battery) → attach → talk to the backend → act on what it said
     → at most one update step → tell the radio I am going → sleep until the next time
```

How long until the next time comes from the battery: a table from state of charge and its trend to
an interval, with hysteresis. Readings taken while there was no uplink are kept in a bounded ring
and sent later.

As built (task p3-04), the parts of that sequence that are not the wake itself:

- **why did I wake** — `core/cycle/wake_reason.h`. A wake on the radio's line listens for §8's
  whole repeat window (about eleven seconds) for a valid `EV_ATTENTION`; nothing valid is a
  *spurious* wake, counted, and after three of them the line is disarmed as a wake source until
  the next timer wake. The last ten wakes, with their reasons and how long each lasted, are one
  record in RTC memory (`interfaces::RetainedSlot::kDutyCycle`), which also carries the band and
  `saving`'s every-Nth counter. A power-up is read as "the radio was reset with us and is still
  booting", which is the supervisor's own reading of the same three reset reasons.
- **how long until the next time** — `core/power/interval_policy.h`, a pure function; the table
  and every placeholder in it are `docs/power.md`.
- **tell the radio I am going → sleep** — `core/cycle/sleep.h`, and it is **the one way out of a
  wake**: the journal (when there is news), `SLEEP_NOTICE` as the last request (§8), the modem
  down, the pads safe and held, deep sleep. It refuses while the radio's update has a step in the
  air, while a transfer into the other application slot is open, and **while this firmware is on
  trial** — the bootloader turns a `pending-verify` slot into `aborted` at every reset, and a
  wake from deep sleep is one, so a sleep during a trial is a rollback of the image being tried.
- **never awake indefinitely** — `core/cycle/wake_guard.h`: a wake that has not reached sleep in
  five minutes is ended by a reset, the next boot's record says who asked, and the next sleep is
  *lengthened* by the fault rather than shortened. A USB host and the console's `stay` hold it
  off, and nothing else does. `main/` arms it once a boot (task p3-05) and the wake cycle is what
  reaches sleep; two things may **raise** the bound for the wake in hand — a climb of the
  supervisor's ladder and an update — and nothing can remove it.

The modem library's own `sleep()` is not used: it arms the timer and nothing else, and it gives
nothing a chance to hold a pad, write the journal or tell the radio first
(`tasks/phase-3/reports/01.md`).

**The wake itself** (task p3-05, `core/cycle/wake_cycle.h`, `docs/cycle.md`): ten steps in the
order above, each with a name, a precondition, a time bound and an outcome that is written down.
The preconditions are **data** — `kSteps` is the dependency table and a host test walks it — so
"the clock was not set because the uplink did not come up" is a row and not a comment. A step that
fails records why and the wake goes on to the next one that does not depend on it; a step whose
dependency failed is *skipped*, which is a different word from *failed*. **Every wake ends in
sleep**: step 10 depends on nothing, is never refused for want of time, and a wake that reaches it
having done nothing else has still done the one thing that matters. One compact line per wake —
reason, the radio's state, band, each step's outcome in a letter, the seconds awake, the next
interval — goes to the console's log, and to the journal when the wake has news (the same
arithmetic that keeps a sleep out of it, task p3-04).

A wake's budget is two and a half minutes of *ordinary* steps; a climb and an update are outside
it, each bounded by its own machine, because a wake that spent six minutes on a ladder must still
be able to reach its uplink and say so. `main/` runs one pass when there is no USB host on the
port and the console beside it when there is — `cycle now` and `cycle dry` are how a person sees
what a mast does by itself.

**The time of day** is `core/cycle/network_clock.h`, filled from the cellular network at step 6
and given to the radio at step 7 (§5.4). It is believed only inside task p3-01's window, it is
*not* carried across a sleep — the monotonic clock it is kept against restarts at zero, and a
clock reconstructed over a counter that restarted is the one §5.4 warns about — and a person's
`rak time set` does not go through it.

**The backend is an interface** (`interfaces/backend.h`). In these piles its implementation does
nothing (`core/cycle/null_backend.h`): what crosses the seam is a plain structure this firmware
fills and a **closed set of requests it already knows how to carry out by itself** — fetch a
release, run either update, change a non-secret setting, climb the ladder, nothing. Nothing there
names a server, a URL path, a JSON field or a job, and the mesh client's calls are not used at all
(a host test runs a simulated month of wakes and holds the radio to it). The MeshMaster agent goes
behind it when the server side is built (`spec/SYSTEM.md` §8, `spec/proposals/01` and `03`);
`docs/cycle.md` has the page for whoever implements it.

## 11. Flash layout

16 MB. The constraints the table was written to, and (task p0-01) the table itself, held to them by
`tools/check_partitions.py` on every build:

- two application slots with room to grow (3 MB each);
- **settings (NVS) and the image store above the application slots**, so that a factory image,
  which ends where the application ends, cannot reach them — the agent firmware learned this the
  expensive way;
- the image store sized for two radios (golden + candidate + scanner each, a radio image being at
  most the size of its application region — bounded here by the nRF52840's whole flash, 1 MB — and
  a base, which is Intel HEX text and so ~2.75 times the flash it describes). Task p2-02's layout
  and task p5-01's base use 6 242 304 of the 6 MB (§6) and leave 49 152 bytes;
- a journal that is not the settings store (its own wear, its own failure modes);
- a core-dump partition;
- ~~whatever the modem library needs for a modem-firmware update~~ — read in task p0-01: the
  library's own update path (MOTA, driven by BlueCherry) wants a FAT partition named `ffat` large
  enough for the modem image, 9 MB for `LR8.2.1.0`, which does not fit beside the rest in 16 MB.
  MOTA is off and nothing is allocated for it; a modem firmware update is a bench procedure
  (QuickSpot's `modem_firmware_flash` example flashed for the occasion, or the module's `UART1`
  per the datasheet). The 3.75 MB above `rakfw` is left unallocated.

`partitions.csv`:

| Name | Type | Offset | Size | |
|---|---|---:|---:|---|
| (bootloader) | | `0x000000` | | |
| (table) | | `0x008000` | 4 KB | `0x9000`–`0xDFFF` empty on purpose: a factory image fills it with `0xFF` |
| `otadata` | data/ota | `0x00E000` | 8 KB | |
| `ota_0` | app/ota_0 | `0x010000` | 3 MB | |
| `ota_1` | app/ota_1 | `0x310000` | 3 MB | |
| `nvs` | data/nvs | `0x610000` | 64 KB | above the slots |
| `journal` | data/`0x41` | `0x620000` | 64 KB | ours |
| `rakfw` | data/`0x40` | `0x630000` | 6 MB | ours: 2 radios × 3 images; the store uses 4.7 MB of it (§6) |
| — | | `0xC30000` | 3.75 MB | unallocated |
| `coredump` | data/coredump | `0xFF0000` | 64 KB | |

## 12. What is tested where

| | Host tests | Build only | Bench |
|---|---|---|---|
| pin rules | ✔ fake pins | GPIO calls, sleep holds | a scope on nRESET and KILL |
| link | ✔ fixtures, scripted radio | UART driver | through the bulkheads |
| SWD wire, DP, AP | ✔ wire-level simulator | bit-bang timing | IDCODE, the AHB-AP's IDR, the speed ceiling |
| nRF52, flasher | ✔ simulated chip, power loss | — | `rak swd id`: part, variant, protection; a real flash, the bootloader starts it |
| the one door (S-5a) | ✔ the simulated chip with both kinds of APPROTECT and every factory state: every grant against every port state, every write without an erase, every address the door refuses, an erase that never finishes, a soft reset on a part that drops its debug port and one that does not, a grant used twice, the tripwire firing in a test that did not arm it; and each factory state provisioned through the door — the base word for word, `UICR.APPROTECT` as the kind says, one pin reset, the port after it | — | task p5-03's command on a factory radio (`docs/bench.md` §4a, phase 6) |
| `rak provision` (S-5a) | ✔ the sequence over the door, against each factory state: provisioned word for word with one pin reset and the port after it; every refusal before the first write with the chip's counters at zero (the wrong id, `locked` for an open radio, an id for a closed one, a radio with an identity named by its key, no base, a base that fails 5p again, the radio held, the battery low, the kill line); an unknown variant refused with nothing erased on an open radio and, on a locked one, erased, refused, the table extended, finished; a verify difference named and the radio provisioned again; a power cut at every flash and UICR operation, at the chip erase and at the transfers between the writes, each followed by `rak provision check` and the line it says; the reset spacing waited out; the timing from the simulator's clock; a provisioned radio left alone across a simulated week of wakes, the first install clearing the mark over an erased settings page, a restart and a power cut keeping it; and the supervisor, an update and a week of wakes against a factory radio with the tripwire armed | — | a factory RUI3 radio through the D-subs at 125 kHz (`docs/bench.md` §4a, phase 6) |
| manifest, signature, descriptor | ✔ spec fixtures, RFC vectors | — | — |
| image store, journal | ✔ simulated flash, power loss | partition access | — |
| updates | ✔ scripted radio and flash | — | a deliberate bad image rolls back |
| supervisor | ✔ a radio silent on cue, fake pins, a simulated chip, a month of random behaviour | — | nRESET held to ground: the ladder climbs and stops; the Walter's own power-up is not an incident |
| the Walter's own update | ✔ fake slots with the bootloader's rules, a simulated reset | `esp_ota_*` calls | a trial confirmed, a trial failed, a reset mid-trial - on real OTA data |
| modem | ✔ the policy against a fake modem that is silent, has no card, a locked card, never registers, drops, gives no time or gives 1970; a month of wakes with no coverage | every modem call | a SIM, a tower |
| HTTPS | ✔ the URL rules, the HTTP/1.1 writer and parser (split headers, chunked, ten thousand random inputs), the client's redirects and statuses, the download against a scripted server that drops at every range boundary and mid-range, ignores ranges, lies in Content-Range, changes the file; the fetch of the specification's fixture release; the roots' fingerprints; the rail as a grep | the socket, the slots, the one TLS profile call | the chain of a wrong root refused on the modem; a real release fetched and resumed |
| **the rails** | ✔ the suite's own assertions (`host_test/rails/`): the pin rules' tally over every test that drove a wire, simulated flash's over every test that flashed (and, since task p5-02, every chip erase and UICR write counted against the chips a door test armed — the tripwire fails any other test at the first), the verdict's tokens as compile-time refusals, two secrets and the subscription's three numbers searched for in a day of wakes and every console command, a month asked for both updates and read off the journal, the supervisor's caps over a simulated quarter, and **the failure-injection sweep**: four suites' power-loss loops run together, a whole update from download to commit with the power lost at every flash operation of the store, the journal and the radio's chip in turn, each ending in the old image, the new one, or a journalled state the next wake resumes | `tools/check_rails.py`'s greps (one entry per rail, run before anything is compiled) and `tools/verify.sh`'s search of the built image for the test key's bytes and the map for a signing symbol | `docs/bench.md`, all of it: the rails are what a session can hold, and the walk is what a person must |
| the wake cycle | ✔ the good wake; each step failing alone; the dependency table walked as data; the radio as a scanner, silent (the ladder inside its caps, the uplink still used), mid-update (resumed, nothing else touching it) and on trial (confirmed before the wake may sleep); S-7 asked for both updates; a backend asking in `critical`, for a secret key and for a threshold that is not a setting; the budget running out and the backend recorded as *no time*; the telemetry ring filled, dropped, counted, survived a restart and emptied only when taken; the record's format held to `docs/cycle.md`; and a simulated month of 2 400 wakes with a sinking battery, a radio that goes quiet and a modem that drops - every wake in sleep, none past its bound, the caps held, **no mesh request sent**, no flash that was not asked for | `main/`'s construction and the one pass it runs | a day on a mast (`docs/bench.md` §16): `journal` one line a wake, `cycle dry`, `cycle now`, the band falling under a covered panel |
| duty cycle | ✔ the policy as a table over voltages and currents and a ±20 mV wander at each threshold, a simulated December and a simulated month of wakes over the whole bench, every wake reason against the scripted radio (an attention on the 1st, 4th and 10th repeat; noise; a scanner's lines), the spurious disarm and its re-arm, the bound on a wake, the pins checked at the moment of every sleep, the one-way-out grep | `esp_deep_sleep_start`, the ext1 arming, `rtc_gpio_*`, the USB connection monitor | the current asleep, a scope on nRESET and KILL across the transition, a wake by the radio's line, a chattering line disarmed, the energy of one wake with an attach, and the pack's own discharge curve - which is what makes every threshold real (`docs/bench.md` §15) |
| settings and the console | ✔ every key's range and refusal, the store over a map that is empty, corrupt, read-only or missing, the effect of each change on the thing it reaches, `erase confirm` touching the settings and nothing else, every command of the table grepped for a secret, the command table held to `help` and to `docs/console.md`, the session of `docs/provisioning.md` replayed | NVS itself (`nvs_get_str`, `nvs_set_str`, the commit) | the asterisk echo on a real terminal, `status` in an 80-column window, the `nvs` partition read over USB with both secrets in it, the SWD clock on a scope, a wrong PIN against a real card |
| **the mesh over the link** | ✔ the library's 46 runner scenarios a second time, unchanged, with `LinkMesh` and a simulated RAK between the runner and the simulated stock repeater — every send a frame, every reply an event, `client_tx` held equal to the repeater's sends at the end of every job; and 21 cases of the link's own: the lost event (one `REPLY_GET`, never a lost reply), the lost request (the link's retry, still one packet), the RAK's 120 s job timeout, a password read back as zeros from the mesh client, the link client and the RAK and found on the wire once, the sensitive CLI line's flag bit, a `password.set` leaving no value anywhere, the send's ceiling and the Walter's own timer with the wait ended by `JOB_BEGIN`, a late reply dropped and counted, a `boot_id` change under a job and under a wait, the path cached and read back, every core type carried with a session and none without, `client_tx` exact across a mix of jobs, and the local door for each of the thirteen types; the console's `rak mesh status` against the fixtures; the no-allocation check (task p4-02) | `components/agent/` in the board's link, reached by the console's probe; the no-ESP-IDF build step | `docs/bench.md` §17: a `status` on a neighbour over the air — one packet, one reply, `client_tx` up by one |
| **the uplink over the modem** | ✔ the library's 47 loop scenarios a second time, unchanged, with `ModemUplink`, the HTTPS client and the scripted server between the loop and the library's own scripted meshmaster.dk — every call a request over the firmware's HTTP/1.1, every fault a real 401, 429, 503, 422 or a refused handshake, every claim's bytes on the wire; and the uplink's own cases against the contract fixtures' bytes: each of the seven calls' route, method, headers and body, the reply parsed into the fixture's record; 401, 429 with `Retry-After` as seconds and as a date, 503 with and without, 500, 403, a TLS failure, no route, no uplink, a body one byte over the bound (and one that fits), a 200 the parser refuses, a redirect; the token read back as zeros from the uplink, the client and the transport after every call and found in no log line, error, URL or trace, on the wire once; the wake's deadline refusing a call before the modem is touched and bounding one that stalls; `https://` only; the 120-a-minute ceiling; and `agent url` / `agent probe` on the console with the token nowhere in what it printed (task p4-03) | `components/agent/` in the board's link, reached by the console's probe; the no-ESP-IDF build step | `docs/bench.md` §17: `agent probe` against meshmaster.dk — a 200 with a token from `/exit-nodes/new`, a 401 with a wrong one, a TLS failure against a host whose chain this firmware does not carry |
| **the agent core** | ✔ the library's own two suites, here, against the copied fakes and both fixture sets: `agent_core_runner` (46 cases — all thirteen job types, one send per command, the credential wiped, the lease, the offline buffer, and the fake's own citations) and `agent_core_loop` (47 — the loop, the heartbeat's bytes, the backoff, the reboot guard, every wake case, and what a sleep keeps: the image's bytes, a torn one refused whole, the uplink's ledger, nothing secret in the store), compiled from the same sources the board gets under the same sanitizers and `-Werror`; and the pin's two places held equal (`host_test/rails/test_rail_agent_core.cpp`) | the component in the board's link, all twenty-three objects of it, kept by `main/agent_anchor.cpp` until task p4-05 calls `wake()`; `tools/sync-agent-core.ps1 -Verify` (every file of both copied trees against its manifest, before the container starts) and `tools/check_rails.py`'s entry A (the library's boundary as an allow list, ten ways of skipping certificate verification, no store and no file system) | nothing: the library is linked and unused until p4-05, and what it then does is `docs/bench.md` §§17–18 and phase 4's own bench lines |

## 13. The agent

Everything above this section is a unit that looks after itself and its radio. This section is
what makes it a unit somebody can *ask* for something — a MeshMaster agent on meshmaster.dk,
registered by its key, heartbeating, answering the jobs the server hands out, and serving them
over the UART to the repeater it is bolted to and over the air to that repeater's neighbours
(`spec/SYSTEM.md` §8, `spec/proposals/01` and `03`).

None of that logic is written here. It is written, tested and proven on the air in
`twinoak/twinoak.meshmaster.exitnode-firmware`, whose portable half is the library
`lib/agent-core/` — and this firmware is its first consumer. The library arrives as a **checked
copy at one commit** (`components/agent-core/`, [`upstream.md`](upstream.md)): never edited here,
held byte for byte against a manifest, and bumped by a re-copy.

### What the library is

Thirteen job types and their rules; the login sequence and its four classes; the ACL restore that
ends an admin session; the config read and the config apply engines; the session cache; the
airtime budget and the uplink's own ledger of seconds and bytes; the agent API's envelopes and
result shapes; the reply codec that reads what a stock MeshCore repeater answers; the observation
ring; the offline result buffer and the image a sleep keeps it in; the rate limiter; the backoff
and the policy that reads the server's status codes; the log's formatting; and the loop that
drives all of it — for ever (`loop()`) or **one wake at a time** (`wake()`), which is the one this
firmware uses.

It is not our hardware. It has no uplink, no mesh, no storage, no clock, no console and no main,
and it includes nothing of Arduino, the ESP-IDF, WiFi, NVS, lwIP, FreeRTOS or MeshCore — an allow
list over every `#include` in it, which `tools/check_rails.py`'s entry A holds here as upstream's
own `test_library` holds it there. Its one dependency is ArduinoJson 7.4.3, from the component
registry.

### The three seams, and who fills them

```
                     ┌──────────────────────────────────────────┐
  core/cycle/        │  mm::AgentLoop::wake(intent, budget)     │   components/agent-core/
  wake_cycle.h  ───► │  mm::JobRunner  — thirteen job types     │   (copied, never edited)
  step 8             └───┬──────────────┬───────────────┬───────┘
                         │ IMesh        │ IUplink       │ IPlatform
                    ┌────▼────┐   ┌─────▼──────┐   ┌────▼──────────┐
                    │LinkMesh │   │ModemUplink │   │WalterPlatform │   components/agent/
                    └────┬────┘   └─────┬──────┘   └────┬──────────┘   (tasks 02, 03, 04)
                         │              │               │
              core/link/ │   core/net/  │   interfaces/ │
              the RAK on │   HTTPS over │   Clock, WallClock,
              UART1      │   the modem  │   Retained, Restart
```

| Seam | Ours | What it is made of |
|---|---|---|
| `IMesh` | `LinkMesh` (task p4-02, **built**: `components/agent/link_mesh.h`) | one request of `spec/LINK-PROTOCOL.md` §6 per call — the link was designed against this list (`SYSTEM.md` S-4): `JOB_BEGIN`/`JOB_END`, `PATH_SET`, the five `SEND_*`, `MESH_COUNTERS` — and §7's `OBS_PULL` for the advert ring (task p4-04 fills the hook). `awaitReply` is the `EV_MESH_REPLY`, waited for over the runner's own timeout plus a margin, with `REPLY_GET` as the fallback; the send carries a ceiling (§6.2 takes the timeout at the send, the library gives it at the await), and when this end gives up first the wait is ended with `JOB_BEGIN` again so the RAK is never busy for the next send. The path is cached and read back with `PATH_GET` after a flood's reply (a returned path is learned, §6.3). `counters().tx` is `client_tx`, never the radio's `tx`. `overheard()` is 0 and `rxByType` empty: §6.4 carries neither (a specification gap). `supports()` is *MESH_CLIENT in the session* for every core type, trace included — §6.2 has `SEND_TRACE` and §6.3 a trace record |
| `IUplink` | `ModemUplink` (task p4-03, **built**: `components/agent/modem_uplink.h`) | the seven calls of the agent API (API §3) over phase 3's HTTPS client (§9), **one request each**: `GET /agent/v1/config`, `POST …/heartbeat`, `…/observations`, `…/jobs/claim`, `…/jobs/{id}/result`, `…/extend`, `…/progress` under `server.url`, with `Authorization: Bearer <server.token>`, `X-Agent-Version: <this firmware's version>`, `Accept: application/json` and `Content-Type: application/json` on a POST; the body the library's writer made, carried unchanged; the reply read into one buffer of the object (2 kB, 6 kB for a claim), cut at the bound by the client and refused here, parsed by the library's own parser and wiped. The token is read from the settings for the call into one buffer, handed to the client as a pointer, flagged *sensitive* so the transport wipes the head it wrote the moment it is on the wire, and wiped when the client returns; it is in no log line, no URL, no error text. The `Call` record as `ServerPolicy` reads it: `reachable` is a status, whatever it said; `ok` a 2xx whose body fit and parsed; `retryAfterS` the header as seconds, 0 for absent or a date; `rateLimitRemaining` or -1; `via` "lte". No redirect is followed on an agent route (a 3xx is `ok = false`); `https://` only is the client's rule and the settings table's. The owner gives it the wake's uplink deadline and says when the modem is up; a call that could not finish in what is left is `reachable = false` with *budget* and touches the modem not at all. The library's 120-a-minute limiter is taken before every call |
| `IPlatform` / `IClock` | `WalterPlatform` (task p4-04) | mostly interfaces this firmware already has: `Clock::nowMs()`, `Clock::delayMs()`, `esp_random()`, `WallClock`, the heap figures, `Restart`, and the health record in retained storage for the self-reboot count |

And a fourth, **optional** and ours to fill because this unit sleeps (`agent/IRetained.h`,
`agent/KeptImage.h`, new in `v0.3.0`): a store that outlives a deep sleep. A result the server did
not take and an advert a wake could not post live in RAM, which the sleep takes with it — so the
promise *no job left leased and silent* held only within one boot. `AgentLoop::setRetained(store,
scratch, len)` hands the loop somewhere to put them and `restoreKept()` reads them back once a
boot: **one image a wake**, at most about 19.5 kB, written only when it differs from the one the
store already holds, refused whole when it is torn or foreign and counted when it is. The image
may hold nothing a result may not — the runner wipes every credential before a result is
written — and the store is bytes and a capacity, nothing more. No task of this phase was written
for it (it did not exist when they were); `capacity`/`write`/`read` over a flash region or an NVS
blob belongs beside `WalterPlatform` in task p4-04, and what it is worth — a week of results and
adverts surviving the sleeps that would otherwise drop them — is task p4-05's simulated month.

**The local door** (task p4-02). A job begun for the unit's *own* repeater — the key `IDENTITY`
answers, which task p4-04 gives `LinkMesh::setOwnKey()` — sends nothing on the mesh: the link has
console privileges (§5.7), so a login is answered by this end with a synthetic admin reply (the
bytes of the companion fixture `login-success.hex`, held to it by a test), a CLI line is §5.7's
`CLI` request (a sensitive one wiped like a login), a telemetry request is §5.8's `LPP_GET` at an
admin's permissions, and the access list is answered with silence, which the library reads as *no
ACL in this snapshot, no evidence either way*. Nothing of §5 yields the bytes of a status reply
(`RepeaterStats`), a neighbours page or an owner-info reply — `stats-*` and `neighbors` print
text — so those sends are `Failed`, which the library reports as `failed` with *the packet could
not be built*; a discovery and a trace of oneself the same. `client_tx` does not move for a local
job. Per core type:

| type | the unit's own repeater, over the UART | outcome |
|---|---|---|
| `telemetry` | `LPP_GET` at `0x07` | served |
| `cli`, `config.apply`, `advert.request`, `clock.sync`, `acl.set` | `CLI` lines | served |
| `password.set` | `CLI`, the password line sensitive, the restore after it | served; no value anywhere |
| `reboot` | `CLI reboot`: no answer, the expected silence (§5.7) | served, *ok as sent* |
| `config.read` | `CLI` lines; the access list silent | served, without the ACL |
| `discover` | the UART is the direct path: the synthetic login teaches it | served, zero hops |
| `status`, `neighbours` | no request of §5 yields the bytes the job parses | `failed` — link 1.1 |
| `trace` | a radio cannot trace a round trip to itself | `failed` |

The job types this unit advertises, the heartbeat's identity taken from the radio, and the
observations pulled out of the RAK's ring are task p4-04's; `AgentBackend : Backend` — the
implementation of §10's seam, which is **one call to `AgentLoop::wake()`** — and the firmware jobs
mapped onto phase 2's image store and update machines are task p4-05's.

### Where it sits in one wake

§10's step 8 is *talk to the backend*, and `core/cycle/null_backend.h` is what is behind it in
phases 0–3. Task p4-05 puts `AgentBackend` there and nothing else about the wake changes: the
step keeps its precondition, its bound and its outcome, and every rail of §10 — one update step a
wake, every wake ends in sleep, never awake indefinitely — still belongs to the wake cycle and not
to the agent. What crosses the seam stays a plain structure and a closed set of requests this
firmware already knows how to carry out.

`WakeIntent` comes from the interval policy of step 5 and the band; `Budget` from what is left of
the wake's seconds and the LTE plan. `WakeState` — 76 bytes, a trivially copyable POD carrying the
backoff, the accepted interval, the airtime budget, a 401 and the heartbeat's counters — goes into
a retained slot of its own before the sleep and is restored before the next wake: every duration in
it is a *duration* and not a deadline, because `millis()` restarts at zero after a deep sleep.

### What it costs, and what it is linked by

`mm::JobRunner` is tens of kilobytes of buffers — one scratch area every job shares rather than a
stack frame — and `mm::AgentLoop` is mostly its 64-entry observation ring. Both must be
namespace-scope objects, never locals, and task p4-05 is where they are constructed. Until then
nothing in this firmware calls a line of the library, so `main/agent_anchor.cpp` takes the address
of five of its functions through a volatile and `app_main` keeps the result: `--gc-sections` would
otherwise drop every object of it, and the board build would prove the library compiles without
proving it links. The anchor constructs nothing. Task p4-01's report has the size it adds.

### The rails the agent inherits

Six, all of them the library's own, and this firmware owns half of each: nothing the mesh says is
logged as text; a password is in RAM for the job and gone; **one send per command** — which across
the link is `client_tx` and the runner's transaction count being equal, exactly; TLS is verified;
a flood's path hashes are two bytes or more, never one; and the contract is the .NET agent's, one
dialect, posted byte for byte as the library wrote it. [`upstream.md`](upstream.md) says what a
bump may do to each.
