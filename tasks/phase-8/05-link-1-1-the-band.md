# 05 — Link 1.1: the band

**Depends on:** 04 (both change what a wake knows about its radio; 04 first keeps the two PRs apart).
**Spec:** `spec/LINK-PROTOCOL.md` **1.1** (the status lines at its top; §5.1, the `band` row and *The band, and why it is here*; *Versions*), `spec/fixtures/link/link-v1.1.json` (every case of 1.0 and the four of 1.1), `spec/fixtures/link/link-v1.json` (1.0's, unchanged), `spec/tools/linkref.py`; `spec/FIRMWARE-MANIFEST.md` (the descriptor's `link_major`/`link_minor`, and what a release's `link` must agree on between the Walter and a radio image); `components/core/link/codec/` (`HELLO`'s body, `EV_BOOT`), `components/core/link/client/` (`Session`: role, board, fw_version, boot_id), `components/core/include/core/image/descriptor.h` and wherever the build sets this firmware's own `link_minor`; `tools/release/build_release.py` (the manifest's `link`); `components/core/console/console.cpp` (`rak hello`, `status`); `components/agent/firmware_jobs.cpp` (`FirmwareJobs::writeStateResult`: the golden image's descriptor, else `HELLO`'s role and board, and never a band); meshmaster.dk's `src/MeshMaster.Core/Firmware/FirmwareRollout.cs` (`PickImage`: role, board **and** band, read-only from the sibling repository); `../phase-6/FOLLOWUPS.md`, *A unit with no golden image cannot be offered a radio release over LTE*.
**Size:** one session.
**Model:** claude-opus-5

## Goal

On 6 November job 20990, *read the unit's state*, came back as `{"slot": 0}` and nothing else. The
Walter had no golden image, so it fell back on `HELLO`, and `HELLO` has no band. meshmaster.dk picks a
release's image by role, board **and** band, so its canary page said *Read the unit's state first*
forever. A radio that has never had a first install through its Walter could not be offered one over
LTE.

The reviewer changed the specification that evening: link **1.1** ends `HELLO`'s answer and `EV_BOOT`
with `band`, the band the running image was built for. The repeater firmware's pile 3.8 makes the
radio say it. This task makes the Walter ask in 1.1, keep the band, and report it.

## Deliverables

1. **The codec speaks 1.1.**
   - A `HELLO` request asks for minor 1.
   - An answer, or an `EV_BOOT`, whose body says minor 1 or more has `band` at its end. A body that
     says 0 has none, and trailing bytes beyond what this firmware knows are ignored (*Versions*).
   - Every case of `link-v1.1.json` passes, as every case of `link-v1.json` still does. Add the new
     file to the existing fixture tests rather than replacing the old one.
2. **The session keeps it.** `Session` gains the band, empty when the radio speaks 1.0.
   - `rak hello` prints `band 868`, or `band: not said (a 1.0 radio)`.
   - `status` shows it where it shows the board.
3. **The state report.** With no golden image, `twinoak.fw.state` reports the slot's role, board
   **and** band from `HELLO` when all three are known. With a 1.0 radio it reports role and board,
   as now. With a golden image, the descriptor stays the source. Use the key the server reads
   (`band`, per `FirmwareRollout`), and test all three cases.
4. **This firmware's own link version.**
   - Its descriptor's `link_minor` and its release manifest's `link` say 1.1.
   - Read `FIRMWARE-MANIFEST.md` for what must agree between a Walter's link and a radio image's, and
     make sure a 1.1 Walter still accepts, flashes and talks to a 1.0 repeater image: the unit on the
     bench runs repeater v0.1.0.
   - Test it with the fixture *hello response from a 1.0 radio to a 1.1 request*.
5. **Docs.**
   - `docs/console.md` (`rak hello`, `status`) and `docs/link.md` (or the doc that describes the link
     client).
   - `docs/bench.md`, for Thomas:
     - with repeater v0.1.0 on the radio, `rak hello` says `band: not said`;
     - with repeater v0.1.1 (pile 3.8), it says `868`, and *read the unit's state* on meshmaster.dk
       reports the band without a golden image.

## Acceptance criteria

- [ ] 1.1's codec: every case of both fixture files passes; the band is read only from a 1.1 body.
- [ ] `rak hello` and `status` print it; `twinoak.fw.state` reports it with no golden image, tested for a 1.1 radio, a 1.0 radio and a golden image.
- [ ] The descriptor and the manifest say link 1.1, and a 1.0 radio image is still accepted (test).
- [ ] Docs updated; the `../phase-6/FOLLOWUPS.md` item ticked.
- [ ] `tools/verify.ps1` green.

## Out of scope

The radio's side (repeater firmware pile 3.8). meshmaster.dk, which already reads `band`.
