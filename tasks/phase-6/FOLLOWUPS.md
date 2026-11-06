# Phase 6 — follow-ups

Review findings and what a walk saw, tagged with the task that must pick them up (`[02]`),
`[Thomas]` or `[reviewer]`. Only the reviewer ticks an item.

## Written before the run (22–23 October)

- [ ] **[03] The radio must run after `rak provision` without a power cycle.** On 22 October the
  hardware repository's bench tool (`software/firmware/walter-swd-bench`) provisioned the first radio,
  and its bootloader only started after a power cycle. That tool never withdraws CDBGPWRUPREQ and
  CSYSPWRUPREQ, and it clears halting debug in one DHCSR write without resuming the core first, so it
  leaves the nRF52 in Debug Interface mode. This firmware's `Nrf52::run()` and `Nrf52::close()` do both
  properly, and `Provision::resetAndLook` calls them before its pin reset. 03 shows it on the board:
  after `rak provision`, and again after the first install over SWD, the bootloader (UF2 drive with
  `rak_usb`) or the application (`rak hello`) answers **with no power cycle in between**. A radio that
  needs one is a finding against `rak provision` and the update path. An update that leaves the radio
  silent fails its health check and rolls back every time.
- [ ] **[Thomas] Before 03: is this radio's identity disposable?** On 23 October the first radio
  (`E926F6BEB407A412`) is being registered by hand on meshmaster.dk with the key its repeater image made.
  A `rak provision` erases that key, and meshmaster.dk then sees *companion changed*. Either run phase 6
  with `"radio": "keep"`, in which case 03 reports `rak provision` as *not verified* (the next new radio
  is its walk), or accept re-registering the repeater afterwards.
- [ ] **[Thomas] The unit's power during the run:** the bench supply only, at a 2 A limit for the
  modem's bursts, until U3 is reworked (README, *Before you start*).

- [ ] **[02] The main task's stack, measured.** The first `lte up` on a board (23 October) overflowed
  the main task at ESP-IDF's default 3584 bytes: a panic reset, and a core dump in the coredump
  partition. Everything this firmware does runs in that one task. `sdkconfig.defaults` now sets
  `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`, by hand and without a measurement. 02 adds the
  high-water mark (`uxTaskGetStackHighWaterMark(NULL)`) to a console line the bench reads, after
  `lte up`, `agent probe`, `agent wake`, `rak update` and a signature check, and sizes the stack from
  the worst of them with a margin it states. A rail or a test holds that the setting is not left at
  the default, since a host test cannot see a stack.

- [ ] **[02] The modem refuses root 1, USERTrust ECC (slot 13, profile 3).** On the first board
  (23 October), ISRG Root X1 went into slot 12 and profile 2 at the first request, with the radio on,
  and matches. USERTrust ECC failed every time, in 200-600 ms, before radio-on and after. Either
  `AT+SQNSNVW="certificate",13,<size>` or `AT+SQNSPCFG=3,...` is refused, and the firmware cannot
  yet say which. Since then a request waits only for its own root (`TrustLoader::ensureRoot`,
  `core/net/http_over_tls.cpp`), so meshmaster.dk (root 0) is not blocked by it. What still needs it
  is `fetch` from github.com, which the self-update uses. 02 prints the modem's reply to each of the
  two writes in `trust tls`, finds which one and why (a P-384 root on this modem's TLS stack? the
  slot?), and either fixes it or finds github.com's chain another root this modem takes.
- [ ] **[reviewer] Five fixes from the first hand-run on a board** (23 October, uncommitted when
  this was written, one PR): the main task's stack (above); the agent runner and kept-image scratch
  taken in the static initialisers, because the 66 kB runner fits only in the heap region the stack
  is also taken from (`main/main.cpp`); `AT+CPIN?` polled until the card answers definitely
  (`walter_uplink.cpp`, `sim`); no `AT+CGATT=1`, whose refusal while searching failed every bring-up
  (`walter_uplink.cpp`, `attach`); `ensureRoot` (above); and `+CME ERROR: 4` to `AT+SQNSRECV`
  read as "nothing buffered yet", where it had hung up every TLS connection 10 ms after the request
  (`walter_tls.cpp`, `receive`; found with the library's own TX/RX trace, a bench build that is not
  in the PR). None of them could be seen by a host test: every fake modem answered as the code
  expected. Each one's comment says what the board showed. **The fakes want the same lesson**: the
  scripted modem of `host_test/` should answer `+CME ERROR: 4` and `+CME ERROR: 10` where the real
  one did, so that a test holds each fix.

