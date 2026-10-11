#!/usr/bin/env python3
"""Sign a release manifest. Ed25519 over the manifest's bytes (spec/FIRMWARE-MANIFEST.md §3).

    RELEASE_SIGNING_KEY=<private hex>  python tools/release/sign_manifest.py out/release/manifest.json
    python tools/release/sign_manifest.py --test-key out/release/manifest.json

The first is the release: the private key comes from an environment variable and from nowhere else
- the workflow's `RELEASE_SIGNING_KEY` secret (.github/workflows/release.yml). There is no
`--key` argument and no key file, because an argument is in the process table and a file is one
`git add` from being committed. **A tag with no such secret fails**, with the instructions for
making the key.

The second is a rehearsal: it signs with `spec/fixtures/manifest/TEST-KEY.txt`, the key pair the
specification publishes on purpose. The test key may only sign a version that is not a release -
`0.0.0-dryrun+<sha7>` or `0.0.0-dev`, not `X.Y.Z+<sha7>` - so a dry run's manifest can never be
mistaken for a signed release. The private key is read from spec/tools/manifestref.py, so a dry
run has no secret anywhere near it and nothing to print.

Nothing here prints a private key, in either mode. What it prints is the signature line it wrote:
the algorithm, the key id (§3: eight bytes of SHA-256 over the public key - not a secret) and the
signature. spec/tools/manifestref.py does the signing; this file decides what may be signed.
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))
sys.path.insert(0, HERE)

import manifestref  # noqa: E402

from build_release import RELEASE_VERSION, Refused  # noqa: E402

ENV_VAR = "RELEASE_SIGNING_KEY"

NO_KEY = """no signing key: %s is not set in the environment.

A release of this firmware is signed where it is built (spec/SYSTEM.md S-8) with the one private
key that also signs the repeater firmware's releases. It does not exist yet - it is Thomas's to
make, once (spec/SYSTEM.md section 10, item 1):

    python spec/tools/manifestref.py keygen

Store the printed `private` half as the secret %s of this repository and of
twinoak.meshcore.repeater-firmware, and put the `public` half in keys/release-keys.txt in both.
It is never committed, logged or pasted anywhere else. Until it exists, a tag fails here and no
unit accepts an update, which is the safe state.

A rehearsal that publishes nothing needs no secret: --test-key.""" % (ENV_VAR, ENV_VAR)


def release_shaped(manifest):
    """Whether this manifest claims to be a release: X.Y.Z+<sha> as the version, or vX.Y.Z as the
    tag. A dry run is neither, which is what makes the test key safe for it."""
    version = str(manifest.get("version", ""))
    tag = str(manifest.get("tag", ""))
    return bool(RELEASE_VERSION.fullmatch(version)) or bool(re.fullmatch(r"v\d+\.\d+\.\d+", tag))


def private_key(use_test_key):
    """The 32 private bytes, and a word for where they came from. Never returned to a caller that
    prints things, and never put in an argument or a file."""
    if use_test_key:
        return manifestref.TEST_PRIVATE, "the specification's public test key"
    raw = os.environ.get(ENV_VAR, "").strip()
    if not raw:
        raise Refused(NO_KEY)
    try:
        key = bytes.fromhex(raw)
    except ValueError:
        raise Refused("%s is not hex. It is the `private` line of `manifestref.py keygen`, "
                      "64 hex characters and nothing else." % ENV_VAR)
    if len(key) != 32:
        raise Refused("%s holds %d bytes; an Ed25519 private key is 32 (64 hex characters)."
                      % (ENV_VAR, len(key)))
    if key == manifestref.TEST_PRIVATE:
        raise Refused("%s holds the specification's test key, which is public and signs nothing "
                      "that is released (spec/FIRMWARE-MANIFEST.md section 3). Use --test-key for "
                      "a rehearsal, or set the secret to the real release key." % ENV_VAR)
    return key, "the key in %s" % ENV_VAR


def sign(manifest_path, use_test_key):
    with open(manifest_path, "rb") as fh:
        data = fh.read()
    try:
        manifest = json.loads(data)
    except ValueError as exc:
        raise Refused("%s is not JSON: %s" % (manifest_path, exc))

    key, whose = private_key(use_test_key)
    if use_test_key and release_shaped(manifest):
        raise Refused("refusing to sign %s %s with the specification's test key: that key is "
                      "public, so the signature would prove nothing, and a unit that trusted it "
                      "would accept anybody's firmware (spec/FIRMWARE-MANIFEST.md section 3). A "
                      "release is signed with the key in %s."
                      % (manifest.get("tag"), manifest.get("version"), ENV_VAR))

    line = manifestref.sign(key, data)
    with open(manifest_path + ".sig", "w", encoding="ascii", newline="\n") as fh:
        fh.write(line)
    return line.strip(), whose, manifest


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest", help="out/release/manifest.json; the signature goes beside it")
    ap.add_argument("--test-key", action="store_true",
                    help="sign with the specification's public test key. Refused for a release "
                         "version or a vX.Y.Z tag")
    args = ap.parse_args(argv[1:])
    try:
        line, whose, manifest = sign(args.manifest, args.test_key)
    except Refused as exc:
        print("REFUSED: %s" % exc, file=sys.stderr)
        return 1
    print("signed %s (%s, tag %s) with %s" % (manifest.get("product"), manifest.get("version"),
                                              manifest.get("tag"), whose))
    print("%s.sig: %s" % (args.manifest, line))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
