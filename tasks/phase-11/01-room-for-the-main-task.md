# 01 — Room for the main task

**Depends on:** nothing in this phase.
**Spec:** [evidence/](evidence/README.md) (the trial's core dump and what the bench read); `main/main.cpp` (~L185–210: the runner and the kept-image scratch taken from the heap in the static initialisers, and why; `app_main` ~L212); `sdkconfig.defaults` (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`, ~L84; `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG` and its paragraph, ~L91–110); `components/agent/include/walter_agent/agent_backend.h` (`kKeptScratchBytes`, ~L104); `components/agent-core` (`mm::JobRunner`); `components/modem` (`modem_trace`), `components/core/trace/redact.*`; `tasks/phase-4/reports/05.md` (*Decisions*: the runner on the heap), `tasks/phase-10/reports/06.md` (~L36–48 and *For FOLLOWUPS*: the 3 080 bytes and the *37 037 bytes of DIRAM free*); `docs/bench.md` §13 (the trace build); ESP-IDF in the pinned image (`components/freertos/app_startup.c`, `components/heap/port/esp32s3/`, `components/esp_system/` startup), read there and not from memory.
**Size:** one session.
**Model:** claude-opus-5

## Goal

v0.1.3 never started on a board. Its trial on LTE Exit Node 1 ended in
`assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)`. ESP-IDF could not create
the main task, because no free block of the heap held its 16 KB stack once the static initialisers
had taken the runner (~66 KB) and the scratch (20 KB). v0.1.2 starts on the same board. Between
them, phase 10 added static storage: at least the modem library's `_logEscapeBuffer` (3 080 bytes,
there because of `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`) and the redactor's 322 bytes.

This task finds out **how much room there was and how much there is**, and puts it back. It does
that from the builds, with a model that has to explain both facts: 0.1.2 starts, 0.1.3 does not.
It does not use a number that looks about right.

## Deliverables

1. **Both releases measured.** Build `v0.1.2` and `v0.1.3` at their tags in the pinned image (the
   same way `tools/verify.sh` builds). For each, from the map and the ELF:
   - `.dram0.data`, `.dram0.bss`, `_heap_start`, and every other symbol that bounds a heap region;
   - the regions ESP-IDF registers for the esp32s3 heap at the pinned version, and which of them
     exist when the main task is created (the ROM's reserved ranges included);
   - `sizeof(mm::JobRunner)` and `kKeptScratchBytes`, from the DWARF (the toolchain's `readelf` or
     `objdump`) or a `static_assert`, whichever you can show;
   - every allocation made before the main task: ESP-IDF's own (the IPC tasks' stacks and TCBs and
     anything else its start-up allocates) and the static initialisers' `new`s, **in the order they
     run** (`.init_array`).

   The table of both goes in the report, with the delta and what it is made of.
2. **A model, `tools/heap_at_start.py`.** It takes a build directory and prints the regions, the
   allocations in order, where each one landed, and the largest free block when the main task asks
   for `CONFIG_ESP_MAIN_TASK_STACK_SIZE` plus its TCB. It must say *fits* for 0.1.2 and *does not
   fit* for 0.1.3. It is deterministic: the region table and the allocator's placement rule come
   from ESP-IDF's source at the pinned version, and the allocations from the code. If it cannot
   explain both builds, the report says so and what is missing. It does not get a fudge factor.
3. **The room back, in the release build.** The first candidate is to take
   `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG` out of `sdkconfig.defaults`:
   - the modem trace goes back to a trace build (`docs/bench.md` §13 says how to make one);
   - `trace modem on` in a build without it says so in one sentence and switches nothing on;
   - its host tests stay.

   Other savings are allowed if the report gives their bytes. Leave the 16 KB main stack alone: it
   was raised after an overflow, and no high-water figure from a board says it can come down.
   **Target:** the model says the main task fits with **at least 8 KB to spare** in the block it
   gets, and the report says what 0.1.2's margin was.
4. **The words.**
   - `docs/ARCHITECTURE.md` gets a short section, *The heap at start*: what is taken before
     `app_main`, in what order, and why the linker's *DIRAM free* is not the figure that decides
     whether the image starts.
   - Report p10-06's *37 037 bytes free* is history and stays as written. The places that quote
     it as a budget say what the budget really is.
   - Tick phase 10's *escape buffer* follow-up with a pointer to this report.

## Acceptance criteria

- [ ] The report has both tags' table and the model's output for both: 0.1.2 *fits*, 0.1.3 *does not fit*.
- [ ] This task's build: the model says *fits*, with at least 8 KB to spare.
- [ ] `trace modem on` in the release build says it is not in this build (host test).
- [ ] `tools/verify` and the host tests green; no new code runs before `app_main`.

## Out of scope

- The rail that fails a build (02 makes 01's model one).
- Moving the runner or the scratch out of the static initialisers. The 23 October boot is why
  they are there; if the numbers argue for it, it is a follow-up.
