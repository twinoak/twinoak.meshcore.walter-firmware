# 03 — A silent radio is not a scanner

**Depends on:** nothing in this phase.
**Spec:** `../phase-8/FOLLOWUPS.md`, the review's S1; `tasks/phase-8/04-the-radio-known-before-a-job.md` and its report; `components/agent/agent_backend.cpp` (about line 223: `radio_silent = !radio_up || …`), `components/core/include/core/supervisor/supervisor.h` (about lines 345–347: why a slot running NoiseScope, or awaiting its first install, is not *link known down*), `components/core/update/radio_update.cpp` (about lines 458 and 477: the link-down and battery-unknown refusals of an apply), `components/core/update/self_update.cpp` (about lines 218 and 464–467: the radio's condition in a self-switch's confirmation); `docs/update.md`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

p8-04 made a wake claim only the unit's own job types when it cannot read the radio's key. To do that
it set `radio_silent` whenever there is no frame session. A slot running NoiseScope, or one awaiting
its first install, has no frame session either, and the supervisor rules on purpose that such a slot
is not *link known down*. p8-04's flag now says it is, and that has two effects:

- a `twinoak.fw.apply` loses its link-down and battery-unknown refusals;
- a self-switch's confirmation marks the radio's condition *not applicable*.

## Deliverables

1. **Two different facts, kept apart.**
   - *No key to tell a local job by* is what the claim needs.
   - *The link is known down* is the supervisor's, and the refusals read only that.
2. **Tests.** Each of a scanner slot, a slot awaiting its first install, a dead radio and a live one:
   - what the claim takes;
   - whether an apply refuses on the link and on the battery;
   - what a self-switch's confirmation says of the radio.

   The scanner and awaiting cases fail before the fix.
3. **`docs/update.md`**, if it describes either.

## Acceptance criteria

- [ ] The claim's rule and the supervisor's *known down* are separate; the apply's refusals and the self-switch's confirmation read the supervisor's.
- [ ] Tests for the four slot states; the S1 item ticked; `tools/verify.ps1` green.

## Out of scope

Anything else p8-04 did, which the review found right.
