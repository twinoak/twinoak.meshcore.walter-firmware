# Contract fixtures this end has and meshmaster.dk has not — yet

Every other shape this firmware sends or reads is held to **meshmaster.dk's own bytes**: the
copied contract fixtures under `host_test/agent-core/tests/fixtures/contracts/`, read at run time
and never transcribed (`tasks/README.md`, `docs/upstream.md`). That tree is a copy, checked byte
for byte against its `MANIFEST.txt` before anything is built, so a shape that does not exist on
the server yet cannot be put there.

This directory is where such a shape waits. A file here is **not meshmaster.dk's**: it is this
firmware's reading of a contract agreed in words, written as the two blocks of the task that
agreed it, and it must be replaced by the server's own file — not merely compared with it — as
soon as that file exists.

| file | the contract | whose task | until then |
|---|---|---|---|
| `UnitReportPayload.json` | `twinoak.report`'s payload | meshmaster.dk's 216 | written here from `tasks/phase-13/01-what-the-unit-says.md`'s *The contract* |
| `UnitReportResult.json` | `twinoak.report`'s result | meshmaster.dk's 216 | the same |

`tasks/phase-13/FOLLOWUPS.md` carries the item that compares the two byte for byte once task 216
is deployed, and moves them into the copied tree where every other fixture is.
