#!/usr/bin/env python3
"""tools/gen_trust_table.py's tests (task p2-01): what the generated trust table says for the
repository's own key file (nothing), for a table with keys, for the --test-key build, and what it
refuses. Standard library plus `cryptography` for making keys; tools/verify.sh runs this in the
release-tests step.

    python tools/test_gen_trust_table.py
"""

import hashlib
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))
sys.path.insert(0, os.path.join(HERE, "release"))
sys.path.insert(0, HERE)

import manifestref  # noqa: E402
import verify_release  # noqa: E402

import gen_trust_table  # noqa: E402

INDEX = os.path.join(REPO, "spec", "fixtures", "manifest", "index.json")
KEYS_FILE = os.path.join(REPO, "keys", "release-keys.txt")


def some_key(seed):
    """A deterministic 32-byte public key for a test table: Ed25519's public keys are 32 bytes of
    any value as far as the table is concerned."""
    return hashlib.sha256(b"trust table test key %d" % seed).digest()


def bytes_in(header):
    """The bytes the header's initialisers spell, row by row: [(id, key), ...] of the real rows."""
    rows = re.findall(r"\{\{([^}]*)\},\s*\{([^}]*)\}\}", header)
    out = []
    for id_text, key_text in rows:
        if key_text.strip() == "0":
            continue
        id_bytes = bytes(int(x, 16) for x in id_text.split(","))
        key_bytes = bytes(int(x, 16) for x in key_text.split(","))
        out.append((id_bytes, key_bytes))
    return out


class TheTable(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="twinoak-trust-test-")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def table(self, lines):
        path = os.path.join(self.dir, "keys.txt")
        with open(path, "w", encoding="ascii") as fh:
            fh.write("\n".join(lines) + "\n")
        return path

    def test_the_repositorys_table_is_the_keys_that_file_names_and_never_the_test_key(self):
        """The header is keys/release-keys.txt and nothing else: the keys it names, with their ids
        recomputed, the rest of the three rows unused, and - whatever that file says - never the
        specification's public test key, which only --test-key may add (keys/README.md,
        spec/FIRMWARE-MANIFEST.md section 3). The file named no key until the release key was made
        (2026-10-20), and names one now; both are states of the project and neither is a rule."""
        header = gen_trust_table.generate(KEYS_FILE, None)
        named = verify_release.load_keys(KEYS_FILE)
        self.assertIn("kTrustedKeyCount = %d;" % len(named), header)
        self.assertIn("kTrustsTestKey = false;", header)
        rows = bytes_in(header)
        self.assertEqual([k for _, k in rows], named)
        for id_bytes, key in rows:
            self.assertEqual(id_bytes.hex(), manifestref.key_id(key))
        self.assertEqual(header.count("// unused"), 3 - len(named))
        test_key = verify_release.test_public_key()
        self.assertNotIn(test_key.hex(), header)
        self.assertNotIn(", ".join("0x%02x" % b for b in test_key), header)

    def test_keys_come_out_with_their_ids_in_order(self):
        keys = [some_key(1), some_key(2)]
        path = self.table(["# a comment", "ed25519 %s %s  # first" % (manifestref.key_id(keys[0]), keys[0].hex()),
                           "ed25519 %s %s" % (manifestref.key_id(keys[1]), keys[1].hex())])
        header = gen_trust_table.generate(path, None)
        self.assertIn("kTrustedKeyCount = 2;", header)
        self.assertIn("kTrustsTestKey = false;", header)
        rows = bytes_in(header)
        self.assertEqual([k for _, k in rows], keys)
        for id_bytes, key in rows:
            self.assertEqual(id_bytes.hex(), manifestref.key_id(key))
            self.assertEqual(id_bytes, hashlib.sha256(key).digest()[:8])
        self.assertEqual(header.count("// unused"), 1)

    def test_the_test_key_build(self):
        """--test-key appends the specification's test key, read from the fixture index, and
        marks the table. This is the only way the key reaches a build."""
        header = gen_trust_table.generate(KEYS_FILE, INDEX)
        named = verify_release.load_keys(KEYS_FILE)
        self.assertIn("kTrustedKeyCount = %d;" % (len(named) + 1), header)
        self.assertIn("kTrustsTestKey = true;", header)
        rows = bytes_in(header)
        self.assertEqual(len(rows), len(named) + 1)
        self.assertEqual(rows[-1][1], verify_release.test_public_key())
        self.assertEqual(rows[-1][0].hex(), "c17969deb0026955")
        self.assertIn("the test key of spec/fixtures/manifest/index.json", header)

    def test_the_test_key_in_the_release_table_is_refused(self):
        public = verify_release.test_public_key()
        path = self.table(["ed25519 %s %s" % (manifestref.key_id(public), public.hex())])
        with self.assertRaises(verify_release.Refused) as caught:
            gen_trust_table.generate(path, None)
        self.assertIn("test key", str(caught.exception))

    def test_a_wrong_key_id_is_refused(self):
        key = some_key(3)
        path = self.table(["ed25519 0000000000000000 %s" % key.hex()])
        with self.assertRaises(verify_release.Refused) as caught:
            gen_trust_table.generate(path, None)
        self.assertIn("key id", str(caught.exception))

    def test_three_keys_fit_and_a_fourth_does_not(self):
        keys = [some_key(i) for i in range(10, 13)]
        path = self.table(["ed25519 %s %s" % (manifestref.key_id(k), k.hex()) for k in keys])
        header = gen_trust_table.generate(path, None)
        self.assertIn("kTrustedKeyCount = 3;", header)
        self.assertNotIn("// unused", header)
        # Three release keys plus the test key is four: a test build cannot be made then either.
        with self.assertRaises(verify_release.Refused) as caught:
            gen_trust_table.generate(path, INDEX)
        self.assertIn("room for 3", str(caught.exception))
        keys.append(some_key(13))
        path = self.table(["ed25519 %s %s" % (manifestref.key_id(k), k.hex()) for k in keys])
        with self.assertRaises(verify_release.Refused):
            gen_trust_table.generate(path, None)

    def test_the_output_is_deterministic_and_only_rewritten_when_changed(self):
        out = os.path.join(self.dir, "gen", "trust_keys.h")
        self.assertEqual(gen_trust_table.main(["gen_trust_table.py", KEYS_FILE, out]), 0)
        with open(out, "rb") as fh:
            first = fh.read()
        before = os.stat(out).st_mtime_ns
        self.assertEqual(gen_trust_table.main(["gen_trust_table.py", KEYS_FILE, out]), 0)
        with open(out, "rb") as fh:
            self.assertEqual(fh.read(), first)
        self.assertEqual(os.stat(out).st_mtime_ns, before)
        self.assertEqual(gen_trust_table.generate(KEYS_FILE, None).encode("ascii"), first)
        self.assertNotIn(b"\r", first)

    def test_the_command_line_refuses_a_bad_table_with_exit_1(self):
        key = some_key(4)
        path = self.table(["ed25519 %s %s" % ("1" * 16, key.hex())])
        out = os.path.join(self.dir, "trust_keys.h")
        self.assertEqual(gen_trust_table.main(["gen_trust_table.py", path, out]), 1)
        self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main(verbosity=2)
