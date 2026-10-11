#!/usr/bin/env python3
"""The last gate before anything is published: do these files pass the checks a Walter will make?

    python tools/release/verify_release.py out/release                 # keys/release-keys.txt
    python tools/release/verify_release.py --test-key out/release      # a dry run's

Walks spec/FIRMWARE-MANIFEST.md §5 in its order, over the files on disk, with the public keys a
firmware build would trust and nothing else:

  1. the signature line names a trusted key and verifies over manifest.json's bytes;
  2. the manifest validates (§2) and its product is this firmware's;
  3. there is the one image this product publishes (`manager.walter.lte-m`);
  4. every file's size and SHA-256 are the manifest's - and SHA256SUMS agrees with them;
  5. every file holds exactly one descriptor and it agrees with its entry;
  6. (§5's sixth check is about a `swd` file's app_region; this product publishes none.)

A release that cannot pass this is not published - that is the whole reason the workflow runs it
after signing and before `gh release create`. It is also what the local dry run in tools/verify.sh
ends with, so the gate is exercised on every verified commit and not for the first time on a tag.

**The trusted keys.** keys/release-keys.txt is the release's trust table, the file phase 2
generates the firmware's compiled-in key list from. It is empty today, which means no key is
trusted, which means a tag cannot be published - and that is correct: the key does not exist yet
(spec/SYSTEM.md §10 item 1, keys/README.md). --test-key trusts the specification's public test key
instead, for a rehearsal.
"""

import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))
sys.path.insert(0, HERE)

import manifestref  # noqa: E402

from build_release import BAND, BOARD, LINK, PRODUCT, ROLE, SLOT_ANY, Refused  # noqa: E402

KEYS_FILE = os.path.join(REPO, "keys", "release-keys.txt")

NO_KEYS = """no release key is trusted: %s names none.

That is the state before the key exists, and it is safe - a Walter with no key compiled in accepts
no update at all (spec/FIRMWARE-MANIFEST.md section 3). It also means this release cannot be
verified and therefore must not be published.

Make the key once (spec/SYSTEM.md section 10, item 1):

    python spec/tools/manifestref.py keygen

Put the `public` half in %s as

    ed25519 <key id> <public hex>   # who made it, when

and the `private` half in the RELEASE_SIGNING_KEY secret. A rehearsal needs no key: --test-key."""


def test_public_key():
    """The public half of the specification's test key, read from the fixtures it publishes it in
    (spec/fixtures/manifest/index.json, `test_public_key`) rather than written down again here."""
    path = os.path.join(REPO, "spec", "fixtures", "manifest", "index.json")
    with open(path, encoding="utf-8") as fh:
        index = json.load(fh)
    public = bytes.fromhex(index["test_public_key"])
    if manifestref.key_id(public) != index["test_key_id"]:
        raise Refused("%s: test_public_key and test_key_id do not agree" % path)
    return public


def load_keys(path):
    """The trust table: one key a line, `ed25519 <key id> <public hex>`, # to end of line a comment.
    Room for three (section 3), so that a key can be replaced by shipping a build that trusts both.
    Never the specification's test key: a build that trusted it would accept anybody's firmware
    (section 3), so a table naming it is refused here - for the release gate and for the
    firmware's own table, which tools/gen_trust_table.py reads through this function."""
    keys = []
    test_key = test_public_key()
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as fh:
            for lineno, raw in enumerate(fh, 1):
                line = raw.split("#", 1)[0].strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) != 3 or parts[0] != "ed25519":
                    raise Refused("%s line %d: expected `ed25519 <key id> <public hex>`, got %r"
                                  % (path, lineno, raw.strip()))
                try:
                    public = bytes.fromhex(parts[2])
                except ValueError:
                    raise Refused("%s line %d: the public key is not hex" % (path, lineno))
                if len(public) != 32:
                    raise Refused("%s line %d: an Ed25519 public key is 32 bytes, this is %d"
                                  % (path, lineno, len(public)))
                if manifestref.key_id(public) != parts[1]:
                    raise Refused("%s line %d: the key id is %s, not %s (section 3: the first eight "
                                  "bytes of SHA-256 over the public key)"
                                  % (path, lineno, manifestref.key_id(public), parts[1]))
                if public == test_key:
                    raise Refused("%s line %d names the specification's public test key (%s). It is "
                                  "never in the trust table: a build that trusted it would accept "
                                  "anybody's firmware (section 3). A test build is made with "
                                  "-DTWINOAK_TRUST_TEST_KEY=1, not by editing this file."
                                  % (path, lineno, parts[1]))
                keys.append(public)
    if len(keys) > 3:
        raise Refused("%s names %d keys; a build has room for three (section 3)" % (path, len(keys)))
    return keys


def read(path):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise Refused("cannot read %s: %s" % (path, exc))