- [ ] **[04] A person's `agent wake` keeps the 20-minute bound for the rest of the boot.**
  `AgentBackend` calls `guard().allow(kBackendBoundMs)` (`components/agent/agent_backend.cpp`, about
  line 307), and `allow` only ever raises the bound, counted from the boot's `arm()`. On the first
  board (23 October), after a bench session with `agent wake` and the cable pulled, the Walter stayed
  awake at about 43 mA until a power cycle. It would have restarted into a normal wake 20 minutes after
  boot plus the minute's grace, so nothing was wrong, but it was invisible. 04 decides whether a
  console `agent wake` lowers the bound again when it returns, and says so on the console.
- [ ] **[02] Asleep, measured once:** after a power cycle with no cable, one wake (about a minute,
  jumpy), then **8 mA** for the whole unit on the lab supply at 3.7 V: the repeater listening, the
  Walter in deep sleep with the modem at `CFUN=0`, and connector B. §16's microamp walk splits that
  between the boxes.
- [ ] **[p8-02] `walterctl.py push` cannot finish on a board: the transfer's answers go
  through the console's LF -> CR LF conversion.** First tried 6 November, repeater v0.1.0 as the
  candidate into a Walter running v0.1.0: the hello and the first frames went through, then
  `no answer to type 0x03 seq 9` four times, and the console printed what walterctl could not read
  (`again` among the bytes). `Receiver::respond` (`components/core/image/receive.cpp`) writes its
  frames through `interfaces::TextOut`, and `platform::UsbConsole::write` turns every 0x0A into
  0x0D 0x0A for the terminal. The answer to chunk 9 carries `expect_seq` 10 = 0x0A, so it is
  corrupted every time; any 0x0A in `written` or the CRC does the same earlier or later. `self
  receive` uses the same receiver and is broken the same way; the LTE fetch is not (it writes the
  store directly). The host tests could not see it: `console_host` and the captures write bytes
  verbatim. The fix: a raw write for frames (a `writeBytes` beside `write`, verbatim on the
  platform and looping until every byte is out, bounded), `respond` using it; and the LF -> CR LF
  conversion moved into one core helper that `console_host` uses too, so that
  `tools/test_walterctl.py` runs against the conversion the board has and holds the fix with a
  transfer of more than ten chunks. Until it is released, `walterctl.py push --undo-crlf` takes the
  conversion back out on the laptop (its exact inverse, held by `tools/test_walterctl.py`); the
  flag is for a Walter at 0.1.0 and must not be used against a fixed one. The same evening showed
  walterctl's own serial read waiting out the whole 5 s reply timeout for every acknowledgement
  (`read(4096)` returns early only at 4096 bytes): about 2.5 hours for one radio image. Fixed in the
  tool (`SerialPort.read` returns what has arrived), with a progress line per tenth of a file; the
  console host's pipe never showed it, so a test against a real port is the bench's.
- [ ] **[p8-05] A unit with no golden image cannot be offered a radio release over LTE.**
  Without a golden image, `twinoak.fw.state` (`FirmwareJobs::writeStateResult`,
  `components/agent/firmware_jobs.cpp`) reports a slot's role and board from `HELLO`, and only when a
  link session is up, and never its band. meshmaster.dk picks the image by role, board **and** band
  (`FirmwareRollout.PickImage`), so the canary picker and the offer both say *Read the unit's state
  first* for ever: on 6 November job 20990 reported `{"slot": 0}` and nothing else. So a unit's first
  install can only come over the cable (`docs/field.md` step 3 says so, but the server's page
  suggests otherwise). Either the state brings `HELLO`'s band too (if the link carries one; otherwise
  the descriptor of what the radio runs, read over SWD), or meshmaster.dk says on the canary page
  that a unit with no golden image takes its first install over the cable.
