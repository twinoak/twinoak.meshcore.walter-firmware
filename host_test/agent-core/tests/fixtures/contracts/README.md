# The contract fixtures

A copy. These JSON files are generated in **twinoak/meshmaster.dk** by the
`MeshMaster.Contracts` round-trip tests — one file per envelope of the agent
protocol, written by the test itself so they cannot drift from the records —
and copied here so the firmware's own tests check the same bytes. The rail is
the one in the repository README: *the contract is the .NET agent's*, and a
difference is a bug in the firmware, not a second dialect.

**Never edit a `.json` file in this directory.** They are written by
[`tools/sync-fixtures.ps1`](../../../tools/sync-fixtures.ps1) and by nothing
else (this `README.md` is the firmware's own and is not copied):

```powershell
.\tools\sync-fixtures.ps1 -Commit <sha>   # fetch from that meshmaster.dk commit
.\tools\sync-fixtures.ps1 -Verify         # do the files still match MANIFEST.txt?
```

`MANIFEST.txt` records the meshmaster.dk commit the copy came from and the
SHA-256 and byte count of every file. Two things read it:

- `tools/sync-fixtures.ps1 -Verify`, which CI runs on every commit: a fixture
  whose bytes changed here fails the build, and the fix is to change it in
  meshmaster.dk and sync again.
- `pio test -e native` (`test/test_contract_fixtures/`), from the other side:
  every listed file is present, its size is the recorded one, it is well formed,
  and no `.json` in this directory is missing from the manifest.

The copy is normalised — UTF-8 without a BOM, LF, one trailing newline — so
that a fixture's SHA-256 is the same on every machine. That normalisation is
the only difference from the bytes in meshmaster.dk.

**Two of these files are not envelopes.** `JsonEscaping.json` is a test-only
record nothing sends: its one string carries a character per clause of the
escaping rule the shared serializer applies, so a second implementation can
prove its own writer against bytes instead of against a description
(`test/test_json_escaping`, and `agent/JsonWriter.h`'s header for the rule).
`AgentHeartbeatNoWake.json` is a second sample of a record that already has a
file — the heartbeat an exit node that does not pace itself sends, which is this
board's, held byte for byte by `test_envelopes`. meshmaster.dk's own README in
this directory is the authority on both; this one is the firmware's side of it.

A change to the *wire* protocol is a meshmaster.dk change first and a firmware
change second; [`docs/upstream.md`](../../../docs/upstream.md) is the page that
says so.