def verify(out_dir, keys, label_of_keys):
    checks = []

    def ok(line):
        checks.append("ok    " + line)

    manifest_path = os.path.join(out_dir, "manifest.json")
    data = read(manifest_path)
    sig_path = manifest_path + ".sig"
    sig = read(sig_path).decode("ascii", "replace")

    # 1. the signature.
    if not keys:
        raise Refused(NO_KEYS % (KEYS_FILE, KEYS_FILE))
    if not manifestref.verify(keys, data, sig):
        raise Refused("the signature in %s does not verify over %s with %s. Either the manifest "
                      "changed after it was signed, or it was signed by a key this release does "
                      "not trust (spec/FIRMWARE-MANIFEST.md section 3)."
                      % (os.path.basename(sig_path), os.path.basename(manifest_path), label_of_keys))
    ok("the signature verifies with %s: %s" % (label_of_keys, sig.strip().split(" ")[1]))

    # 2. the manifest.
    if data.startswith(b"\xef\xbb\xbf"):
        raise Refused("manifest.json begins with a byte-order mark; §2 says UTF-8 without one, and "
                      "it is not in the canonical form this repository writes")
    try:
        manifest = json.loads(data)
    except ValueError as exc:
        raise Refused("manifest.json is not JSON: %s" % exc)
    try:
        manifestref.validate(manifest)
    except ValueError as exc:
        raise Refused("the manifest does not validate (section 2): %s" % exc)
    if manifest["product"] != PRODUCT:
        raise Refused("the manifest's product is %r, not %r" % (manifest["product"], PRODUCT))
    if manifestref.canonical(manifest) != data:
        raise Refused("manifest.json is not in the canonical form this repository writes "
                      "(section 2, 'How ours is written')")
    ok("the manifest validates, is %s, and is in canonical form" % PRODUCT)
    if manifest["link"] != LINK:
        raise Refused("the manifest says link %r and this firmware speaks %r" % (manifest["link"], LINK))

    # 3. the one image this product publishes.
    want_id = "%s.%s.%s" % (ROLE, BOARD, BAND)
    images = [i for i in manifest["images"] if i["id"] == want_id]
    if len(images) != 1 or len(manifest["images"]) != 1:
        raise Refused("expected exactly one image, %s; the manifest has %r"
                      % (want_id, [i["id"] for i in manifest["images"]]))
    image = images[0]
    ok("one image, %s, mcu %s" % (image["id"], image["mcu"]))

    # 4. and 5. each file.
    for entry in image["files"]:
        blob = read(os.path.join(out_dir, entry["name"]))
        if len(blob) != entry["size"]:
            raise Refused("%s is %d bytes and the manifest says %d" % (entry["name"], len(blob), entry["size"]))
        digest = hashlib.sha256(blob).hexdigest()
        if digest != entry["sha256"]:
            raise Refused("%s hashes to %s and the manifest says %s" % (entry["name"], digest, entry["sha256"]))
        try:
            offset, fields = manifestref.find_descriptor(blob)
        except ValueError as exc:
            raise Refused("%s: %s (section 4)" % (entry["name"], exc))
        wanted = {"role": image["role"], "board": image["board"], "band": image["band"],
                  "version": manifest["version"], "slot": SLOT_ANY,
                  "link_major": manifest["link"]["major"], "link_minor": manifest["link"]["minor"]}
        wrong = ["%s is %r, not %r" % (k, fields.get(k), v) for k, v in sorted(wanted.items())
                 if fields.get(k) != v]
        if wrong:
            raise Refused("%s: the descriptor at 0x%X disagrees with the manifest: %s"
                          % (entry["name"], offset, "; ".join(wrong)))
        ok("%s (%s, %d bytes, loads at %s): the hash is the manifest's and the one descriptor at "
           "0x%X says %s/%s/%s %s" % (entry["name"], entry["use"], entry["size"],
                                      entry.get("load_address", "-"), offset, fields["role"],
                                      fields["board"], fields["band"], fields["version"]))

    # SHA256SUMS, as a reader of the release would use it.
    sums_path = os.path.join(out_dir, "SHA256SUMS")
    counted = 0
    for raw in read(sums_path).decode("ascii").splitlines():
        if not raw.strip():
            continue
        digest, name = raw.split(None, 1)
        name = name.strip().lstrip("*")
        got = hashlib.sha256(read(os.path.join(out_dir, name))).hexdigest()
        if got != digest:
            raise Refused("SHA256SUMS says %s for %s, and it hashes to %s" % (digest, name, got))
        counted += 1
    if counted < len(image["files"]):
        raise Refused("SHA256SUMS covers %d files and the release has %d images"
                      % (counted, len(image["files"])))
    ok("SHA256SUMS: %d files, every hash correct" % counted)

    return checks, manifest


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_dir", help="the directory build_release.py wrote")
    ap.add_argument("--test-key", action="store_true",
                    help="trust the specification's public test key instead of keys/release-keys.txt")
    ap.add_argument("--keys", default=KEYS_FILE)
    args = ap.parse_args(argv[1:])

    try:
        if args.test_key:
            keys = [test_public_key()]
            label = "the specification's public test key"
        else:
            keys = load_keys(args.keys)
            where = os.path.relpath(args.keys, REPO)
            if where.startswith(".."):  # a table outside the repository: say where it really is
                where = args.keys
            label = "%s (%d key%s)" % (where, len(keys), "" if len(keys) == 1 else "s")
        checks, manifest = verify(args.out_dir, keys, label)
    except Refused as exc:
        print("REFUSED: %s" % exc, file=sys.stderr)
        return 1
    print("\n".join(checks))
    print("%s %s (%s) passes all of spec/FIRMWARE-MANIFEST.md section 5 that applies to it"
          % (manifest["product"], manifest["tag"], manifest["version"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
