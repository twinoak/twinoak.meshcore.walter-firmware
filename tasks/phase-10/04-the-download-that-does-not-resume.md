# 04 — The download that does not resume

**Depends on:** nothing in this phase.
**Spec:** `tasks/phase-9/FOLLOWUPS.md`, *The Walter's own download does not resume, and the message says it does* (found while doing 07, with the code it read); `components/core/update/fetch.cpp` (`describe(FetchVerdict::kBudget)`), `components/agent/firmware_jobs.cpp` (`runOffer`'s budget branch, its log line and the `reason` it posts), `components/core/update/self_update.cpp` (`beginWrite`, `abandonWrite`); `docs/bench.md` §18 (the figure Thomas writes down for the 0.1.1 → 0.1.2 download over LTE, if it is there when this runs).
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

A radio image's download resumes at the next wake. The Walter's own does not: its slot is erased and
written from zero. Yet two messages, one of them posted to meshmaster.dk, promise *the next wake
resumes it*.

## Deliverables

1. **The two sentences** say what happens for the Walter's own image (*the download starts again
   next wake*), and keep saying *resumes* for a radio image. The branch already knows which it is in.
2. **A test** for each sentence, for both products.
3. **The figure.** If `docs/bench.md` §18 has Thomas's time for the 0.1.1 → 0.1.2 download, set it
   against the wake's budget (`kWakeDownloadBytes`, `kBackendBoundMs`) in the report, and say whether
   a resume is worth building (`esp_ota_resume`, the journal carrying the bytes written). If it is not
   there, say so. Either way, build no resume here.
4. `tasks/phase-9/FOLLOWUPS.md`'s item ticked, with what is left of it (the design question).

## Acceptance criteria

- [ ] Neither message promises a resume for the Walter's own image; both tested.
- [ ] The report sets the bench's figure (or its absence) against the budget; `tools/verify.ps1` green.

## Out of scope

Making the Walter's own download resume.
