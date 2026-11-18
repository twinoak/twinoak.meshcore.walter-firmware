# 05 — The room is in the PSRAM

**Depends on:** 04
**Spec:** `sdkconfig.defaults` (~L19–22: *PSRAM stays off*, and that the task that first needs it decides; ~L98–106: `.bss` is what decides whether an image starts), `tasks/phase-0/reports/01.md` (~L184, the decision), `docs/ARCHITECTURE.md` §14 (*The heap at start*: the four internal heaps and the order they are tried in), `tools/heap_at_start.py` and rail R22, `tools/release/boot_gate.py` and rail R23 (`docs/release.md` ~L166–180), `components/core/include/core/console/heap_report.h`, `components/platform/heap.cpp` and `components/platform/include/platform/heap.h`, `components/interfaces/include/interfaces/README.md`; the console's `heap` (`components/core/console/console.cpp` ~L915) and `docs/bench.md` §1 (the ★ `heap:` line, ~L135–155); task 03's room and its `room` line, task 04's posts. In the pinned ESP-IDF, read in the container: `components/esp_psram/` (its Kconfig and `esp_psram.h`) and the guide *Support for External RAM* (`docs/en/api-guides/external-ram.rst`): the ways external RAM is used, what happens when none is found, and its *Restrictions*. The Walter datasheet as `sdkconfig.defaults` cites it: the module, and the variants' PSRAM.
**Size:** one session, a long one.
**Model:** claude-opus-5

## Goal

A step of a session keeps its lines in a room of 8 192 bytes, because that is what the internal
heap of a unit in service has in one piece: *27472 bytes free, largest free block 12288*. It
makes a minute of `scope` about thirteen frames of 250 points.

The module has 2 MB of PSRAM, which this firmware has left switched off since its first task
with the note that the first task to need it decides. This is that task. Every Walter Thomas
has is the N16R2: 2 MB, quad.

The PSRAM gets one job, the room of a session. Nothing this board needs in order to boot, to
keep its radio, to update or to reach the server may come to depend on it.

## Deliverables

1. **The room of a session is 65 536 bytes of PSRAM when there is PSRAM**, and task 03's
   8 192 bytes of the internal heap when there is not, through the seam task 03 asks its room
   of. 64 kB because six steps of it are 384 kB, inside the 512 kB the server keeps of one
   session, and because a minute of `scope` at a frame a second and 250 points is 30 kB.
2. **An ordinary wake does not touch the PSRAM.** It is brought up when a session takes its
   room and at no other moment, so a wake that runs no session boots and runs as it does
   today. If the pinned ESP-IDF has no supported way to bring it up after boot, it is brought
   up at boot with a PSRAM that is not found ignored, and the report says what was tried and
   why it could not be the first way. Either way:
   - a board with no PSRAM, with PSRAM of the other kind, or with one that does not answer
     boots, runs, and holds its sessions in the 8 192 bytes;
   - `malloc`, `new`, every task's stack, `.data`, `.bss` and everything the copied library
     allocates stay in internal memory, exactly as today. Only the room asks for external
     memory, and asks for it by name.
3. **Tested when it is taken.** A pattern written over the whole room and read back, before
   the first line goes in. A room that fails is given back, and the session takes the 8 192
   bytes instead.
4. **Touched from a task and never from an interrupt.** No interrupt handler, and nothing
   that runs while the flash cache is off, holds a pointer into the room (the guide's
   *Restrictions*). A post's body is sent out of it by the path task 04 already uses; the
   report says which task does that reading.
5. **Nothing is kept there over a sleep.** A sleep is a boot.
6. **The start is not made tighter.** `tools/heap_at_start.py` over this task's build against
   main's: the main task still fits (R22) and the margin is not below main's. If switching
   the PSRAM on costs internal memory the margin does not have, the firmware is left as it
   was: none of this task's code is merged. The report says the figures and what would pay
   for it, and the report alone is merged, so that a run of the pile goes on to the next task.
7. **Said.**
   - `heap` prints one line more: the PSRAM's size and how much of it is free; or *not
     brought up in this boot*; or *none found*.
   - The session's `room` line says where the room came from, inside its 72 bytes:
     `65536 psram of 2097152; internal 27472, block 12288; least 25300`. When the heap gave
     the room it is task 03's line.
   - `heap room` takes the room exactly as a session would (the PSRAM brought up, the
     pattern), prints the `room` line and gives the room back. It is how a Walter alone on a
     cable, with no radio to turn into a scanner, proves its PSRAM.
   - The boot's `heap:` line and the heartbeat's two heap figures are about the internal
     heap, as today.
8. **`sdkconfig.defaults`**: every line added with its reason, and the note *PSRAM stays off*
   rewritten to what is now true, with the other variant named and what happens on it.
9. **The release still boots where there is no PSRAM**: `tools/release/boot_gate.py` over
   this task's release files, in the QEMU it uses (R23).
10. **The words.** `docs/ARCHITECTURE.md` §14, a paragraph: what the PSRAM is used for here
    and what it is never used for. `docs/update.md` where task 03 described the room,
    `docs/console.md`. And `docs/bench.md`, as ⚠ items that need a board, because no test
    here can bring a PSRAM up:
    - on a Walter alone: `heap`, then `heap room`, then `heap` again: *not brought up in this
      boot*, the `room` line saying `psram of 2097152`, and the PSRAM's size with all of it
      free again;
    - on a board with a radio: `rak session 60 scope 863 870 250 0 1000`, and the session's
      `room` line saying `psram`;
    - that step: sixty `scope` lines, ended `clock`, about 30 kB;
    - the boot's `heap:` line against main's on the same board (§1's ★ item): no kilobyte
      lost;
    - a wake with `stay` off and no session: as long as before, and no line about the PSRAM.

    One line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] With the external memory scripted as present: a session's room is 65 536 bytes, the pattern is written and read back before the first line goes in, and the `room` line says `psram` with the scripted figures in 72 bytes or less (tests).
- [ ] With it scripted as absent, as failing to come up, and as failing its pattern: the room is 8 192 bytes of the internal heap, the session runs, and the `room` line is task 03's (tests).
- [ ] A `scope` step scripted at sixty frames of 482 bytes: in the big room it ends `clock` with sixty lines; in the small one it ends `full` with every line it kept whole (tests).
- [ ] A step of 30 kB of lines: four posts of at most 8192 bytes, each cut between lines, each `{from}` the count before it (test).
- [ ] `heap room` with the external memory scripted as present prints the `psram` line and leaves nothing taken; scripted as absent it prints task 03's line (tests).
- [ ] A wake that runs no session never brings the PSRAM up (test). If deliverable 2's second way was taken instead: a boot with none found goes on, shown in QEMU.
- [ ] Nothing but the session's room asks for external memory: a rail in `tools/check_rails.py` over the sources, or a test that says how it holds.
- [ ] `tools/heap_at_start.py` says *FITS*, and the report sets its three figures beside main's, both built in the same container. Rail R22 green.
- [ ] `tools/release/boot_gate.py` green over this task's release files. Rail R23 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Anything else in the PSRAM: no stack, no buffer of the modem's, no `.bss`, no image, no log.
The 8 MB variant: it is named in the configuration and holds its sessions in the small room,
and nothing here is tuned for it. A room bigger than 64 kB. The program the server proposes
(meshmaster.dk's task 237). The board: no session opens a port, which is why the bench items
are Thomas's. A release: Thomas presses the button.
