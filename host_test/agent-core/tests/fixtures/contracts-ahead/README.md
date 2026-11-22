# The fixtures that wait

**Four wait**, all of them pile 1.20's and all written from the four lines
meshmaster.dk's task p4.47-253 carries: the claim of a wake that has already made
one, the answer to a result that says more work is waiting, and the two claims
that say what the exit node holds. The six that
waited before — the neighbours result of task p1.15-56 and the update's five of
task p1.15-61 — came across in the sync of 19 November, the same bytes as the
lines that had waited, and the tests read them from the copied tree
([`tests/fixtures/contracts/`](../contracts/README.md)). The directory stays,
because the two repositories' piles will run side by side again — as they do
now.

**Not a copy — ever.** Every file here is an envelope of the agent protocol that
twinoak/meshmaster.dk has not put on its `master` at the time the task that needs
it ran, written here **from the line the task carries** so that this side can be
built and tested against the same bytes. The two repositories' piles run at the
same time, and neither waits for the other.

`tools/sync-fixtures.ps1` does not touch this directory and
[`MANIFEST.txt`](../contracts/MANIFEST.txt) does not list it. The copied tree is
the contract and stays the contract: when the other side lands,

```powershell
.\tools\sync-fixtures.ps1 -Commit master
```

brings the real file across, it must be **the same bytes** as the one here, the
tests are pointed at the copied tree and the file here goes. A difference is a
wire the two ends do not agree on, and it is found before any release — it is the
first item of the pile's `FOLLOWUPS.md`.

| File | Written for | From meshmaster.dk's |
|---|---|---|
| `ClaimRequestLeft.json` | task p1.20-69: `transactionsLeft` after `max`, which a wake states in every claim after its first | task p4.47-253 |
| `JobResultReplyWaiting.json` | task p1.20-69: `jobsWaiting` last in the answer to a result, on which a wake claims again | task p4.47-253 |
| `ClaimRequestHolding.json` | task p1.20-70: `holding` last, the job ids this exit node is running or keeps a result for | task p4.47-253 |
| `ClaimRequestHoldingNothing.json` | task p1.20-70: the same member **empty**, which says *nothing held* where an absent member says *this agent does not say* | task p4.47-253 |

## Adding one

1. Write the file here, from the one line the task carries, with the single
   trailing newline every fixture of the copied tree has. Do not touch
   `tests/fixtures/contracts/` and do not run a sync.
2. Add its line to `kWaiting` in
   [`test/test_contract_fixtures/test_contract_fixtures.cpp`](../../../test/test_contract_fixtures/test_contract_fixtures.cpp),
   beside the bytes themselves — a file here that no test holds to its task's
   line fails the build, and so does one that is already in the copied tree.
3. Add a row to the table above, and name the task on both sides.
4. Read it in the tests exactly as a copied fixture is read, from
   `AHEAD_FIXTURES_DIR` (`platformio.ini`, `[env:native]`).
5. Put the sync, the byte-for-byte comparison and the deletion into the pile's
   `FOLLOWUPS.md`, tagged for the next pile.

A file here whose envelope **nests** — one fixture carried verbatim inside
another, as the copied tree's pairs are — says so in its row and is held to it by
the same suite.

[`docs/upstream.md`](../../../docs/upstream.md) is the page: there first, here
second, and the sync's copy is the one that holds.
