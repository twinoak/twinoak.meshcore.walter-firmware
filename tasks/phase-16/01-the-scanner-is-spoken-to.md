# 01 — The scanner is spoken to

**Depends on:** —
**Spec:** `spec/LINK-PROTOCOL.md` §9 (*NoiseScope 0.1 speaks NDJSON lines on this same UART, not frames*); `spec/SYSTEM.md` S-12; `components/core/include/core/link/client/client.h` (`kProbeWaitMs` ~L66, `kTextLineMax` ~L77, `kTextRing` ~L78, `Fault::kTextMode` ~L95, `Mode` ~L105, `TextWatcher` ~L171, the counters ~L196, the text calls ~L315–325, the probe ~L366, the line's own storage ~L408–413), `components/core/link/client/client.cpp` (`probeForScanner` ~L575, where a byte becomes a line); `components/core/console/` (`rak text`: `commands.cpp` ~L81, `console.cpp` ~L1105 and ~L1502) and `components/core/include/core/console/parser.h` (~L69); `docs/console.md` (~L114, ~L333), `docs/bench.md` §3 (*NoiseScope on the RAK*, ~L334) and §9 (*A role switch and back keeps the identity*, ~L877); `tools/heap_at_start.py` and rail R22. In the repeater firmware's repository, read only: `docs/noisescope.md` §5 (*Framing*, *Commands*, *Messages*).
**Size:** one session.
**Model:** claude-opus-5

## Goal

When the radio runs NoiseScope the link's client knows it, keeps the last three lines it heard
cut to 320 bytes, and has said one word to it: `info`, to find out. That was enough to tell a
scanner from a silent radio.

A measurement needs two things more. The scanner must be told what to measure: it takes plain
lines (`sweep`, `hist 869.4 869.8 25`, `stop`) and answers each with an `ack` line. And what it
says back must be had whole: a `sweep` line of the band is about 4 kB, the longest it can send
about 7.6 kB.

## Deliverables

1. **A line said.** In text mode, one command line is sent and its `ack` is waited for inside
   `kProbeWaitMs`. Three answers: the scanner said ok; it said not ok, with its `msg`; it said
   nothing. The `ack` that counts is the one whose `cmd` is the line's first word. In any other
   mode nothing is sent and the fault says which mode it was. No frame in text mode and no
   text in frame mode, as §9 has it.
2. **Whole lines, for whoever lends the room.** A caller lends the client a buffer. While it
   is lent, every line that begins with `{` is laid down in it whole, one after another, each
   ended with `\n`. A line that does not begin with `{` is not kept. A line that does not fit
   in what is left ends the lending as *full*, and nothing of that line is kept. The caller
   takes the buffer back and is told how many lines and bytes it holds, and whether a `done`
   line for a given command is among them.
3. **`rak text` is what it was.** The three cut lines, their counts and the watcher are
   untouched, lending or not.
4. **No room of this firmware's own.** The buffer is the caller's. Static storage does not
   grow by more than the few words that point at it.
5. **Counted**, beside the text counters: lines said, `ack`s that were not ok, lines said that
   got no `ack`, lendings ended as full.
6. **`rak say <words>`.** Says the words to a scanner, prints the `ack`, then prints whole
   every line of the next five seconds, or until a `done` for that command. Printed and not
   logged (S-12). With no scanner there it says so and sends nothing. It takes a buffer of
   8 192 bytes from the heap for the length of the command and gives it back, and says so when
   the heap has none.
7. **The words.** `docs/console.md`. `docs/bench.md` §9, one item: after `rak role scanner`,
   `rak say sweep` prints `ack` … `started`, one `sweep` line with `"n":281` and no cut, and a
   `done`; `rak say dfu` is not to be tried on a mast. One line in
   `release-notes/unreleased/`.

## Acceptance criteria

Against a scripted scanner on the simulated port.

- [ ] `sweep` said, `{"t":"ack","cmd":"sweep","ok":true,"msg":"started"}` scripted: answered ok (test).
- [ ] An `ack` with `ok` false and a `msg`: answered not ok, with the `msg` (test).
- [ ] An `ack` for another command and then silence: not answered (test).
- [ ] Said in frame mode and in unknown mode: no byte goes out, and the fault names the mode (tests).
- [ ] A lent buffer of 8 192 bytes and a scripted `sweep` line of 7 600 bytes arriving in reads of 64: the buffer holds that line whole, byte for byte (test).
- [ ] Two lines in one read, and one line over three reads: each whole, in order (tests).
- [ ] A `# help` line between two lines: not in the buffer (test).
- [ ] A line that would end past the buffer's end: the lending ends full, the lines before it are whole and nothing follows them (test).
- [ ] A `{"t":"done","cmd":"sweep"…}` among the lines: found for `sweep`, not found for `hist` (test).
- [ ] With a buffer lent, `rak text`'s three lines and counts are what they are without one (test).
- [ ] No text of a scanner's line is in the log, the journal or a report after `rak say` (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Understanding a line beyond `t`, `cmd` and `ok`. Deciding what is said: tasks 03 and 04.
Posting a line anywhere. The scanner's firmware. The board: no session opens a port. A release:
Thomas presses the button.
