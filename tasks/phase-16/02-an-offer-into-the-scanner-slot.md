# 02 — An offer into the scanner slot

**Depends on:** —
**Spec:** meshmaster.dk's `docs/dev/unit-protocol.md` §4 and §5 (`twinoak.fw.offer`) and its pile 4.42, task 235 (the same contract as below); `components/agent/include/walter_agent/firmware_jobs.h` (the offer, ~L12–27), `components/agent/firmware_jobs.cpp` (`runOffer` ~L370: the payload's members ~L379–383, the store's read ~L443, the fetch ~L454; `writeOfferResult` ~L549; `writeStateResult` ~L1200); `components/core/include/core/update/fetch.h`, `components/core/image/receive.cpp` (~L14: the role follows the image), `components/core/include/core/image/store.h` (`Which` ~L109; a download half done, ~L65 and ~L279), `components/core/cycle/wake_cycle.cpp` (~L1201: `into_scanner`; ~L1281: the sleep shortened for a download half done); the console's `fetch [base-url] [candidate|scanner]` (`docs/console.md` ~L157); `host_test/agent-core/tests/fixtures/contracts/FirmwareOfferPayload.json` (the copied tree: not edited) and `host_test/fixtures/contracts-ahead/README.md` (where a shape the server does not have yet waits).
**Size:** one session.
**Model:** claude-opus-5

## Goal

The console's `fetch scanner` puts a release's scanner image into the scanner slot. The
server's offer cannot: `twinoak.fw.offer` always fills the candidate. A unit on a mast has no
console, so it holds no scanner image and cannot be given one.

## The contract

```
Payload   FirmwareOfferPayloadScanner.json: FirmwareOfferPayload.json with one member more, at its end
{"releaseId":7,"product":"twinoak-repeater-firmware","version":"0.3.0+abc1234","slot":0,"base":"/agent/v1/firmware/7","expectedSha256":"662a969ee40557c75cdb9dec10d91477a848282914cdae9a740f08d1a4372b0a","size":4096,"into":"scanner"}

  into   "candidate" or "scanner". Left out, it is "candidate": the payload of an offer of
         today is byte for byte what it was.
         "scanner": the unit picks the manifest's image whose role is scanner, for the slot's
         board and band, and stores it as the slot's scanner image. Everything else is an
         offer as it is: the manifest and its signature, the six checks, ranges, progress,
         the lease, a download taken up again at the next wake.

Result    FirmwareOfferResult.json, as it is. imageId is the scanner image's
          (scanner.rak4630-single.868). An unknown word in `into`, or "scanner" for the
          Walter's own product, is answered stored: false with a reason.
```

The line above is the fixture, byte for byte. meshmaster.dk's task 235 is written against the
same line and keeps it under the same name. The server makes such an offer only to a unit that advertises
`twinoak.noisescope.session`, which is task 04.

## Deliverables

1. **`into` is read.** Left out or `candidate`: the offer is what it is today, and every test
   of it passes untouched. `scanner`: the same fetch into the scanner slot.
2. **Taken up again as a candidate's download is.** A scanner image half fetched when the
   wake's budget ends posts no result, and the next wake's claim of the same job continues
   the file. The next sleep is shortened for it by the rule a candidate's has. A scanner slot
   that an erase or a promotion left void reads as empty and not as a download (pile 15,
   task 01: say in the report whether the scanner slot was already covered).
3. **Stored already.** An offer for a scanner image the slot already holds, verified, answers
   `stored` and fetches nothing, as a candidate's does.
4. **Refused in words**, `stored: false` and a reason: a word in `into` that is neither;
   `scanner` for the Walter's own product; a release with no scanner image for the slot's
   board and band.
5. **Nothing else moves.** The candidate, the golden image and the bad list are untouched by
   a scanner offer, whatever it comes to.
6. **The fixture** `FirmwareOfferPayloadScanner.json` waits in
   `host_test/fixtures/contracts-ahead/` as the line above, byte for byte, with its row in
   that directory's README. The copied tree is not edited, and the tests of an offer into the
   candidate go on reading its `FirmwareOfferPayload.json`.
7. **The words.** `docs/cycle.md` (the page for the implementer of `Backend`), `docs/update.md`.
   `docs/bench.md` §17, one item: an offer with `into: scanner` from a bench server, then
   `image list` names the scanner image and the candidate is as it was. One line in
   `release-notes/unreleased/`.

## Acceptance criteria

- [ ] `contracts-ahead/FirmwareOfferPayloadScanner.json` is the line above byte for byte, and the copied tree still matches its `MANIFEST.txt` (tests).
- [ ] A payload with no `into` and one with `candidate`: the candidate slot is filled, as today (tests).
- [ ] `into: scanner` against a scripted release with both images: the scanner slot holds the scanner image, the result's `imageId` is `scanner.rak4630-single.868`, the candidate slot is byte for byte what it was (test).
- [ ] The same download stopped by the budget after half the file: no result is posted; the next wake's claim finishes it and the bytes fetched in all are the file's once (test).
- [ ] A scanner image already held with the offered hash: `stored`, no range is asked for (test).
- [ ] `into: golden`, and `into: scanner` with the Walter's product: `stored: false`, each with its reason, nothing fetched (tests).
- [ ] A release whose manifest has no scanner image for this board and band: `stored: false` with the reason (test).
- [ ] After an `image erase scanner` the slot reads empty and the next sleep is not shortened (test).
- [ ] `twinoak.fw.state` names the scanner image stored (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Flashing it: `rak role scanner` and task 03. Advertising the session type: task 04. The
server's half. The board: no session opens a port. A release: Thomas presses the button.
