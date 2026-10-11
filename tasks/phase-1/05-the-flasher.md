# 05 — The flasher

**Depends on:** 04.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §5, §2; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5, §10 item 3; [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §2 (`app_region`); the repeater firmware's `docs/ARCHITECTURE.md` §9 (the flash map from the other side); the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `PROJECT-KNOWLEDGE.md` §6 ("Never chip-erase the RAK in service"), `BENCH-BREADBOARD.md` §6 caveats 4–6.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

Give it an image and the Walter puts that image into the radio's application
region — and into nothing else — checks every byte, leaves the bootloader
willing to start it, and restarts the radio. Interrupt it anywhere, run it
again, and it finishes. This is the operation the whole unit was designed
around; it is developed against the simulated chip, with the power cut at every
step.

## Read first

- **The bootloader the RAK4630 ships with** — Adafruit's nRF52 bootloader
  (`adafruit/Adafruit_nRF52_Bootloader`) and the OTAFIX variant MeshCore
  recommends (`oltaco/Adafruit_nRF52_Bootloader_OTAFIX`): `src/main.c` (how it
  decides between the application and DFU mode: the double-reset word, the
  `GPREGRET` magic values, the DFU button), `bootloader_app_is_valid` in
  `lib/sdk11/components/libraries/bootloader_dfu/bootloader.c`, the settings
  structure and its address in `dfu_types.h` / the linker scripts, and where
  the bootloader itself and the MBR parameter page live. As read on
  2026-10-11: with a *valid application* recorded, it computes a CRC over the
  recorded size and compares it with the recorded CRC unless that is 0; with
  the settings page **erased** (`bank_0 == 0xFFFF`) it treats the application
  as "flashed by a debugger" and checks the vector table instead. **Confirm
  both against the source at a named commit, and against the OTAFIX variant,
  before relying on either.**
- MeshCore's linker scripts for the RAK4631 at the pin
  (`d92964352441e53b93e8667b802e04f6e072b39e`, `boards/`): where the
  application starts, where it may end, and where the file system begins —
  with and without `EXTRAFS`.

## Deliverables

- **Board profiles** (`components/core/flasher/BoardProfile`): for
  `rak4630-single`, the application region (start = the end of the SoftDevice;
  end = the start of the file system, **the more conservative of the linker
  scripts in use**), the page size, the expected part, and the bootloader
  settings page's address. Constants with the file and line of upstream they
  were read from. This table is the authority (ARCHITECTURE §5): a manifest's
  `app_region` must lie inside it, and is refused if it does not.
- **`components/core/flasher/Flasher`**: `flash(image, load_address, profile,
  progress)`, returning a result that names the step that failed.
  1. Acquire the pins; connect; identify — refuse a wrong part; refuse a
     protected chip, saying so.
  2. Refuse an image that is empty, not word-aligned in length after padding,
     or not wholly inside the profile's region. **Nothing has been written
     yet.**
  3. Reset-and-halt the core.
  4. **Make the old application unstartable first.** If the image's first
     page (the vector table) differs from what is there, erase that page
     before anything else and write it **last**. The bootloader refuses an
     application whose first word is blank whatever its settings say, so from
     this moment until the last write the radio sits in its bootloader — never
     in half of one firmware and half of another.
  5. Page by page over the remaining pages the image covers: read the page; if
     it already equals the image's bytes (padded with `0xFF`), skip it; if it
     can be reached by clearing bits only, write without erasing; otherwise
     erase and write. Then read it back and compare. A mismatch is retried
     once (erase, write, verify) and then fails. Then the first page.
  6. Read the whole image back and compare its SHA-256 with the one given.
     **The bootloader's settings page**: bring it to the state the bootloader
     accepts for an application it did not install — decided from the source
     read above, implemented behind the profile, with the reasoning in a
     comment. This is the **one** page outside the application region the
     flasher may touch, by a dedicated function that can touch only that
     address. Whether it is done before step 4 or here, and why that order is
     the safe one given step 4, is part of the decision — write it down.
  7. System reset through the debug port, release the pins to the safe state,
     and wait at least a second before anybody may reset the radio again
     (`RadioPins`' own rule).
  It never erases a page the image does not cover. It reports progress in
  pages so a caller can extend a lease or feed a watchdog; it feeds none
  itself.
- **Convergence.** `flash()` run again after any interruption completes the
  job with no more erases than needed. Document the state the radio is in
  after an interruption at each step: which firmware (none) runs, what the
  bootloader does, that the identity and settings are untouched.
- **`verify(image, load_address, profile)`** on its own: read back and compare,
  changing nothing — used by the supervisor before it trusts a golden image is
  really what is on the chip.
- **The console**: `rak flash <slot-name>` is phase 2's (there is no image
  store yet). Here: `rak flash-test` writes a 2-page test pattern at the *top*
  of the application region's last two pages **only if those pages are blank**,
  verifies it, erases it again, and leaves the application alone — a bench
  command to prove erase/write/verify on real silicon without risking the
  running firmware. It refuses if the pages are not blank.

## Tests (host, against the simulated chip)

- A full image into a blank region; into a region holding another image; the
  same image again (**zero** erases, zero writes — count them in the
  simulator's log).
- **Power loss at every flash operation in turn** (a loop over N): after each,
  the simulator's chip is in a legal half-way state; `flash()` again succeeds;
  the final contents are exact; **no page outside the image's pages and the
  settings page was ever erased or written**, in any run. This one test is the
  reason the simulator exists.
- **Never half an application.** Implement the bootloader's validity rule in
  the simulator, from its source, and evaluate it after every interrupted run
  of the power-loss loop: from the first flash operation to the last, it
  refuses the application; only after a completed flash does it accept it —
  for a settings page that started as "installed by DFU, CRC recorded" and for
  one that started erased.
- An image one page too long for the region; one that starts below it; an
  image whose manifest region is wider than the profile's: refused with the
  log empty.
- A protected chip; a wrong part; a target that goes silent mid-flash (pins
  released, a clear error).
- A flash page that will not take a write (a stuck bit in the simulator):
  retried once, then a failure that names the address.
- `verify()` changes nothing: the log is empty.
- The pin rules hold throughout, on every exit path.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The power-loss loop covers every flash operation of a multi-page image and the report gives the count.
- [ ] The power-loss loop also proves the simulated bootloader never accepts a partly written application, from either starting state of the settings page.
- [ ] The report states, with the bootloader's source lines and commit, what the flasher does to the settings page and why the bootloader then starts the application — for the stock bootloader and for OTAFIX — or that the two differ and how the profile tells them apart.
- [ ] The region constants cite MeshCore's linker script lines at the pin.
- [ ] The report gives the expected duration of a full flash at 100 kHz (transfers per word, words per image, erase time), since a caller has to budget for it.

## Not verified here (Thomas, with the rig) — the most important bench session of the project

Add to `docs/bench.md`, "5. Flashing":

1. `rak id` first; note the variant; stop if *protected*.
2. Note the RAK's ID from its USB console (`get public.key`) and one setting
   you can recognise (`get name`).
3. `rak flash-test`: passes; the RAK still boots what it had.
4. Flash a repeater image (phase 2's `rak flash`, or load one over USB): the
   RAK restarts into it — **not** into its UF2 bootloader — `rak hello` shows
   the version flashed, and its ID and name are what they were in step 2.
5. Pull the Walter's power half-way through a flash. The RAK stays in its
   bootloader (a UF2 drive on its USB). Flash again: it completes and boots.
6. Double-tap the RAK's reset afterwards: the UF2 bootloader still works, and
   a UF2 of the stock firmware still installs — the bootloader was not harmed.
7. Time a full flash; find the fastest SWD speed that still verifies.

## Out of scope

Where an image comes from, whether it is signed, the health check and the
rollback (phase 2).
