# The release keys

The public half of every key whose signature this firmware accepts on a release manifest, and
nothing else. **The private half is never in this repository** — not in a file, not in a default,
not in a fixture, not in a log line. It lives in one place: the `RELEASE_SIGNING_KEY` secret of
this repository's release workflow (`spec/FIRMWARE-MANIFEST.md` §3).

| | |
|---|---|
| `release-keys.txt` | the trust table: the keys a release may be signed with, and that a Walter build will trust. **Empty today.** |

## Why it is empty, and why that is right

The key does not exist yet. It is Thomas's to make, once, and it is the same key that signs the
repeater firmware's releases (`spec/SYSTEM.md` §10, item 1):

```
python spec/tools/manifestref.py keygen
```

Three things come out of that: `private`, `public` and `key id`.

- **`private`** goes into the repository secret `RELEASE_SIGNING_KEY`, here and in
  `twinoak.meshcore.repeater-firmware`, and nowhere else. Not into a password manager note that
  gets pasted into a terminal, not into a file in a checkout. If it is ever exposed, make a new
  one: a key is replaced by shipping a Walter release that trusts both the old and the new, then
  one that trusts only the new — which is why the table has room for three.
- **`public`** and **`key id`** go into `release-keys.txt`, one line:

  ```
  ed25519 <key id> <public hex>   # who made it, when
  ```

- Nothing goes into a commit message, an issue or a report.

Until that line is there:

- `tools/release/verify_release.py` refuses a release, because it has no key to verify the
  signature with — so `.github/workflows/release.yml` fails on a tag, before `gh release create`,
  with those instructions. A *Run workflow* dry run is unaffected: it signs and verifies with the
  specification's public test key and publishes nothing.
- the firmware, when phase 2 compiles this table into it, trusts no key and therefore **accepts no
  update at all**. That is the safe state, and the one the project is in until a person decides
  otherwise (`spec/FIRMWARE-MANIFEST.md` §3, last bullet).

## The test key is not in here

`spec/fixtures/manifest/TEST-KEY.txt` is a key pair the specification publishes on purpose, so
that every repository can test signature checking against the same one. It is never in this
table: a build that trusted it would accept firmware signed by anybody who can read the
specification. `tools/release/sign_manifest.py` refuses to sign a release version with it, and
`tools/release/test_release.py` holds that refusal.
