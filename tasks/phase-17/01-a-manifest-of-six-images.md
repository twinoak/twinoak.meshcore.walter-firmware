# 01 — A manifest of six images

**Depends on:** —
**Spec:** `components/core/include/core/image/manifest.h` (~L26–41: the three bounds, each with its reasoning; ~L119 and ~L137: the two arrays of a parsed manifest; ~L145–148: the three refusals; ~L161–170: the token table) and `components/core/image/manifest.cpp` (~L19–27: where the token bound comes from, and its `static_assert`; ~L324 and ~L406; ~L451; ~L472); `components/core/third_party/jsmn/README.md`; `components/core/include/core/image/receive.h` (~L92: `kMaxManifestBytes`); `host_test/core/image/test_manifest.cpp` (~L326–360) and its fixtures; `spec/FIRMWARE-MANIFEST.md` §2 (the seven `use` values, and which board has which) and §5 (check 2); `main/main.cpp` (~L147–151 and ~L436: the verdict is a static of `app_main`, *~30 KB of parser tokens and parsed manifest*); `docs/ARCHITECTURE.md` §14 (*The heap at start*), `tools/heap_at_start.py` and rail R22; `tasks/phase-16/reports/05.md` (*What it costs, in figures*) and `tasks/phase-16/reports/04.md` (the 644). In the repeater firmware's repository, read only: a release's `manifest.json` as its tooling writes one.
**Size:** one session, a short one.
**Model:** claude-opus-5

## Goal

Task 05 of pile 16 put a session's room into the PSRAM, and did not merge it. Switching the
PSRAM on costs 5 904 bytes of static memory. Before `app_main` three blocks have to be found in
two heaps, and on `main` the second of them fits with **644 bytes** to spare. With 5 904 bytes
less, it does not, and the board would not start. The report's number to beat is 5 256 bytes,
and it says of our own static storage: *no single object of ours is that big*.

One is. The update verdict is 35 472 bytes of `.bss`, and 20 480 of them are the manifest
parser's token table: 1 024 tokens of 20 bytes. It is sized for a manifest filled to both of
its bounds at once, eight images of seven files each, 957 tokens.

No release is that. The Walter's own manifest is 64 tokens. The repeater's is 167: two images
of four files. An image cannot have seven files, because no board has all seven uses: `ota`
and `factory` are the Walter's, and `swd`, `usb-dfu`, `usb-uf2` and `provision` the radio's.
And a manifest may be 8 192 bytes, which a release of eight such images is not.

This task sizes the table for the releases there can be, and gives the memory back.

## Deliverables

1. **The bounds.**

   | | was | is | because |
   |---|---|---|---|
   | `kMaxImages` | 8 | **6** | three roles on the radio in two bands |
   | `kMaxFilesPerImage` | 7 | **5** | the most uses one image can have: `swd`, `usb-dfu`, `usb-uf2`, `debug` and `provision` on the radio; `ota`, `factory` and `debug` on the Walter |
   | `kMaxJsonTokens` | 1 024 | **640** | a manifest filled to those two is 569 tokens, by the count `manifest.cpp` already makes; the rest is room for fields the schema does not know, which are ignored and still counted |

   The reasoning beside each constant is rewritten to say this, and the `static_assert` that
   ties the third to the other two stays.
2. **Nothing else of a manifest changes.** What is read, what is refused and in which words:
   a release beyond a bound is still *a release this firmware cannot read*, said at check 2
   with the bound it broke.
3. **Held against the releases there are.** A test parses a manifest shaped as the repeater's
   release writes one, two images of four files, and one shaped as the Walter's own, and a
   manifest filled to the two bounds. The fixture that is one image too many is made for the
   new bound.
4. **What it gave back, measured.** `tools/heap_at_start.py` over this task's build and over
   `main`'s, both built in the same container: `_heap_start`, the `.bss` of the verdict, and
   the spare bytes of the tightest block before `app_main`, which were 644. The report sets
   the figures side by side.
5. **The words.** `docs/ARCHITECTURE.md` §14: the figure it gives for the tightest block, and
   one sentence on what a bound of the manifest costs in static memory. `docs/update.md`
   where the checks are described, if it names a bound. One line in
   `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The three constants are 6, 5 and 640, and the `static_assert` holds (the build).
- [ ] A manifest of two images of four files (`swd`, `usb-dfu`, `usb-uf2`, `provision`), and one of one image with `ota` and `factory`, parse and validate (tests).
- [ ] A manifest of six images of five files parses; of seven images: *too many images*; with a sixth file in one image: *too many files* (tests).
- [ ] A manifest with a member of 640 more tokens than it needs: *too large: more JSON tokens than this firmware holds* (test).
- [ ] Every other test of `host_test/core/image/` passes unchanged but for the fixtures of the two bounds.
- [ ] `tools/heap_at_start.py` says *FITS*; the tightest block before `app_main` has at least 7 680 bytes more to spare than on `main`; the report sets the figures beside `main`'s. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The PSRAM (02). The parser itself. The two places that hold a manifest's bytes, and
`kMaxManifestBytes` (`FOLLOWUPS.md`). The modem library's static storage (`FOLLOWUPS.md`).
meshmaster.dk's catalogue, which has no such bounds. A release: Thomas presses the button.
