# 01 — A delay that yields

**Depends on:** nothing in this phase.
**Spec:** `../phase-8/FOLLOWUPS.md`, *A busy wait below one tick still starves the watchdog* (7 November, with the decoded backtrace), and p8-01's item and report (`../phase-8/reports/01.md`); `components/platform/clock.cpp` (`EspClock::delayMs`: below one tick, `esp_rom_delay_us`), `components/interfaces/include/interfaces/clock.h` (*never shorter than asked*), `components/agent/walter_platform.cpp` (`WalterPlatform::delayMs`: *a yield and never a busy spin*, which it is not), `components/agent-core/src/JobRunner.cpp` (`idle`: `delayMs(1)` in a loop, read only: it is agent-core's), `components/core/flasher/flasher.cpp` (p8-01's `letIdleTaskRun`), `components/modem/walter_tls.cpp` (the receive wait, `delayMs(5)`), the link client's polls; `sdkconfig.defaults` (`CONFIG_FREERTOS_HZ`, the task watchdog's lines); ESP-IDF v5.5's notes on `CONFIG_FREERTOS_HZ` and `pdMS_TO_TICKS`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

On 7 November, Walter 0.1.1 reset during a `config.read` of the unit's own repeater:
`task_wdt: … IDLE0 (CPU 0) … CPU 0: main`. The backtrace runs from `JobRunner::idle` through
`WalterPlatform::delayMs` to `EspClock::delayMs`'s busy branch.

agent-core waited out the airtime budget, up to about 40 s, with `delayMs(1)` in a loop. Every one of
those was `esp_rom_delay_us(1000)`, because 1 ms is less than a tick at 100 Hz. p8-01 put pauses into
the flasher's loop. This task fixes the clock, so that no loop of this firmware can do it again.

## Deliverables

1. **`EspClock::delayMs(n)` blocks for every n ≥ 1, and never spins.** Two ways; choose one and
   say why:
   - `CONFIG_FREERTOS_HZ=1000`, so that a millisecond is a tick. Check every `pdMS_TO_TICKS` and
     every tick count in this firmware, ESP-IDF's and the modem library's assumptions, the power cost
     of a 1 kHz tick on a unit that sleeps, and p8-01's `letIdleTaskRun` (10 ms becomes ten ticks).
   - Or a floor of one tick for any n ≥ 1, which makes a 1 ms poll a 10–20 ms one. Measure what that
     does to the link client's polls and the modem's receive loop.

   `delayUs` stays a busy wait: the SWD wire's half-bits and short UART turnarounds need it, and none
   is long.
2. **Every busy loop, found and named.** Grep for every loop that waits on the clock:
   `delayMs`, `delayUs`, `nowMs` polled. Name in the report each one's longest wait and whether it can
   now block. Any loop that can run for more than a second without blocking gets a pause between
   operations, as p8-01 did.
3. **The test that would have caught it.** The host clock's tick rule from p8-01 applies to
   `WalterPlatform::delayMs`.
   - Run a fake agent wake with a 34-line local `config.read` at a budget of 2 a minute, through the
     real `JobRunner` (agent-core as synced) and the real `LinkMesh` over the fake radio.
   - Assert no stretch of simulated time longer than 2 s without a blocking wait.
   - It fails before the fix. Show the failure in the report, with the longest stretch it measured.
4. **The watchdog stays as it is**: 30 s, panic, both idle tasks watched.
5. **`docs/bench.md`**, for Thomas: a `config.read` of the unit's own repeater with the budget as it
   is, and `boot` afterwards never says `watchdog`.

## Acceptance criteria

- [ ] `EspClock::delayMs(n ≥ 1)` blocks; the choice and its costs are in the report.
- [ ] Every clock-waiting loop is listed with its longest wait; none can spin past a second.
- [ ] The 2 s test fails before and passes after; the watchdog is unchanged.
- [ ] The `../phase-8/FOLLOWUPS.md` item ticked (its fix part 1); `tools/verify.ps1` green.

## Out of scope

agent-core's own changes: a local job skipping the budget is the exit-node firmware's task 40, and 05
here syncs it.
