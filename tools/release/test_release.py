#!/usr/bin/env python3
"""The release tooling's tests. Standard library (unittest) plus `cryptography`, which the image
already has (tools/requirements-verify.txt); tools/verify.sh runs this as the `release-tests` step.

    python tools/release/test_release.py            # or -v

What is held here is what must not happen, because each of these published once would be a unit in
a field with no settings, or an image nobody can identify, or a signature that proves nothing:

  * a factory image that reaches NVS, the journal or the image store;
  * a file with no descriptor, and one with two;
  * a descriptor that disagrees with the manifest entry it would be published under;
  * a factory image that does not carry the very application beside it, and an application whose
    esp_app_desc_t names another version or another project;
  * signing a release version with the specification's public test key;
  * signing with no key in the environment, and with the test key *in* the environment;
  * verifying a release with an empty trust table, and with the wrong key, and after one byte of
    the manifest changed;
  * publishing a release nobody wrote a word about: nothing waiting in release-notes/unreleased/,
    or a file there with nothing in it (the compiler itself is tested in test_release_notes.py);
  * publishing an image that does not start (task p11-04): the boot gate's reading of a boot is
    held here to two transcripts from the pinned image's QEMU - this firmware's, and v0.1.3's,
    which panicked before app_main on every board and in the emulator alike.

The shape of the manifest is held against spec/fixtures/manifest/manifest-walter.json, which
spec/tools/manifestref.py generated from FIRMWARE-MANIFEST.md: same keys, same order, same types.
"""

import contextlib
import copy
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, HERE)

import check_partitions
import manifestref

import boot_gate
import build_release
import release_notes
import sign_manifest
import verify_release
from build_release import Refused

FIXTURES = os.path.join(REPO, "spec", "fixtures", "manifest")
COMMIT = "1a2b3c4" + "d" * 33
DRYRUN = "0.0.0-dryrun+1a2b3c4"
RELEASE = "0.3.1+1a2b3c4"
BUILT_AT = "2026-10-11T12:00:00Z"
# The line a task left (tasks p7-01, p12-02), as every test that is not about the notes gets one:
# a release version needs at least one waiting line and is refused when there is none.
HANDWRITTEN = "What a person with a unit will notice. It has not been on a board.\n"


def partitions():
    with open(os.path.join(REPO, "partitions.csv"), encoding="utf-8") as fh:
        return {p.name: p for p in check_partitions.parse_csv(fh.read())}


def descriptor(version=DRYRUN, role="manager", board="walter", band="lte-m",
               major=build_release.LINK["major"], minor=build_release.LINK["minor"]):
    return manifestref.make_descriptor(role, version, board, band, major, minor)


def app_image(version=DRYRUN, descriptors=1, at=0x200, project=build_release.PROJECT_NAME,
              app_desc_version=None, app_desc=True, idf="v5.5.5", **kwargs):
    """A stand-in application image, shaped like the one ESP-IDF makes: 0xE9 at 0x0, an
    esp_app_desc_t at 0x20 (build_release.APP_DESC_OFFSET) and 256 bytes long, then filler with
    0, 1 or 2 TWOKFWD1 descriptors in it on 4-byte boundaries - the first at `at`, which is past
    the esp_app_desc_t, as the real one is (it is in .rodata, tens of kilobytes in)."""
    desc = descriptor(version=version, **kwargs)
    image = bytearray(manifestref.fake_image(desc, 4096, at, 7))
    image[0] = 0xE9
    if app_desc:
        block = bytearray(256)
        block[0:4] = struct.pack("<I", build_release.APP_DESC_MAGIC)
        block[16:48] = (app_desc_version or version).encode().ljust(32, b"\0")
        block[48:80] = project.encode().ljust(32, b"\0")
        block[112:144] = idf.encode().ljust(32, b"\0")
        image[build_release.APP_DESC_OFFSET:build_release.APP_DESC_OFFSET + 256] = block
    if descriptors == 0:
        image[at:at + len(desc)] = b"\x00" * len(desc)
    elif descriptors == 2:
        image[at + 0x400:at + 0x400 + len(desc)] = desc
    return bytes(image)


def factory_image(app=None, version=DRYRUN, size=None, **kwargs):
    """A stand-in merged image: what tools/check_partitions.check_factory looks for - a bootloader
    at 0x0, the table at 0x8000, 0xFF through the gap - with the application image itself at
    ota_0, which is what build_release.py holds it to. Unless the caller asks for a longer one it
    ends exactly at nvs's offset."""
    parts = partitions()
    if size is None:
        size = parts["nvs"].offset
    image = bytearray(b"\xff" * size)
    image[0] = 0xE9
    image[check_partitions.TABLE_OFFSET:check_partitions.TABLE_OFFSET + 2] = b"\xaa\x50"
    ota0 = parts["ota_0"].offset
    blob = app if app is not None else app_image(version=version, **kwargs)
    image[ota0:ota0 + len(blob)] = blob
    return bytes(image)


class ReleaseCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="twinoak-release-test-")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.out = os.path.join(self.dir, "release")
        # A release-notes/ of this test's own, so that what the repository's own directory holds
        # changes nothing here (tasks p7-01, p12-02). `build(notes=None)` leaves unreleased/
        # empty, which is the state a release is refused in.
        self.notes_dir = os.path.join(self.dir, "release-notes")
        os.makedirs(os.path.join(self.notes_dir, release_notes.UNRELEASED))

    def write(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def build(self, version=DRYRUN, tag=None, app=None, factory=None, factory_size=None,
              notes=HANDWRITTEN, **kwargs):
        """One build's two files: the factory image carries the application image, as it does in
        the real thing (idf.py merge-bin), unless a test deliberately gives another. `notes` is
        the line a task left, written into this test's release-notes/unreleased/; None leaves that
        directory empty, which is what a release is refused for."""
        blob = app if app is not None else app_image(version=version)
        app_path = self.write("app.bin", blob)
        fac_path = self.write("factory.bin", factory if factory is not None
                              else factory_image(app=blob, size=factory_size))
        tag = tag or "v" + version.split("+")[0]
        if notes is not None:
            with open(os.path.join(self.notes_dir, release_notes.UNRELEASED, "p0-00-a-task.md"),
                      "w", encoding="utf-8", newline="\n") as fh:
                fh.write(notes)
        kwargs.setdefault("notes_dir", self.notes_dir)
        kwargs.setdefault("notes_repo", self.dir)
        return build_release.build(version, COMMIT, tag, app_path,
                                   fac_path, self.out, os.path.join(REPO, "partitions.csv"),
                                   BUILT_AT, **kwargs)

    def manifest(self):
        with open(os.path.join(self.out, "manifest.json"), "rb") as fh:
            return fh.read()


# ------------------------------------------------------------------ the manifest it writes


class TheManifest(ReleaseCase):
    def test_it_validates_and_is_canonical(self):
        result = self.build()
        data = self.manifest()
        manifest = json.loads(data)
        manifestref.validate(manifest)
        self.assertEqual(manifestref.canonical(manifest), data)
        self.assertEqual(data[-1:], b"\n")
        self.assertNotIn(b"\r", data)
        self.assertEqual(manifest["product"], "twinoak-walter-firmware")
        self.assertEqual(manifest["version"], DRYRUN)
        self.assertEqual(manifest["tag"], "v0.0.0-dryrun")
        self.assertEqual(manifest["commit"], COMMIT)
        self.assertEqual(manifest["built_at"], BUILT_AT)
        self.assertEqual(manifest["link"], {"major": 1, "minor": 2})  # spec/LINK-PROTOCOL.md
        self.assertEqual([i["id"] for i in manifest["images"]], ["manager.walter.lte-m"])
        self.assertEqual(sorted(result["written"]),
                         sorted(["NOTES.md", "SHA256SUMS", "manifest.json",
                                 "twinoak-walter-v0.0.0-dryrun.bin",
                                 "twinoak-walter-v0.0.0-dryrun-factory.bin"]))

    def test_its_shape_is_the_specs_walter_fixture(self):
        """Same keys in the same order as the fixture manifestref.py generated, at all three
        levels. A field added to section 2 and to the fixtures shows up here as a difference."""
        self.build()
        ours = json.loads(self.manifest())
        with open(os.path.join(FIXTURES, "manifest-walter.json"), encoding="utf-8") as fh:
            theirs = json.load(fh)
        self.assertEqual(list(ours), list(theirs))
        self.assertEqual(list(ours["images"][0]), list(theirs["images"][0]))
        self.assertEqual([f["use"] for f in ours["images"][0]["files"]],
                         [f["use"] for f in theirs["images"][0]["files"]])
        for mine, mirror in zip(ours["images"][0]["files"], theirs["images"][0]["files"]):
            self.assertEqual(list(mine), list(mirror))
        for key in ("role", "board", "band", "mcu"):
            self.assertEqual(ours["images"][0][key], theirs["images"][0][key])

    def test_the_ota_load_address_is_the_first_slot_from_the_partition_table(self):
        result = self.build()
        ota = next(f for f in result["manifest"]["images"][0]["files"] if f["use"] == "ota")
        self.assertEqual(ota["load_address"], "0x%08X" % partitions()["ota_0"].offset)
        factory = next(f for f in result["manifest"]["images"][0]["files"] if f["use"] == "factory")
        self.assertEqual(factory["load_address"], "0x00000000")

    def test_sha256sums_covers_the_files(self):
        self.build()
        with open(os.path.join(self.out, "SHA256SUMS"), encoding="ascii") as fh:
            lines = [l.split(None, 1) for l in fh.read().splitlines() if l.strip()]
        self.assertEqual([name.strip() for _, name in lines],
                         ["twinoak-walter-v0.0.0-dryrun.bin",
                          "twinoak-walter-v0.0.0-dryrun-factory.bin", "manifest.json"])
        for digest, name in lines:
            with open(os.path.join(self.out, name.strip()), "rb") as fh:
                self.assertEqual(hashlib.sha256(fh.read()).hexdigest(), digest)

    def test_the_notes_say_what_a_factory_flash_keeps(self):
        self.build()
        with open(os.path.join(self.out, "NOTES.md"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("esptool", text)
        self.assertIn("0x0 twinoak-walter-v0.0.0-dryrun-factory.bin", text)
        self.assertIn("keeps a unit's settings", text)
        self.assertIn("check_partitions.py", text)
        self.assertIn("dry run", text)
        self.assertIn(COMMIT, text)
        # The ESP-IDF the image says it was built with, and the pinned modem library.
        self.assertIn("v5.5.5", text)
        self.assertIn("walter-modem", text)
        # The box link this build speaks, from the one place that says it (build_release.LINK,
        # which the descriptor inside the image is held against).
        self.assertIn("%d.%d (`spec/LINK-PROTOCOL.md`)" % (build_release.LINK["major"], build_release.LINK["minor"]), text)

    def test_a_release_versions_notes_do_not_say_dry_run(self):
        self.build(version=RELEASE)
        with open(os.path.join(self.out, "NOTES.md"), encoding="utf-8") as fh:
            self.assertNotIn("dry run", fh.read())

    def test_a_commit_that_is_not_40_hex_is_refused(self):
        app = self.write("app.bin", app_image())
        fac = self.write("factory.bin", factory_image())
        for commit in ("1a2b3c4", "", "Z" * 40):
            with self.assertRaises(Refused) as caught:
                build_release.build(DRYRUN, commit, "v0.0.0-dryrun", app, fac, self.out,
                                    os.path.join(REPO, "partitions.csv"), BUILT_AT)
            self.assertIn("40", str(caught.exception))


# ------------------------------------------------------------------ the notes it publishes


class TheNotesTop(ReleaseCase):
    """Tasks p7-01 and p12-02: a release's body is the lines the tasks wrote, then the generated
    part. The lines are release-notes/unreleased/*.md; a release with none is refused the way one
    with no key is. tools/release/test_release_notes.py holds the compiler itself."""

    def test_a_release_with_no_line_waiting_is_refused(self):
        with self.assertRaises(Refused) as caught:
            self.build(version=RELEASE, notes=None)
        message = str(caught.exception)
        self.assertIn("there is nothing to release", message)
        self.assertIn("release-notes/unreleased/", message)
        self.assertFalse(os.path.exists(os.path.join(self.out, "NOTES.md")))

    def test_a_file_with_only_whitespace_is_no_line_at_all(self):
        with self.assertRaises(Refused) as caught:
            self.build(version=RELEASE, notes="   \n")
        self.assertIn("is empty", str(caught.exception))

    def test_the_lines_come_first_and_the_generated_part_after_them(self):
        self.build(version=RELEASE, notes="The first release. It has not been on a board.\n")
        with open(os.path.join(self.out, "NOTES.md"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertTrue(text.startswith("## What is new\n\n- The first release. It has not been "
                                        "on a board.\n"), text[:160])
        top, rule, generated = text.partition("\n---\n")
        self.assertEqual(rule, "\n---\n")
        self.assertNotIn("esptool", top)
        self.assertIn("esptool", generated)
        self.assertIn("TwinOak Walter firmware v0.3.1", generated)
        self.assertIn("keeps a unit's settings", generated)

    def test_the_generated_part_links_the_flashing_guide_at_the_commit(self):
        """What the four things release-notes/README.md asks for can be generated is generated
        (task p12-02): the guide is linked rather than copied, at the commit being released, so
        that the link cannot rot into a page describing another release."""
        self.build(version=RELEASE)
        with open(os.path.join(self.out, "NOTES.md"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("%s/blob/%s/docs/flashing.md" % (build_release.REPO_URL, COMMIT), text)

    def test_a_dry_run_needs_no_line_and_gets_the_generated_part_alone(self):
        result = self.build(version=DRYRUN, notes=None)
        self.assertIsNone(result["handwritten"])
        with open(os.path.join(self.out, "NOTES.md"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertTrue(text.startswith("TwinOak Walter firmware v0.0.0-dryrun"), text[:120])
        self.assertIn("This is a dry run.", text)

    def test_a_dry_run_rehearses_the_body_when_lines_are_waiting(self):
        """A rehearsal is of the body as much as of the files: the lines that would be published
        are in a dry run's NOTES.md, which is how a line is read before it is released."""
        self.build(version=DRYRUN, notes="Something a person will notice. Not on a board.\n")
        with open(os.path.join(self.out, "NOTES.md"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertTrue(text.startswith("## What is new"), text[:80])
        self.assertIn("Something a person will notice.", text)
        self.assertIn("This is a dry run.", text)

    def test_the_repositorys_own_published_notes_are_named_for_a_tag_and_are_not_empty(self):
        """The real directory. A file named for a version that is not vX.Y.Z would be invisible
        until a release asked for it, which is the one moment it must not be found."""
        for name in sorted(os.listdir(build_release.NOTES_DIR)):
            path = os.path.join(build_release.NOTES_DIR, name)
            if name in ("README.md", release_notes.STANDING, release_notes.UNRELEASED):
                continue
            self.assertRegex(name, r"^v\d+\.\d+\.\d+\.md$")
            with open(path, encoding="utf-8") as fh:
                body = fh.read()
            self.assertTrue(body.strip(), path)
            self.assertNotIn(chr(13), body)


# ------------------------------------------------------------------ the four refusals


class TheRefusals(ReleaseCase):
    def test_a_factory_image_that_reaches_nvs(self):
        """One byte past nvs's offset and a bench flash would erase a unit's settings - and the
        radio images above them. check_partitions.py owns the rule; this is the release refusing
        to publish a file that breaks it."""
        nvs = partitions()["nvs"].offset
        with self.assertRaises(Refused) as caught:
            self.build(factory_size=nvs + 1)
        message = str(caught.exception)
        self.assertIn("nvs", message)
        self.assertIn("factory image is refused", message)
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json")),
                         "nothing is written when the release is refused")
        # One byte short of it is fine: the boundary is where it is said to be.
        self.build(factory_size=nvs)

    def test_a_factory_image_that_reaches_the_image_store_or_the_journal(self):
        parts = partitions()
        for name in ("journal", "rakfw", "coredump"):
            with self.subTest(name):
                with self.assertRaises(Refused) as caught:
                    self.build(factory_size=parts[name].offset + 1)
                # nvs is the lowest of them, so it is the one the message names; the point is that
                # the image is refused before it can reach anything of a unit in service.
                self.assertIn("ends at", str(caught.exception))

    def test_an_image_with_no_descriptor(self):
        """§4: an image that does not say what it is cannot be published. The factory image
        carries the same application, so it has none either, and both are refused."""
        with self.assertRaises(Refused) as caught:
            self.build(app=app_image(descriptors=0))
        self.assertIn("0 valid descriptors", str(caught.exception))
        self.assertIn("section 4", str(caught.exception))

    def test_an_image_with_two_descriptors(self):
        """Two and the image cannot be believed about which it is - §4 refuses it as firmly as none."""
        with self.assertRaises(Refused) as caught:
            self.build(app=app_image(descriptors=2))
        self.assertIn("2 valid descriptors", str(caught.exception))

    def test_an_image_that_is_not_an_esp_idf_application(self):
        """esp_app_desc_t is the other thing the build writes into the image. Without it the file
        is not an application of ours at all, whatever a TWOKFWD1 descriptor in it says."""
        with self.assertRaises(Refused) as caught:
            self.build(app=app_image(app_desc=False))
        self.assertIn("esp_app_desc_t", str(caught.exception))

    def test_an_esp_app_desc_that_names_another_version_or_project(self):
        """The two things the build writes about itself - esp_app_desc_t and the TWOKFWD1
        descriptor - come from the same PROJECT_VER. A release in which they differ is not one."""
        with self.assertRaises(Refused) as caught:
            self.build(app=app_image(app_desc_version="0.0.0-dev"))
        self.assertIn("esp_app_desc_t", str(caught.exception))
        self.assertIn("PROJECT_VER", str(caught.exception))
        with self.assertRaises(Refused) as caught:
            self.build(app=app_image(project="hello-world"))
        self.assertIn("hello-world", str(caught.exception))

    def test_a_descriptor_that_disagrees_with_the_manifest_entry(self):
        """§5 check 5, applied where the release is made: the bytes inside the file and the entry
        above it say the same thing, or nothing is published."""
        cases = [
            # The TWOKFWD1 version differs while esp_app_desc_t still says the release's, so it is
            # this check and not the esp_app_desc one that refuses it.
            ({"version": "9.9.9+deadbee", "app_desc_version": DRYRUN}, "version"),
            ({"role": "repeater"}, "role"),
            ({"board": "rak4630-single"}, "board"),
            ({"band": "868"}, "band"),
            ({"major": 2}, "link_major"),
            ({"minor": 7}, "link_minor"),
        ]
        for kwargs, named in cases:
            with self.subTest(**kwargs):
                with self.assertRaises(Refused) as caught:
                    self.build(app=app_image(**kwargs))
                message = str(caught.exception)
                self.assertIn("does not agree with the manifest entry", message)
                self.assertIn(named, message)

    def test_the_factory_image_must_carry_this_very_application(self):
        """One build makes both files (`idf.py build` then `merge-bin`), so the factory image holds
        the application image byte for byte at the slot the manifest gives as its load address. A
        pair that does not came from two builds, and a release of two builds is how a unit ends up
        flashed with one version and told it has another."""
        app = app_image(version=DRYRUN)
        other = bytearray(factory_image(app=app))
        offset, _ = manifestref.find_descriptor(app)
        ota0 = partitions()["ota_0"].offset
        # Change one byte of the descriptor inside the factory image's copy - the kind of
        # difference that is invisible in the file sizes.
        changed = bytearray(app[offset:offset + 96])
        changed[11] = 0x00  # slot 0 instead of 0xFF, "any"
        changed[92:96] = struct.pack("<I", zlib.crc32(bytes(changed[:92])) & 0xFFFFFFFF)
        other[ota0 + offset:ota0 + offset + 96] = changed
        with self.assertRaises(Refused) as caught:
            self.build(app=app, factory=bytes(other))
        message = str(caught.exception)
        self.assertIn("does not carry this application", message)
        self.assertIn("0x%X" % ota0, message)

    def test_a_table_with_no_application_slot(self):
        table = os.path.join(self.dir, "no-slots.csv")
        with open(table, "w", encoding="utf-8") as fh:
            fh.write("nvs, data, nvs, 0x9000, 0x6000,\n")
        app = self.write("app.bin", app_image())
        fac = self.write("factory.bin", factory_image())
        with self.assertRaises(Refused) as caught:
            build_release.build(DRYRUN, COMMIT, "v0.0.0-dryrun", app, fac, self.out, table, BUILT_AT)
        self.assertIn("ota_0", str(caught.exception))

    def test_a_test_key_build_is_refused_by_its_version(self):
        """Task p2-01: -DTWINOAK_TRUST_TEST_KEY=1 stamps the version +TESTKEY. Whatever else the
        build looks like, it is never released, and the message says how to build without it."""
        version = RELEASE + build_release.TEST_KEY_STAMP
        with self.assertRaises(Refused) as caught:
            self.build(version=version, tag="v0.3.1", app=app_image(version=version))
        message = str(caught.exception)
        self.assertIn("test-key build", message)
        self.assertIn("TWINOAK_TRUST_TEST_KEY", message)
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json")))

    def test_an_image_carrying_the_test_keys_bytes_is_refused(self):
        """And by the key's own 32 bytes in the image - the other witness, which a version
        string cannot lie about. The key is read from the fixture index, as the tooling reads it."""
        key = build_release.test_public_key()
        self.assertEqual(len(key), 32)
        for which in ("app", "factory"):
            with self.subTest(which):
                app = app_image()
                if which == "app":
                    app = bytearray(app)
                    app[0x800:0x800 + 32] = key
                    app = bytes(app)
                    factory = factory_image(app=app)
                else:
                    factory = bytearray(factory_image(app=app))
                    # Past the application's copy, so that the two files still carry the same app.
                    at = partitions()["ota_0"].offset + len(app) + 0x100
                    factory[at:at + 32] = key
                    factory = bytes(factory)
                with self.assertRaises(Refused) as caught:
                    self.build(app=app, factory=factory)
                message = str(caught.exception)
                self.assertIn("test key", message)
                self.assertIn("never released", message)
        # The same images without the key are a release.
        self.build()

    def test_a_build_made_with_another_version_is_refused(self):
        """ESP-IDF's project_description.json records the PROJECT_VER the build was given. A
        release under a different string would publish an image whose descriptor says the other
        one - caught here, before the descriptor check, with the command that fixes it."""
        build_dir = os.path.join(self.dir, "build")
        os.makedirs(build_dir)
        with open(os.path.join(build_dir, "project_description.json"), "w", encoding="utf-8") as fh:
            json.dump({"project_version": "0.0.0-dev", "idf_version": "v5.5.5"}, fh)
        app = self.write("app.bin", app_image())
        fac = self.write("factory.bin", factory_image())
        with self.assertRaises(Refused) as caught:
            build_release.build(DRYRUN, COMMIT, "v0.0.0-dryrun", app, fac, self.out,
                                os.path.join(REPO, "partitions.csv"), BUILT_AT, build_dir=build_dir)
        self.assertIn("PROJECT_VER", str(caught.exception))


# ------------------------------------------------------------------ signing


class Signing(ReleaseCase):
    def setUp(self):
        super().setUp()
        self.old_env = os.environ.pop(sign_manifest.ENV_VAR, None)
        if self.old_env is not None:
            self.addCleanup(os.environ.__setitem__, sign_manifest.ENV_VAR, self.old_env)

    def test_the_test_key_signs_a_dry_run_and_the_signature_verifies(self):
        self.build(version=DRYRUN)
        line, whose, _ = sign_manifest.sign(os.path.join(self.out, "manifest.json"), True)
        self.assertTrue(line.startswith("ed25519 "))
        self.assertIn("test key", whose)
        self.assertTrue(manifestref.verify([verify_release.test_public_key()], self.manifest(),
                                           line + "\n"))

    def test_the_test_key_refuses_a_release_version(self):
        self.build(version=RELEASE, tag="v0.3.1")
        with self.assertRaises(Refused) as caught:
            sign_manifest.sign(os.path.join(self.out, "manifest.json"), True)
        message = str(caught.exception)
        self.assertIn("refusing to sign", message)
        self.assertIn(sign_manifest.ENV_VAR, message)
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json.sig")))

    def test_the_test_key_refuses_a_release_tag_even_with_a_dry_run_version(self):
        self.build(version=DRYRUN, tag="v0.3.1")
        with self.assertRaises(Refused):
            sign_manifest.sign(os.path.join(self.out, "manifest.json"), True)

    def test_no_key_in_the_environment_fails_with_the_instructions(self):
        self.build(version=RELEASE, tag="v0.3.1")
        with self.assertRaises(Refused) as caught:
            sign_manifest.sign(os.path.join(self.out, "manifest.json"), False)
        message = str(caught.exception)
        self.assertIn(sign_manifest.ENV_VAR, message)
        self.assertIn("manifestref.py keygen", message)
        self.assertIn("keys/release-keys.txt", message)

    def test_the_test_key_in_the_environment_is_refused(self):
        self.build(version=RELEASE, tag="v0.3.1")
        os.environ[sign_manifest.ENV_VAR] = manifestref.TEST_PRIVATE.hex()
        self.addCleanup(os.environ.pop, sign_manifest.ENV_VAR, None)
        with self.assertRaises(Refused) as caught:
            sign_manifest.sign(os.path.join(self.out, "manifest.json"), False)
        self.assertIn("public", str(caught.exception))

    def test_a_key_that_is_not_32_bytes_of_hex_is_refused(self):
        self.build(version=RELEASE, tag="v0.3.1")
        for value, expect in (("not hex", "not hex"), ("00" * 31, "32"), ("00" * 33, "32")):
            os.environ[sign_manifest.ENV_VAR] = value
            self.addCleanup(os.environ.pop, sign_manifest.ENV_VAR, None)
            with self.assertRaises(Refused) as caught:
                sign_manifest.sign(os.path.join(self.out, "manifest.json"), False)
            self.assertIn(expect, str(caught.exception))

    def test_a_real_key_signs_a_release_version(self):
        """Not the release key - a key made here, so that the tagged path is exercised without
        one existing. The signature verifies with its own public half and with nothing else."""
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        key = ed25519.Ed25519PrivateKey.generate()
        private = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                    serialization.NoEncryption())
        public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.build(version=RELEASE, tag="v0.3.1")
        os.environ[sign_manifest.ENV_VAR] = private.hex()
        self.addCleanup(os.environ.pop, sign_manifest.ENV_VAR, None)
        line, whose, _ = sign_manifest.sign(os.path.join(self.out, "manifest.json"), False)
        self.assertIn(sign_manifest.ENV_VAR, whose)
        self.assertIn(manifestref.key_id(public), line)
        self.assertTrue(manifestref.verify([public], self.manifest(), line + "\n"))
        self.assertFalse(manifestref.verify([verify_release.test_public_key()], self.manifest(),
                                            line + "\n"))
        # Nothing the tool returns or writes carries the private key.
        with open(os.path.join(self.out, "manifest.json.sig"), encoding="ascii") as fh:
            self.assertNotIn(private.hex(), fh.read())
        self.assertNotIn(private.hex(), line + whose)

    def test_nothing_printed_carries_the_test_keys_private_hex(self):
        """The dry run's log must not contain it - not because it is a secret, but because a log
        that shows a private key teaches the wrong habit, and the real one looks the same."""
        self.build(version=DRYRUN)
        path = os.path.join(self.out, "manifest.json")
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(sign_manifest.main(["sign_manifest.py", "--test-key", path]), 0)
            self.assertEqual(verify_release.main(["verify_release.py", "--test-key", self.out]), 0)
        printed = out.getvalue() + err.getvalue()
        self.assertNotIn(manifestref.TEST_PRIVATE.hex(), printed)
        self.assertIn(manifestref.key_id(verify_release.test_public_key()), printed)


# ------------------------------------------------------------------ the gate before publishing


class Verifying(ReleaseCase):
    def signed_dry_run(self):
        self.build(version=DRYRUN)
        sign_manifest.sign(os.path.join(self.out, "manifest.json"), True)

    def test_a_signed_dry_run_passes_every_check_of_section_5(self):
        self.signed_dry_run()
        checks, manifest = verify_release.verify(self.out, [verify_release.test_public_key()],
                                                 "the test key")
        self.assertEqual(manifest["version"], DRYRUN)
        text = "\n".join(checks)
        for expected in ("the signature verifies", "the manifest validates", "one image",
                         "SHA256SUMS"):
            self.assertIn(expected, text)
        self.assertEqual(len([c for c in checks if "descriptor" in c]), 2)

    def test_an_empty_trust_table_refuses_the_release_with_the_instructions(self):
        self.signed_dry_run()
        with self.assertRaises(Refused) as caught:
            verify_release.verify(self.out, [], "keys/release-keys.txt (0 keys)")
        message = str(caught.exception)
        self.assertIn("manifestref.py keygen", message)
        self.assertIn("RELEASE_SIGNING_KEY", message)

    def test_the_repositorys_trust_table_parses_and_is_not_the_test_key(self):
        """keys/release-keys.txt is the real file: it must parse through the gate's own loader,
        hold at most the three keys section 3 allows, and never name the specification's public
        test key. Whether it names a key at all is the state of the project, not a rule: it named
        none until the release key was made (2026-10-20, spec/SYSTEM.md section 10 item 1) and a
        tag could not publish then; it names the release key now."""
        keys = verify_release.load_keys(verify_release.KEYS_FILE)
        self.assertLessEqual(len(keys), 3)
        with open(os.path.join(FIXTURES, "index.json"), encoding="utf-8") as fh:
            test_public = bytes.fromhex(json.load(fh)["test_public_key"])
        self.assertNotIn(test_public, keys)

    def test_a_trust_table_line_is_held_to_its_key_id(self):
        # Not the test key: the loader refuses that one by name (task p2-01, the test below).
        public = hashlib.sha256(b"a public key for this test only").digest()
        good = os.path.join(self.dir, "good.txt")
        with open(good, "w", encoding="ascii") as fh:
            fh.write("# a comment\ned25519 %s %s  # a key made for this test\n"
                     % (manifestref.key_id(public), public.hex()))
        self.assertEqual(verify_release.load_keys(good), [public])
        for line, expect in (
            ("ed25519 0000000000000000 %s" % public.hex(), "key id"),
            ("ed25519 %s notthex" % manifestref.key_id(public), "not hex"),
            ("ed25519 %s %s" % (manifestref.key_id(public), public.hex()[:40]), "32 bytes"),
            ("ed448 %s %s" % (manifestref.key_id(public), public.hex()), "expected"),
        ):
            bad = os.path.join(self.dir, "bad.txt")
            with open(bad, "w", encoding="ascii") as fh:
                fh.write(line + "\n")
            with self.assertRaises(Refused) as caught:
                verify_release.load_keys(bad)
            self.assertIn(expect, str(caught.exception))

    def test_the_test_key_in_the_trust_table_is_refused(self):
        """Section 3: the test key is never in the table. The firmware's table is generated through
        this loader (tools/gen_trust_table.py), so this holds for the unit as well as the gate."""
        public = verify_release.test_public_key()
        path = os.path.join(self.dir, "with-test-key.txt")
        with open(path, "w", encoding="ascii") as fh:
            fh.write("ed25519 %s %s\n" % (manifestref.key_id(public), public.hex()))
        with self.assertRaises(Refused) as caught:
            verify_release.load_keys(path)
        message = str(caught.exception)
        self.assertIn("test key", message)
        self.assertIn("TWINOAK_TRUST_TEST_KEY", message)

    def test_more_than_three_keys_is_refused(self):
        path = os.path.join(self.dir, "four.txt")
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        with open(path, "w", encoding="ascii") as fh:
            for _ in range(4):
                public = ed25519.Ed25519PrivateKey.generate().public_key().public_bytes(
                    serialization.Encoding.Raw, serialization.PublicFormat.Raw)
                fh.write("ed25519 %s %s\n" % (manifestref.key_id(public), public.hex()))
        with self.assertRaises(Refused) as caught:
            verify_release.load_keys(path)
        self.assertIn("three", str(caught.exception))

    def test_one_changed_byte_of_the_manifest_fails_the_signature(self):
        self.signed_dry_run()
        path = os.path.join(self.out, "manifest.json")
        with open(path, "rb") as fh:
            data = fh.read()
        with open(path, "wb") as fh:
            fh.write(data.replace(b'"esp32s3"', b'"esp32s2"'))
        with self.assertRaises(Refused) as caught:
            verify_release.verify(self.out, [verify_release.test_public_key()], "the test key")
        self.assertIn("does not verify", str(caught.exception))

    def test_a_signature_by_an_untrusted_key_fails(self):
        self.build(version=DRYRUN)
        other = hashlib.sha256(b"some other key").digest()
        path = os.path.join(self.out, "manifest.json")
        with open(path + ".sig", "w", encoding="ascii", newline="\n") as fh:
            fh.write(manifestref.sign(other, self.manifest()))
        with self.assertRaises(Refused) as caught:
            verify_release.verify(self.out, [verify_release.test_public_key()], "the test key")
        self.assertIn("does not verify", str(caught.exception))

    def test_a_file_changed_after_the_manifest_was_written_fails(self):
        self.signed_dry_run()
        path = os.path.join(self.out, "twinoak-walter-v0.0.0-dryrun.bin")
        with open(path, "r+b") as fh:
            fh.seek(0)
            fh.write(b"\x00")
        with self.assertRaises(Refused) as caught:
            verify_release.verify(self.out, [verify_release.test_public_key()], "the test key")
        self.assertIn("hashes to", str(caught.exception))

    def test_a_manifest_that_is_not_canonical_fails(self):
        """Signed or not, a manifest this repository publishes is written the one way §2 describes
        - two-space indent, LF, one trailing newline, no BOM. A validly signed file in another
        shape means something other than build_release.py wrote it."""
        self.signed_dry_run()
        path = os.path.join(self.out, "manifest.json")
        manifest = json.loads(self.manifest())
        for data in (json.dumps(manifest, indent=4).encode() + b"\n",     # another indent
                     manifestref.canonical(manifest).rstrip(b"\n"),       # no trailing newline
                     b"\xef\xbb\xbf" + manifestref.canonical(manifest)):  # a BOM
            with open(path, "wb") as fh:
                fh.write(data)
            with open(path + ".sig", "w", encoding="ascii", newline="\n") as fh:
                fh.write(manifestref.sign(manifestref.TEST_PRIVATE, data))
            with self.assertRaises(Refused) as caught:
                verify_release.verify(self.out, [verify_release.test_public_key()], "the test key")
            self.assertIn("canonical", str(caught.exception))

    def test_a_manifest_for_the_other_product_fails(self):
        self.signed_dry_run()
        path = os.path.join(self.out, "manifest.json")
        manifest = json.loads(self.manifest())
        manifest["product"] = "twinoak-repeater-firmware"
        data = manifestref.canonical(manifest)
        with open(path, "wb") as fh:
            fh.write(data)
        with open(path + ".sig", "w", encoding="ascii", newline="\n") as fh:
            fh.write(manifestref.sign(manifestref.TEST_PRIVATE, data))
        with self.assertRaises(Refused) as caught:
            verify_release.verify(self.out, [verify_release.test_public_key()], "the test key")
        self.assertIn("product", str(caught.exception))

    def test_a_second_image_in_the_manifest_fails(self):
        self.signed_dry_run()
        path = os.path.join(self.out, "manifest.json")
        manifest = json.loads(self.manifest())
        second = copy.deepcopy(manifest["images"][0])
        second["id"] = "repeater.rak4630-single.868"
        second["role"], second["board"], second["band"] = "repeater", "rak4630-single", "868"
        manifest["images"].append(second)
        data = manifestref.canonical(manifest)
        with open(path, "wb") as fh:
            fh.write(data)
        with open(path + ".sig", "w", encoding="ascii", newline="\n") as fh:
            fh.write(manifestref.sign(manifestref.TEST_PRIVATE, data))
        with self.assertRaises(Refused) as caught:
            verify_release.verify(self.out, [verify_release.test_public_key()], "the test key")
        self.assertIn("exactly one image", str(caught.exception))


# ------------------------------------------------------------------ the boot gate

# Two transcripts, both from the pinned image's QEMU on 11 November 2026 (task p11-04's report):
# the build of this task passing, and **v0.1.3 rebuilt at its own tag** failing - the release that
# was published without ever having started, and whose boot ends where the core dump of
# 9 November says it ended. What the gate reads is a pure function of these two strings, so these
# are the tests of it; the gate's own run in QEMU is tools/verify.sh's release step and the
# release workflow's publish job.
PASSING_CONSOLE = (
    "\ntwinoak-walter 0.1.4+1a2b3c4\n"
    "heap: 33840 bytes free, largest free block 18432 (internal, 8-bit), the main task's "
    "16384-byte stack already taken ('heap')\n")
PASSING_GDB = """\
Breakpoint 1 at 0x4037a08f
Hardware assisted breakpoint 2 at 0x4200cf5f: file /project/main/main.cpp, line 224.

Thread 1 hit Breakpoint 2, app_main () at /project/main/main.cpp:224
GATE-APPMAIN
Hardware assisted breakpoint 3 at 0x420097ab
GATE-HEAP
[Inferior 1 (process 1) detached]
"""
FAILING_GDB = """\
Breakpoint 1 at 0x4037a0bf
Hardware assisted breakpoint 2 at 0x4200d4db: file /project/main/main.cpp, line 222.
GATE-PANIC assert failed: 0x42071b2a <cached disabled>:86 (<cached disabled>)
GATE-WHERE
#0  esp_system_abort (details="assert failed: 0x42071b2a <cached disabled>:86 (<cached disabled>)")\
 at /opt/esp/idf/components/esp_system/port/esp_system_chip.c:87
#1  __assert_func (file=<optimized out>, line=<optimized out>, func=<optimized out>, \
expr=<optimized out>) at /opt/esp/idf/components/newlib/src/assert.c:81
#2  esp_startup_start_app () at /opt/esp/idf/components/freertos/app_startup.c:86
#3  start_cpu0_default () at /opt/esp/idf/components/esp_system/startup.c:222
[Inferior 1 (process 1) detached]
"""
RELEASED_AS = "0.1.4+1a2b3c4"


class TheBootGateReads(unittest.TestCase):
    """tools/release/boot_gate.py's judge(): the three criteria, over transcripts."""

    def judge(self, console, gdb_log, version=RELEASED_AS, expect_heap=True):
        ok, lines = boot_gate.judge(console, gdb_log, version, expect_heap=expect_heap)
        return ok, "\n".join(lines)

    def test_a_boot_that_reached_app_main_said_its_version_and_printed_its_heap_line_passes(self):
        ok, said = self.judge(PASSING_CONSOLE, PASSING_GDB)
        self.assertTrue(ok, said)
        self.assertIn("reached app_main", said)
        self.assertIn("twinoak-walter 0.1.4+1a2b3c4", said)
        self.assertIn("16384-byte stack taken", said)
        self.assertIn("33840 bytes free", said)
        self.assertNotIn("NO    ", said)

    def test_the_boot_v0_1_3_had_is_refused_and_the_reason_names_the_panic_and_where_it_was(self):
        """The whole point of the gate. v0.1.3's boot printed nothing at all, because the panic is
        before app_main; the reason has to carry both the assert and - since QEMU redacts the
        assert's own strings - the frame that asserted."""
        ok, said = self.judge("", FAILING_GDB)
        self.assertFalse(ok)
        self.assertIn("never reached app_main: it panicked first", said)
        self.assertIn("assert failed:", said)
        self.assertIn("in esp_startup_start_app at app_startup.c:86", said)
        # And the other two criteria are refused as well, each in its own words.
        self.assertIn("the banner 'twinoak-walter <version>' was never printed", said)
        self.assertIn("the 'heap:' line was not printed", said)
        self.assertEqual(said.count("NO    "), 3)

    def test_a_boot_that_went_nowhere_without_panicking_says_so(self):
        ok, said = self.judge("", "Breakpoint 1 at 0x4037a08f\nGATE-TIMEOUT after 90 s\n")
        self.assertFalse(ok)
        self.assertIn("did not panic either", said)
        self.assertIn("stuck before the firmware's own code", said)

    def test_a_banner_naming_another_version_is_refused(self):
        """What a release is: the version in the banner is the version being published. A build
        whose PROJECT_VER was not the release's would pass every other check there is."""
        ok, said = self.judge(PASSING_CONSOLE, PASSING_GDB, version="0.1.5+9999999")
        self.assertFalse(ok)
        self.assertIn("the banner says 'twinoak-walter 0.1.4+1a2b3c4' and the version being "
                      "released is 0.1.5+9999999", said)

    def test_an_image_that_started_but_printed_no_heap_line_is_refused(self):
        console = "\ntwinoak-walter 0.1.4+1a2b3c4\nrunning from ota_0 at 0x010000\n"
        ok, said = self.judge(console, PASSING_GDB)
        self.assertFalse(ok)
        self.assertIn("the 'heap:' line was not printed", said)

    def test_a_heap_line_with_no_stack_in_it_is_refused(self):
        console = ("\ntwinoak-walter 0.1.4+1a2b3c4\nheap: 33840 bytes free, largest free block "
                   "18432 (internal, 8-bit), the main task's 0-byte stack already taken\n")
        ok, said = self.judge(console, PASSING_GDB)
        self.assertFalse(ok)
        self.assertIn("the main task's stack is 0 bytes", said)

    def test_an_older_release_is_judged_on_app_main_and_its_banner_alone(self):
        """v0.1.2 has no `heap:` line in it - task p11-02 added one - and it starts perfectly
        well. The gate reads that out of the image's own bytes rather than assuming it."""
        console = "\ntwinoak-walter 0.1.2+5aa5542\nrunning from ota_0 at 0x010000\n"
        ok, said = self.judge(console, PASSING_GDB, version="0.1.2+5aa5542", expect_heap=False)
        self.assertTrue(ok, said)
        self.assertIn("has no 'heap:' line in it at all", said)

    def test_a_panic_after_app_main_is_refused_too(self):
        """A boot that started and then died is not a release either, and the first criterion
        alone would have passed it."""
        ok, said = self.judge(PASSING_CONSOLE, PASSING_GDB + FAILING_GDB)
        self.assertFalse(ok)
        self.assertIn("panicked after app_main", said)

    def test_the_heap_line_is_looked_for_in_the_image_and_not_assumed(self):
        self.assertTrue(boot_gate.HEAP_FORMAT in b"....heap: %lu bytes free, largest free block..")
        self.assertFalse(boot_gate.HEAP_FORMAT in b"twinoak-walter 0.1.2+5aa5542")


class TheBootGateFlashImage(unittest.TestCase):
    """What it gives QEMU: the factory image at 0x0 and an erased chip above it."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="twinoak-boot-gate-test-")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def write(self, data):
        path = os.path.join(self.dir, "factory.bin")
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def test_the_factory_image_is_padded_with_0xff_to_the_whole_flash(self):
        factory = self.write(bytes([boot_gate.IMAGE_MAGIC]) + b"\x01\x02\x03")
        out = os.path.join(self.dir, "flash.bin")
        self.assertEqual(boot_gate.flash_image(factory, out), 4)
        with open(out, "rb") as fh:
            data = fh.read()
        self.assertEqual(len(data), boot_gate.FLASH_BYTES)
        self.assertEqual(data[:4], bytes([boot_gate.IMAGE_MAGIC]) + b"\x01\x02\x03")
        self.assertEqual(set(data[4:]), {boot_gate.ERASED})

    def test_a_file_that_is_not_an_esp_idf_image_is_refused(self):
        factory = self.write(b"not an image")
        with self.assertRaises(boot_gate.Refused) as caught:
            boot_gate.flash_image(factory, os.path.join(self.dir, "flash.bin"))
        self.assertIn("does not begin with 0xE9", str(caught.exception))

    def test_an_image_larger_than_the_flash_is_refused(self):
        factory = self.write(bytes([boot_gate.IMAGE_MAGIC]) * 64)
        with self.assertRaises(boot_gate.Refused) as caught:
            boot_gate.flash_image(factory, os.path.join(self.dir, "flash.bin"), flash_bytes=32)
        self.assertIn("more than the 32 of this board's flash", str(caught.exception))

    def test_an_empty_file_is_refused(self):
        factory = self.write(b"")
        with self.assertRaises(boot_gate.Refused):
            boot_gate.flash_image(factory, os.path.join(self.dir, "flash.bin"))


# ------------------------------------------------------------------ the descriptor, against the spec


class TheDescriptor(unittest.TestCase):
    """The firmware's own builder, parser and finder are tested on the host in C++
    (host_test/core/image/test_descriptor.cpp). Here: that the tooling reads the same fixtures,
    through manifestref rather than through a second implementation."""

    def test_every_valid_fixture_image_holds_one_descriptor_the_index_describes(self):
        with open(os.path.join(FIXTURES, "index.json"), encoding="utf-8") as fh:
            index = json.load(fh)
        seen = 0
        for entry in index["descriptors"]:
            with open(os.path.join(FIXTURES, entry["image"]), "rb") as fh:
                image = fh.read()
            if entry["valid"]:
                offset, fields = manifestref.find_descriptor(image)
                self.assertEqual(offset, entry["offset"])
                self.assertEqual(fields, entry["fields"])
                seen += 1
            else:
                with self.assertRaises(ValueError):
                    manifestref.find_descriptor(image)
                seen += 1
        self.assertEqual(seen, len(index["descriptors"]))
        self.assertGreaterEqual(seen, 4)


if __name__ == "__main__":
    unittest.main(verbosity=2)