- [ ] **[p8-01] The SWD flasher starves the task watchdog: no radio image can be
  flashed.** 6 November, the first `rak update --first-install` on a board (repeater v0.1.0, 417 kB,
  SWD at 125 kHz): `boot 19: watchdog reset`, the journal resumed *flashing-candidate* at every boot,
  and after twelve watchdog wakes in a row the run ended `STUCK: no image to return to`. The
  flasher's page loop (`components/core/flasher/flasher.cpp`: read, erase, write, read back, a page
  at a time) and the SWD wire never block: `EspClock::delayMs(1)` is a busy wait below one tick
  (`components/platform/clock.cpp`, 10 ms at 100 Hz) and the wire's half-bit waits are
  `esp_rom_delay_us`. A whole image is minutes of CPU without the idle task running, and the task
  watchdog (30 s, panic, idle tasks of both cores watched) resets the Walter. The fix: a blocking
  pause of at least a tick between pages - between SWD transactions, never inside one - in the
  page loop, the final hash pass, and `rak provision`'s base write (`provision_door.cpp`, the same
  shape and 185 kB of flash); and a host test that fails when the simulated clock runs more than a
  few seconds of flashing without such a pause. Until then **no `rak update` and no `rak provision`
  on 0.1.0**: the radio is recovered over its own USB-C (double-tap reset, the release's `.uf2`),
  which writes the application region only, so the node's identity stays.
- [ ] **[p8-04; meshmaster.dk 4.25; Thomas] A unit's own repeater, read through its own Walter.** 6 November:
  *Read now* on the unit's repeater was refused (a guest read, no guest password stored, no guest
  pair), the advice was to pair LTE test node with its own repeater at the guest door, and the
  pairing's discover then went out on the air from the radio to itself. Three things, one per place:
  (1) **Walter:** the local door (`LinkMesh`, `job_.local`) is armed only once `Identity::refresh()`
  has the radio's key, which needs a `HELLO` in this boot; a console `agent wake` with no `rak hello`
  before it ran the job as an ordinary mesh job. The backend should open the session and read the
  identity before it runs a job, or refuse a job it cannot yet tell is local. (2) **meshmaster.dk:**
  a unit's own repeater should never need a pair: *Read now* through its uplink should run at the
  class the scheduled probes use (`ScheduledRepeater.PrimaryIsUplink`, acl) and never at guest
  (`Routing.cs`'s comment says the uplink is deliberately not exempt from `GuestLoginRule`), and the
  Exit nodes tab should not offer the association wizard for the unit's own uplink, whose discover
  the local door refuses. (3) **Both:** status, neighbours and owner-info have no request on the
  link (`link_mesh.h`: *Failed* for a local job), so the unit cannot read its own repeater's status
  through the Walter at all yet; telemetry (LPP_GET) and CLI lines can. Decide whether the link gets
  a request for them or the CLI's stats lines stand in.
- [ ] **[p8-03; meshmaster.dk 4.25] A response body that stalls, and the job it orphans.** 6 November, after the evening's
  nineteen watchdog boots: heartbeat 200 (143 bytes, 4.5 s), then `/agent/v1/jobs/claim` -> `http 200
  but the body did not arrive whole` after 31 s with the library's `Command time-out (TX) Attempt 1 of
  1`, and `/agent/v1/observations` the same after 46 s. The server had leased the claimed job
  (21026, `twinoak.fw.state`, attempts 1 of 1), the Walter never saw it, and the lease ran out with
  nothing to report. Earlier the same evening (job 20990, 19:43 UTC) claim, job and result all went
  through. A full power cycle did not end it (RSRP -86 dBm, RSRQ -15 dB, TDC band 20): `config`
  200, then `heartbeat` `did not go out` after 506 ms. **The cause, read from the code:** every
  connection is socket 1, and `WalterTlsSocket::open` clears `g_closed`/`g_ring` before dialling.
  The library turns the modem's `+SQNSH: 1` URC (the server's close) into a queued event that its
  event task dispatches later, 10 ms after taking it off the queue (`WalterModem.cpp`, the `+SQNSH: `
  URC and the dispatch loop), and on the way it sets socket 1's state to FREE. A request that ends on
  its Content-Length does not wait for that close, so the previous connection's close lands after
  the next `open` has cleared the flag: `send` sees `g_closed` and stops (`did not go out`), or
  `receive` ends early (`the connection closed before the body was complete`), and the library may
  hold the new connection as FREE. Timing decides, which is why some wakes get through. The fix:
  alternate the socket id between connections (1 and 2) and filter the events by the current one, so
  that a late close of the last connection cannot touch this one; and a test whose fake modem
  delivers the previous connection's close after the next dial. For meshmaster.dk: a claim
  whose answer is lost leaves a one-attempt job dead; a read-only type (`twinoak.fw.state`) could be
  given a second attempt after an expired lease with nothing reported.
