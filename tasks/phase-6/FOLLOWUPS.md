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
