# The companion frames

A copy, like the contract fixtures next door. These `.hex` files are transcribed in
**twinoak/meshmaster.dk** from the MeshCore firmware that writes the bytes — each
one's header names the tag, the commit, the file, the line range and the function
it was read from, and a fixture without that citation cannot be added there — and
they are copied here so the firmware's own parsers are held to the same bytes as
the .NET agent's.

**On a board the frames are one layer thinner.** The `.hex` files are what a
*companion* sends an app over a cable: a push code, a reserved byte, and then a
prefix or a tag, followed by the reply the repeater actually sent. This board **is**
the companion, so what `agent/ReplyCodec.h` parses is the part after that
wrapper — the same bytes, with the serial protocol's envelope taken off:

| fixture | what this firmware parses |
|---|---|
| `status-response.hex`, `status-response-48.hex` | the `RepeaterStats` after `0x87`, the reserved byte and the six-byte prefix |
| `telemetry-response.hex` | the Cayenne LPP after `0x8b`, the reserved byte and the prefix |
| `binary-response-*.hex` | the response data after `0x8c`, the reserved byte and the four-byte tag |
| `login-success*.hex`, `login-failed.hex` | the whole body, which is what the companion composed from the repeater's reply |
| `trace-data*.hex` | the body after `0x89` and the reserved byte |

`test/test_reply_codec` does exactly that: it strips the documented wrapper and
feeds the rest to the board's parser, then checks the values `MeshMaster.Agent.Tests`
asserts for the same file.

**Never edit a `.hex` file in this directory.** They are written by
[`tools/sync-fixtures.ps1`](../../../tools/sync-fixtures.ps1) and by nothing else
(this `README.md` is the firmware's own and is not copied):

```powershell
.\tools\sync-fixtures.ps1 -Commit <sha>     # fetch both sets from that meshmaster.dk commit
.\tools\sync-fixtures.ps1 -Set companion    # this set only
.\tools\sync-fixtures.ps1 -Verify           # do the files still match MANIFEST.txt?
```

`MANIFEST.txt` records the meshmaster.dk commit the copy came from and the SHA-256
and byte count of every file, and is read from both sides: by `-Verify`, which CI
runs on every commit, and by `pio test -e native`, which checks that every listed
file is present, is the recorded size, and is a well-formed hex fixture with its
firmware citation.

The `captures/` sub-directory of meshmaster.dk — frames a real radio sent on a real
afternoon — is **not** copied: it is evidence of a session rather than a shape to
parse, and its provenance is the README beside it there.
