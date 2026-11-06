# 02 — The transfer in bytes

**Depends on:** nothing in this phase. The reviewer's `walterctl.py` changes of 6 November are on `main`.
**Spec:** `components/interfaces/include/interfaces/text_out.h`; `components/platform/usb_console.cpp` (`UsbConsole::write`: every `\n` becomes CR LF, 20 ms a write, what does not fit is dropped); `components/core/image/receive.cpp` (`Receiver::respond`: the leading 0x00 and the frame, through `out_.write`), `components/core/include/core/image/receive.h` (the transfer's types; the hello's text `walter image receive 1`); `components/core/console/console.cpp` (`Console::feed` while a transfer is active, `imageReceive`, `transferEnded`, `self receive`); `host_test/console_host/` (its `TextOut`), `tools/test_walterctl.py`, `tools/walterctl.py` (`UndoCrlf`, `SerialPort.read`, `--undo-crlf`); `spec/LINK-PROTOCOL.md` §2 (the framing the transfer borrows); `docs/console.md` (*image receive*), `docs/flashing.md`, `docs/bench.md`; `../phase-6/FOLLOWUPS.md`, *`walterctl.py push` cannot finish on a board*.
**Size:** one session.
**Model:** claude-opus-5

## Goal

On 6 November `walterctl.py push --as candidate` into a Walter at 0.1.0 got its hello answered and
then stopped:

```
no answer to type 0x03 seq 9; sending it again   (four times)
walterctl: no answer to 4 attempts: the Walter is not speaking this protocol
```

The Walter's answers are frames, and they go out through the console's text output. That output turns
every 0x0A into 0x0D 0x0A for the terminal. Each answer carries the chunk the Walter expects next, and
the answer to chunk 9 expects chunk 10, which is 0x0A. So that answer arrives corrupted every time, and
walterctl sends chunk 9 again. Any 0x0A in the byte count or the CRC breaks other answers the same way.
`self receive` uses the same receiver, so the Walter's own image over USB is broken too.

The host tests passed because `console_host` writes bytes verbatim. That evening the reviewer added
`--undo-crlf` to walterctl as a stopgap, the exact inverse of the conversion. The push then went
through, slowly, which is the second bug the reviewer fixed: `SerialPort.read` waited out the whole
5 s timeout for every chunk.

## Deliverables

1. **Frames are bytes.**
   - The transfer's answers reach the port verbatim, through a write that does no conversion. Add
     one beside `write` on `TextOut`, or give the receiver an interface of its own; choose and say why.
   - On the platform the raw write loops until every byte is out or a bound has passed, and is never
     silently short. `write`'s *drop what does not fit in 20 ms* is right for text on a mast and wrong
     for a frame.
   - `Receiver::respond` and every other binary path use it.
2. **The terminal's conversion in one place.**
   - LF to CR LF becomes one helper in `core/`, used by the platform's `UsbConsole::write` and by
     `console_host`'s `TextOut`. The host tests then see the bytes the board sends.
   - Run the tests with only this part in place. `test_walterctl.py` should now fail exactly as the
     board did. The report shows it.
3. **A push the board would have failed.**
   - `tools/test_walterctl.py` pushes an image of **more than ten chunks**: 240 bytes a chunk, so over
     2,400 bytes. Make it from the spec's test key with `spec/tools/manifestref.py`, the way the
     existing release is made.
   - Add a unit test of `Receiver::respond` whose answer holds 0x0A, through a converting `TextOut`.
     It must come out byte for byte.
4. **walterctl decides by the Walter's word, not by a flag.**
   - The transfer's hello says `walter image receive 1` today. Make it `2` for a Walter that writes
     frames as bytes.
   - walterctl undoes the conversion exactly when the Walter says `1` and the transport is a serial
     port, and never otherwise. If version 1's hello can itself be corrupted by the conversion, the
     version comes from the console's own text before the transfer instead (`status`'s first line), so
     the choice is deterministic either way.
   - `--undo-crlf` stays as a manual override if it still has a use; otherwise it goes. Say which.
   - **Review the reviewer's changes** of 6 November (`UndoCrlf`, `SerialPort.read`, the progress lines):
     - keep them;
     - fix anything wrong in them;
     - give `SerialPort.read` a test of its own against a fake serial object that answers short reads.
5. **Docs.**
   - `docs/console.md`: *image receive*, and what changed in 2.
   - `docs/flashing.md`: pushing into a 0.1.0 Walter, which walterctl now handles by itself.
   - `docs/bench.md`, for Thomas: a push of the repeater's release into a 0.1.1 Walter, timed (about
     1,740 chunks; seconds, not hours); a `self receive` of the Walter's own release.

## Acceptance criteria

- [ ] Frames are written verbatim and never cut short; the conversion lives in one helper that the console host uses too.
- [ ] With the helper in and the fix not, the >10-chunk push fails as on the board (in the report); with the fix, it passes, and so does the `respond` test.
- [ ] walterctl chooses the undo by the Walter's own word; the reviewer's changes are reviewed and kept or fixed; `SerialPort.read` has a test.
- [ ] Docs updated; the `../phase-6/FOLLOWUPS.md` item ticked.
- [ ] `tools/verify.ps1` green.

## Out of scope

The board. The transfer's protocol beyond the hello's version word.
