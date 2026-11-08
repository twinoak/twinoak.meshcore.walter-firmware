# 03 — A radio without its identity

**Depends on:** nothing in this phase.
**Spec:** read only, the repeater firmware: `tasks/phase-3.9/reports/01.md` (*The cause, from the code*; *The fix*; *Follow-up for the Walter firmware, in its `FOLLOWUPS.md` form*) and `tasks/phase-3.9/reports/03.md` (*Follow-up correction*), its `docs/flashing.md` §3a and `docs/link.md` (the lost mode: `HELLO` with `caps 0x21`, `IDENTITY` answered `ERR_STATE`, `EV_ATTENTION` reason 6 detail 1, a `CLI` that knows `erase` and `reboot`). This firmware: `components/agent/identity.cpp` (`readKey`, which treats every answer that is not ok alike; `agentId()`'s branch *the radio has no identity yet*; the heartbeat's `errors`, filled here), `components/core/update/radio_update.cpp` and its header (the health check, *proving*, and what it reads from `HELLO` and `IDENTITY`), the supervisor's classification of the radio (`walter_agent/firmware_jobs.h`, `radioHealth()`), `tasks/phase-8/README.md` and `tasks/phase-8/01-the-flasher-lets-the-idle-task-run.md` (*its key kept*, *its key survived*), `docs/flashing.md`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The first radio lost its key on 6 November: the Adafruit core formats a file system it cannot mount,
and upstream then makes a new key without a word. From repeater v0.1.2 a radio in that state refuses
instead. It keeps the link up, answers `IDENTITY` with `ERR_STATE`, raises attention 6/1, and waits
for a person to type `erase` and `reboot`.

This firmware cannot tell that refusal from a radio that has never had a key. Both arrive as *no
identity*, and the two need opposite things:

- a radio awaiting its first install needs its install;
- a radio that lost its identity needs a person. It must not have an update's rollback spent on a
  file system that no image can mount.

## Deliverables

1. **Three states, not two.** Where this firmware reads the radio's identity: known; *not made yet*
   (an all-zero key, no answer, a radio awaiting its first install); and **lost** (`IDENTITY` answered
   `ERR_STATE`, with attention 6/1 or `caps 0x21` as the evidence). Deterministic, from those
   answers, in one place.
2. **`agent id`** says *the radio refuses: its identity is lost* with the evidence and what a person
   does (`rak cli erase`, then `rak cli reboot`, after reading `docs/flashing.md` §3a), never the
   first-install sentence.
3. **meshmaster.dk is told** without a new field: the heartbeat's `errors` is an open key space on
   both ends, so a counter (`radioIdentityLost: 1`, say) is enough. Read how meshmaster.dk shows the
   counters and say in the report what its page will show. If the library's own `errors` overwrites
   the board's, find out and say.
4. **The update's health check** treats a lost identity after a flash as *the image is fine, the node
   is not*. It does not spend a rollback, it does not put the image on the bad list, and it journals
   the reason. Read the machine before changing it, and test both halves: a candidate that answers
   `HELLO` and refuses `IDENTITY` this way is not a failed image; a candidate that does not answer
   `HELLO` still is.
5. **The pages that say the key was kept**: `tasks/phase-8/README.md`, the p8-01 task file and
   `docs/flashing.md`, corrected with a dated line pointing at the repeater's report. Leave
   `release-notes/v0.1.1.md` as published, because it is the record. v0.1.3's notes (05) say it.
6. **Tests** against the fake radio, in that lost mode.

## Acceptance criteria

- [ ] Known, not made yet and lost are told apart in one place; `agent id` and the heartbeat's counters say *lost* (tests).
- [ ] A lost identity after a flash spends no rollback and bans no image; a radio that does not answer still does (tests).
- [ ] The pages corrected; `tools/verify.ps1` green.

## Out of scope

The radio's lost mode itself (the repeater firmware's). A way for meshmaster.dk to `erase` a radio
remotely: that is a decision of Thomas's, not a task yet.
