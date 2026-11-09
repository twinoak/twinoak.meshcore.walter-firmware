# 02 — A ceiling on the heap

**Depends on:** 01 (its model).
**Spec:** 01's report and `tools/heap_at_start.py`; `tools/verify.sh` (the map rails ~L117–130 and ~L238–259: the pattern a build-time rail follows here), `tools/check_rails.py` (the index; R6 is the rail that *needs the built image*), `.github/workflows/ci.yml` and `release.yml` (where `verify` runs); `main/main.cpp` (`app_main`'s first lines); `components/core/console/commands.cpp` and `console.cpp` (a command's table entry, its help, its tests); `docs/bench.md` §1.
**Size:** one session.
**Model:** claude-opus-5

## Goal

0.1.3 passed every check this repository has. The image fitted its slot, the linker's DIRAM fitted
its segment, and the host tests do not run ESP-IDF's start-up. Nothing looked at the one figure
that decided it: the block the main task gets. This task makes the build look at it every time,
and lets the board say what it got, so the model can be checked against a real start.

## Deliverables

1. **A rail.** `tools/verify.sh` runs `tools/heap_at_start.py` on every build it makes. Below the
   margin it fails, and prints the regions, the allocations and the block the main task got.
   - The margin is one constant with a sentence that says where it came from (01: 8 KB).
   - `tools/check_rails.py` indexes it the way it indexes R6.
   - It runs in CI and in the release build, so no image that would not start can be published.
2. **What the board got.**
   - The first lines of `app_main` print one `heap:` line: free and largest free block of the
     internal heap, as `app_main` finds it.
   - A console `heap` prints the same, plus the minimum free since boot and the main task's stack
     high-water mark.
   - The boot line is the bench's check on the model: its free figure against what the model
     predicts after the main task.
3. **The bench.**
   - `docs/bench.md` §1 gets an item: the `heap:` line at the first boot, and what the model says
     it should be.
   - §16 or §18 (*a day*) gets the `heap` high-water after a day. That figure is what could one day
     let the 16 KB come down.

## Acceptance criteria

- [ ] The rail fails on `v0.1.3` rebuilt at its tag and passes on `v0.1.2` and on this task's build (shown in the report, with the output).
- [ ] The rail is in `check_rails.py`'s index, and deleting it fails `check_rails.py` (the existing pattern).
- [ ] `heap` has host tests: the line's shape and the help entry.
- [ ] `tools/verify` and the host tests green.
