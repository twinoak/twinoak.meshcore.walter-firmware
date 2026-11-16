# 01 — A voided slot is no download

**Depends on:** —
**Spec:** `components/core/image/store.cpp` (`read` ~L382, `readProgress` ~L363, `promote` ~L699, `repair` ~L268, `invalidate` ~L454, `erase` ~L756 and its comment on the header sector), `components/core/include/core/image/store.h` (`Record`: *When !present: a transfer that was interrupted*), `components/core/cycle/wake_cycle.cpp` (the shortened sleep ~L1222; what the backend is told of a transfer ~L1466), `components/core/include/core/cycle/wake_cycle.h` (`kDownloadMinutes`), `components/core/update/radio_update.cpp` (`stepCommitting` ~L773), `components/core/update/fetch.cpp` (the resume), `components/interfaces/include/interfaces/backend.h` (~L123), `host_test/core/image/store_bench.h`, `host_test/core/cycle/test_wake_radio.cpp` (*a download half done*)
**Size:** one session.
**Model:** claude-opus-5

## Goal

The unit is set to check in every 15 minutes and wakes every 5 minutes 40 seconds: 140 timer
wakes in the 13 hours after its update on 15 November. Every wake its journal holds says *next
5 min … the sleep was shortened*.

A sleep is shortened for one reason only: a download is half done and the band is `plenty`.
No download is half done. meshmaster.dk shows the three it was offered finished whole, and
nothing downloaded on the 16th.

What the code says, which nobody has confirmed at the unit's console:

- A radio update that commits promotes the candidate to golden. The area that held the old
  golden image becomes the candidate area, and its record is made void: `invalidate` writes
  zeros over the record's magic. The sector is not erased.
- `read` finds no record there and asks `readProgress`. The markers that image's own download
  left in that header sector are still in it, and the newest of them stands below the image's
  size.
- So the candidate slot reads as a transfer interrupted at X of Y, for as long as nothing new is
  downloaded into it, and the wake cycle shortens every sleep.

If that is right it began with the first radio update that committed, on 13 November, and it has
tripled the unit's wakes since.

## Deliverables

1. **A slot a promotion made void holds no transfer.** `read` answers *not present* and *no
   transfer* for it, whatever its header sector still holds. The store can tell a record that was
   voided from a place no record was ever written to; say in the report which fact in flash the
   rule stands on. The unit in service has this state in its flash today, and the image that
   carries this task must cure it with nobody at a console.
2. **A download that was interrupted is still one.** It reads as a transfer at X of Y, the next
   wake resumes it from X, and in `plenty` the sleep before that wake is shortened. As today.
3. **A promotion leaves nothing behind to misread.** Whether the old markers go at the promotion
   or are only never read again is the session's to choose and to say. A reset in the middle of
   a promotion ends as it ends today.
4. **Everyone who reads it.** The sleep rule, what `twinoak.fw.state` and the heartbeat's
   knowledge say of the candidate slot, and `image` on the console: each says *empty* for a
   voided slot.
5. **Room.** `tools/heap_at_start.py` over this task's build; the margin is not below main's.
6. **The words.** `docs/` where the store and the shortened sleep are described. One line in
   `release-notes/unreleased/`.

## Acceptance criteria

- [ ] A candidate stored through a download that wrote its markers, then promoted: `read` of the candidate slot answers not present and no transfer (test).
- [ ] A store whose candidate area is as the unit's is today, markers under a voided record: after `begin()`, no transfer (test).
- [ ] A download interrupted at X of Y: a transfer at X of Y, resumed from X (the tests it has today, unchanged).
- [ ] A wake after a committed radio update, in `plenty`: the sleep is the band's own interval and the pass is not marked shortened (test on the wake bench).
- [ ] A wake with a download half done: shortened in `plenty` and in no other band (the tests it has today, unchanged).
- [ ] A reset between the index write and the voiding of a promotion: the tests it has today, unchanged.
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The five minutes themselves, and the bands. What meshmaster.dk shows as the unit's golden image:
its own pile 4.39. The lost answer of the observations call: tasks 02 and 03. The board: no
session opens a port. A release: Thomas presses the button.
