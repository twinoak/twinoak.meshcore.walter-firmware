# 04 — The Walter's own update

**Depends on:** 02 (and the journal of 03).
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-6, S-7; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §7, §11; [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §5; ESP-IDF's documentation of over-the-air updates and application rollback, at the version this repository builds with (`$IDF_PATH/docs` in the image, or Espressif's site).
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

The Walter replaces its own firmware and **comes back if the new one is no
good** — where "no good" is not "it crashed" but "it cannot do its job": it
cannot talk to the radio, or it cannot reach the server. A unit on a mast with
a Walter that boots perfectly and has lost the ability to attach to the network
is as lost as one that does not boot; this task makes that image roll back by
itself.

## Read first

How ESP-IDF's bootloader chooses a slot, what *pending verify* means, when it
gives up on an image, what `esp_ota_mark_app_valid_cancel_rollback` and
`esp_ota_mark_app_invalid_rollback_and_reboot` do, and what happens to an image
that is neither confirmed nor rejected before the next reset. Phase 0's task 01
enabled rollback and said what its idle image does about confirmation; read
that report.

## Deliverables

- **`components/hal/`**: an interface over the platform's own-update
  machinery — which slot is running, is it pending verification, begin / write
  / end an image into the other slot, make it the next to boot, confirm the
  running image, reject it and restart — and its ESP-IDF implementation, thin.
- **`components/core/update/SelfUpdate`** — policy, over that interface, the
  store's verdict (task 01), the journal (task 03), the link client and an
  uplink interface (phase 3 fills it; here a fake):
  - **Accepting an image**: the Walter's own product, image `manager.walter.*`,
    the `ota` file — the same six checks, with the descriptor's role `manager`
    and board `walter` — streamed straight into the other slot. It must fit
    the slot and its `load_address` must be a slot's offset.
  - **Preconditions**, each a named refusal: the battery (an interrupted write
    is harmless, but a brown-out during the first boot is a wasted attempt);
    no radio update journalled as in progress (S-7); the running image is
    itself confirmed (never stack an update on an unconfirmed one).
  - **The link's major version**: an image whose link major differs from the
    radio's current one is allowed only if it speaks **both** — the descriptor
    says which it prefers; the manifest's `requires` says what it tolerates —
    because the Walter goes first (S-7). Decide how "speaks both" is expressed
    with the fields the specification has, and if it cannot be expressed, say
    so in the report as a specification gap rather than inventing a field.
  - **Switching**: journal *self-update: switching to <version>, from
    <version>*; set the boot slot; tell the radio (`SLEEP_NOTICE`); restart.
  - **Confirmation — the heart of the task.** An image that boots *pending
    verify* runs a **trial**: it must, within a **trial window** (a build
    constant; long enough for a slow LTE attach, short enough to matter),
    (1) reach its own main loop with the board in the safe state, (2) exchange
    `HELLO` with the radio — or establish that the radio is in a state the
    supervisor already knew about before the update (journalled), so that a
    dead radio does not condemn a good Walter image — and (3) complete one
    exchange with the backend over the uplink. All three: confirm, journal
    *confirmed*. The window passes, or the image restarts for any reason
    during the trial (the journal knows it was on trial): **reject and
    restart** into the previous image, journal *rolled back: <which of the
    three was missing>*, and put the image's hash on the bad list.
  - **A trial is one boot.** The second boot of an unconfirmed image is a
    rollback, whatever the reason for the reset.
  - **Until the backend exists** (phase 3 ends at a seam), condition (3) is an
    interface with two implementations: the fake in tests, and in the firmware
    one that a build constant sets to *reach the configured server's TLS port
    and complete a handshake* — the strongest statement available without an
    agent. Say so in `docs/update.md`.
- **The console**: `self status` (running slot and version, the other slot's,
  confirmed or on trial and for how long, the last self-update's story from
  the journal), `self confirm` and `self reject` for the bench, `self receive`
  (the same USB transfer as task 02, into the other slot, through the six
  checks) and `self switch`.
- **`docs/update.md`** gains the Walter's half, and the order of a
  link-version change.

## Tests (host)

- The good path: accept, switch, trial with all three conditions, confirmed.
- The trial fails on each condition alone; on a restart during the trial; on
  the window passing. Each: rejected, the previous image runs, the hash is on
  the bad list, and the same image is refused if offered again.
- A dead radio that was already known dead before the update does not fail the
  trial; a radio that was alive before and is silent after **does**.
- Preconditions: a radio update in progress; the running image unconfirmed;
  the battery.
- An image that is the radio's, offered as the Walter's (refused at check 3 or
  5); one too large for the slot; one whose `load_address` is not a slot's.
- The journal across the restart: the story reads correctly after a confirmed
  update and after each kind of rollback.
- Power loss while writing the other slot: the running image is untouched and
  the write can be repeated.

## Acceptance criteria

- [ ] `tools/verify.ps1` green; the firmware's own start-up now runs the trial logic (with the fake-free uplink condition) and the report says what a freshly flashed factory image does on its first boot — it must not reject itself for want of a server before it has ever been configured. State the rule that prevents that, and test it.
- [ ] Each rollback reason has a test and a distinct journal line.
- [ ] The trial window and the uplink condition are named constants, listed as "to be set on the bench".
- [ ] The question of expressing "speaks both link majors" is answered or raised as a specification gap.

## Not verified here (Thomas, with a Walter)

Add to `docs/bench.md`, "8. Updating the Walter":

- Push this firmware's own next version with `self receive`, `self switch`:
  it restarts, runs its trial, and `self status` says *confirmed*.
- Push a version built to fail its trial (a build flag that skips the radio's
  `HELLO`): after the window it restarts into the old one; `self status` says
  why.
- Reset the Walter during a trial: it comes up in the old image.
- Pull power while `self receive` is writing: the running image is unaffected.

## Out of scope

Downloading (phase 3). Deciding when (the wake cycle). Updating the modem's
own firmware — the library can; nothing here does.
