# 04 — A batch keeps its id

**Depends on:** the agent library at a release that holds the exit-node firmware's task 55 (*a batch keeps its id over a sleep*), copied here by the bump `docs/upstream.md` describes. Not before.
**Spec:** `components/agent-core/MANIFEST.txt` (the pin), `components/agent-core/include/agent/KeptImage.h` and `AgentLoop.h` as that release has them, `components/agent/include/walter_agent/agent_backend.h` (`kKeptScratchBytes` ~L136 and the two `static_assert`s under it), `components/agent/agent_backend.cpp` (what a sleep keeps), `components/agent/include/walter_agent/observations.h`, `components/core/console/console.cpp` (`agent obs` ~L3530), the wake bench (`host_test/core/cycle/wake_bench.h`) and its scripted server, `docs/upstream.md`
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

A batch whose answer was lost must go out again under the id it was built with, so that the
server answers *had it already* and stores nothing twice. The library kept that id in RAM, and
this board's RAM does not survive a sleep. So at every wake the unit posted everything it still
held as a new batch, and meshmaster.dk stored it again: on 16 November batches of 13, 14, 15,
16, 17 and 18 adverts in six wakes running, and 19 different adverts in its 100 newest rows.

The library now keeps the pending batch's id in the image a sleep keeps (the exit-node
firmware's task 55). This task is what is left for this board once that library is here.

## Deliverables

1. **The room the image needs.** `kKeptScratchBytes` and the two assertions under it, held to
   the library's own figures as they are now. Say the new largest image in the report.
2. **Shown on this board's bench.** On the wake bench, with the real backend and the real
   library: a batch the server takes and whose answer is lost, a deep sleep, and the next wake.
3. **What a person reads.** `agent obs` says when a batch is waiting for its answer, how many
   adverts it holds and since which wake; the report's `observations` section with it.
4. **Room.** `tools/heap_at_start.py` over this task's build; the margin is not below main's.
5. **The words.** `docs/` where the observations are described. One line in
   `release-notes/unreleased/`.

## Acceptance criteria

- [ ] A batch of five is posted and its answer is lost; the board sleeps; the next wake's first observations post carries the same id and the same five (test).
- [ ] The server answers that post *had it already*: the five leave the ring, and what was heard since goes out as a second batch under a new id in the same wake (test).
- [ ] Nothing is stored twice by a server that de-duplicates on the id: its count of adverts is the count heard (test).
- [ ] An image written by the library as it was before the bump is read back at the first boot after an update, with no batch pending (test).
- [ ] `agent obs` names a waiting batch and prints nothing of it when none waits (test).
- [ ] The assertions hold the scratch to the library's largest image. The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The library: it is copied and not edited here; what it keeps and how is the exit-node
firmware's task 55. The rows meshmaster.dk already stored twice. The lost answer itself: tasks
02 and 03. The board: no session opens a port. A release: Thomas presses the button.
