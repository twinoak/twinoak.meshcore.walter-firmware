# 03 — A rollback says what killed it

**Depends on:** nothing in this phase.
**Spec:** [evidence/](evidence/README.md) (a real core dump: [`coredump-0.1.3-ota-trial.bin`](evidence/coredump-0.1.3-ota-trial.bin)); `components/core/update/self_update.cpp` and `include/core/update/self_update.h` (`SelfReason`, `kNeverBooted` ~L135, the finding at boot), `components/core/include/core/update/journal.h` (the entries and their bounds), `docs/update.md` (*After a power cut*, the table ~L415–432); `components/agent/firmware_jobs.cpp` (the apply's result; `twinoak.fw.state` ~L969–1100); `components/core/health/` and `components/platform/boot.cpp` (the reset reasons); `partitions.csv` (`coredump` at `0xFF0000`), `sdkconfig.defaults` (`CONFIG_ESP_COREDUMP_*`, ~L77–81); meshmaster.dk, read only: how a job page renders the apply's result and journal (`FirmwareApplyOutcomes`, the job page).
**Size:** one session.
**Model:** claude-opus-5

## Goal

On 9 November the unit said *the image never reached its main loop (the bootloader gave up on it)*,
and that was all meshmaster.dk could show. The reason was on the board the whole time: the old
image booted with `esp_reset_reason()` = panic, and the core dump partition held the trial's panic
line. Reading it took a person at the bench with esptool. This task makes the unit report it.

## Deliverables

1. **The finding at boot keeps the evidence.** When the old image finds that a trial was ended by
   the bootloader (the rows of `docs/update.md`'s table that end in *rolled back*), it records:
   - this boot's reset reason, which is the reset that ended the trial;
   - if the `coredump` partition holds a dump whose checksum holds **and whose application ELF
     SHA-256 is the trial image's** (its app description in the other slot): the panic line and
     the crashed task's name.

   A dump from another image is never attributed to this trial. It is recorded only at the boot
   that makes the finding, because the next wake's reset reason says nothing about the trial.
2. **It goes where a person looks.**
   - The journal's *rolled back* entry, bounded like every entry.
   - The apply's result: the reason the server shows, e.g. *rolled back: the image never reached
     its main loop - panic: assert failed: esp_startup_start_app app_startup.c:86*.
   - `twinoak.fw.state`: the last trial's reset reason, and whether a dump is present with its
     panic line.

   If meshmaster.dk needs a change to show any of it, that goes to its FOLLOWUPS through the
   report. This repository does not change it.
3. **`coredump` on the console.** It says whether there is a dump, whether its checksum holds,
   which image wrote it (its ELF SHA-256 against both slots), its panic line and its tasks.
   `coredump erase confirm` erases it. Nothing erases it by itself: the one on 9 November was read
   because nothing had.
4. **The reader is host-tested on the real dump.** Read the dump yourself (its ELF notes:
   `ESP_CORE_DUMP_INFO`, `ESP_PANIC_DETAILS`, the TCBs' names) in code the host tests can run, and
   test it on `evidence/coredump-0.1.3-ota-trial.bin`: panic line, task names `ipc0` and `ipc1`,
   ELF SHA-256 starting `5c1cb8f37`.
   - If you use ESP-IDF's `esp_core_dump_get_panic_reason()` on the board instead, say why in the
     report, and the host tests still read the fixture.
   - A dump with a broken checksum, a truncated one and an empty partition each have a test.
5. **`docs/update.md`** (the table's *rolled back* rows) and `docs/bench.md` §10 (*a version built
   to fail its trial*) say what the reason now carries.

## Acceptance criteria

- [ ] The fixture's panic line, tasks and ELF SHA-256 read in a host test.
- [ ] A trial that panicked reports the panic line in the journal, the apply's result and `twinoak.fw.state` (host tests with fakes for the reset reason and the partition).
- [ ] A dump whose ELF SHA-256 is not the trial image's is not attributed (test).
- [ ] Every new string is bounded and every new field has its test; `tools/verify` and the host tests green.
