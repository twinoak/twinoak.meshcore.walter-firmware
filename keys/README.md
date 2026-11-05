# The release keys

The public half of every key whose signature this firmware accepts on a release manifest, and
nothing else. **The private half is never in this repository** — not in a file, not in a default,
not in a fixture, not in a log line. It lives in one place: the `RELEASE_SIGNING_KEY` secret of
this repository's release workflow (`spec/FIRMWARE-MANIFEST.md` §3).

| | |
|---|---|
| `release-keys.txt` | the trust table: the keys a release may be signed with, and that a Walter build will trust. **One key**, `73efdd383a957596`, since 20 October 2026. |

## The key that is in it

Thomas made it once, on 20 October 2026, and it is the same key that signs the repeater firmware's
releases (`spec/SYSTEM.md` §10, item 1):

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

That line is there, so:

- a tag publishes: `tools/release/sign_manifest.py` signs with the secret and
  `tools/release/verify_release.py` verifies the signature against this table before
  `.github/workflows/release.yml` creates the release. `v0.1.0` is the first one it made.
- the firmware compiles this table in and **accepts an update signed with that key and no other**.
  `trust` on the console prints `1 key this build accepts a release signed with:
  73efdd383a957596`.

**Were the line to go** — deleted, or edited to something that is not the key — the project would
go back to its safe state rather than to a dangerous one: `verify_release.py` would refuse a
release because it had no key to verify the signature with, so a tag would fail before
`gh release create`, and a build of the firmware would accept **no update at all**
(`spec/FIRMWARE-MANIFEST.md` §3, last bullet). A *Run workflow* dry run is unaffected either way:
it signs and verifies with the specification's public test key and publishes nothing.

## The test key is not in here

`spec/fixtures/manifest/TEST-KEY.txt` is a key pair the specification publishes on purpose, so
that every repository can test signature checking against the same one. It is never in this
table: a build that trusted it would accept firmware signed by anybody who can read the
specification. `tools/release/sign_manifest.py` refuses to sign a release version with it, and
`tools/release/test_release.py` holds that refusal.
