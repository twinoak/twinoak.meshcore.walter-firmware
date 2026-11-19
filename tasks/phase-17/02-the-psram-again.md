# 02 — The PSRAM, again

**Depends on:** 01
**Spec:** `tasks/phase-16/05-the-room-is-in-the-psram.md` (the whole task: its deliverables and its criteria are this task's) and `tasks/phase-16/reports/05.md` (the whole report: *What it costs, in figures*; *What is on the branch, and how to bring it back*; *Deviations*; *Not verified here*); the two commits on `main`: `8b1b68b` *p16-05: the room of a session out of the PSRAM* and `b5a1c94` *p16-05: revert the implementation*; task 01's report (what the start has to spare now); `tools/heap_at_start.py` and rail R22, `tools/check_rails.py` (R24 came with the implementation), `tools/release/boot_gate.py` and rail R23; `sdkconfig.defaults`; `docs/ARCHITECTURE.md` §14, `docs/update.md`, `docs/console.md`, `docs/bench.md` §1; `tasks/phase-16/FOLLOWUPS.md` (the items on the PSRAM) and `tasks/phase-16/reports/06.md` (what landed after the revert).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Pile 16's task 05 wrote the room of a session out of the PSRAM, tested it, booted it in QEMU
where there is no PSRAM, and then took it out again in its second commit: the PSRAM costs
5 904 bytes of static memory and the start had 644 to spare. Its report says how to bring the
work back if the bytes are found.

Task 01 found them. This task brings the implementation back, measures the start again, and
writes the words task 05 left unwritten because they would have described code that was not
there.

## Deliverables

1. **The implementation is back**: the revert of `b5a1c94`, on top of what has landed since.
   Where task 06 of pile 16 or task 01 here touched the same lines, the report says what was
   put together by hand.
2. **Every deliverable and every criterion of pile 16's task 05 holds**, with its own tests,
   as that report ticked them *done and green, not merged*. They are ticked again here from
   this task's own run.
3. **The start is measured again, and decides.** `tools/heap_at_start.py` over this task's
   build and over `main`'s as task 01 left it: the main task's stack *FITS*, in the heap it
   is in on `main` and with no less to spare, and the tightest block before `app_main` has
   **at least the 644 bytes** it had before this pile. The report sets the figures side by
   side.
   If it does not hold, the firmware is left as task 01 left it: none of this task's code is
   merged, the report says the figures and what is still missing, and the report alone is
   merged.
4. **The release still boots where there is no PSRAM**: `tools/release/boot_gate.py` over
   this task's release files (R23).
5. **The words task 05 did not write**: `docs/ARCHITECTURE.md` §14, a paragraph: what the
   PSRAM is used for here and what it is never used for. `docs/update.md` where the room is
   described. `docs/console.md`: `heap`'s line and `heap room`. `docs/bench.md`, the ⚠ items
   of task 05's deliverable 10. `sdkconfig.defaults` as the commit has it, each line with
   its reason. One line in `release-notes/unreleased/`.
6. **Pile 16's follow-ups** on the small room say what is now true; nothing in them is
   ticked.

## Acceptance criteria

- [ ] The criteria of `tasks/phase-16/05-the-room-is-in-the-psram.md`, each ticked from this task's run with the test that holds it.
- [ ] `tools/heap_at_start.py` says *FITS*, the stack is in the heap it is in on `main` with no less to spare, and the tightest block before `app_main` has 644 bytes or more to spare; the three figures stand beside `main`'s in the report. Rail R22 green.
- [ ] Rails R23 and R24 green.
- [ ] A wake that runs no session never brings the PSRAM up, and a board scripted with none holds its sessions in the 8 192 bytes (the tests of the commit).
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Anything else in the PSRAM. A room bigger than 64 kB. The board: no test here can bring a
PSRAM up, so the ⚠ items of `docs/bench.md` are Thomas's, on a Walter alone and on one with a
radio, before any release. The program meshmaster.dk proposes. A release.
