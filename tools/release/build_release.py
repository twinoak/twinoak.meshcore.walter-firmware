#!/usr/bin/env python3
"""Turn a build into the files a release of this firmware consists of.

    python tools/release/build_release.py --version 0.3.1+1a2b3c4 \\
        --commit <40 hex> --build-dir <the idf build directory> [--out out/release]

Writes into --out (spec/FIRMWARE-MANIFEST.md §2):

    twinoak-walter-vX.Y.Z.bin          the application image - `use: ota`, what a running Walter
                                       updates itself with, at the first application slot's offset
    twinoak-walter-vX.Y.Z-factory.bin  bootloader + table + otadata + application as one file,
                                       flashed at 0x0 - `use: factory`, a module out of the box
    manifest.json                      what each of them is, validated before it is written
    SHA256SUMS                         the three of them, for sha256sum -c
    NOTES.md                           the release notes the workflow publishes: the hand-written
                                       release-notes/vX.Y.Z.md of this repository first, then the
                                       generated part (the version, the commit, the esptool line,
                                       what a factory flash keeps)

The signature is the next step (sign_manifest.py) and the check before publishing the one after
(verify_release.py). Nothing here signs, and nothing here publishes.

**spec/tools/manifestref.py is imported, not re-implemented**: the descriptor's bytes, the
manifest's shape, its canonical form and its validation are that file's, so this tooling cannot
drift from the specification. tools/check_partitions.py is imported for the same reason: it owns
the rule that a factory image ends below NVS and the image store.

Every refusal is a sentence on stderr and exit 1. The refusals, each with a test in
tools/release/test_release.py:

  * the factory image reaches NVS, the journal or the image store (it would erase the settings
    and the stored radio images of a unit it is flashed onto);
  * a file holds no descriptor (§4: the image does not say what it is), or more than one;
  * a descriptor disagrees with the manifest entry it is published under;
  * the manifest does not validate (§2);
  * the build is a test-key build (task p2-01): its version says +TESTKEY, or the bytes of the
    specification's public test key are in an image. Such a build trusts a key everybody has,
    and is never released (spec/FIRMWARE-MANIFEST.md §3);
  * a release has no hand-written notes (task p7-01): a version of the shape X.Y.Z+<sha7> needs
    release-notes/vX.Y.Z.md, a few lines a person wrote about what this release is. A tag without
    that file is refused the way a tag without a key is - nobody reads a release whose body is
    only a table.
"""

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_partitions  # noqa: E402  tools/check_partitions.py
import manifestref  # noqa: E402  spec/tools/manifestref.py - the specification as code

# What this firmware is, in the manifest's words (spec/FIRMWARE-MANIFEST.md §2). These are this
# repository's own knowledge of its product, not something read out of the image: the descriptor
# found inside each file is held against them, which is the point of having both.
PRODUCT = "twinoak-walter-firmware"
PROJECT_NAME = "twinoak-walter"  # the ESP-IDF project, so the names of the built files
ROLE = "manager"
BOARD = "walter"
BAND = "lte-m"
MCU = "esp32s3"
LINK = {"major": 1, "minor": 2}  # spec/LINK-PROTOCOL.md is "protocol 1.2"
SLOT_ANY = 0xFF  # the manager's image is not for a radio position (§4)

# A released version: X.Y.Z and the commit it was built from, nothing else. A dry run's
# 0.0.0-dryrun+<sha7> and a local build's 0.0.0-dev are deliberately not of this shape, and
# sign_manifest.py refuses to sign one that is with the specification's test key.
RELEASE_VERSION = re.compile(r"^\d+\.\d+\.\d+\+[0-9a-f]{7,40}$")


class Refused(Exception):
    """Something the release may not contain. The message is for a person reading a workflow log."""


# A test-key build (task p2-01): CMakeLists.txt stamps PROJECT_VER with this when
# TWINOAK_TRUST_TEST_KEY is on, and components/core/image/trust.cpp then trusts the
# specification's public test key.
TEST_KEY_STAMP = "+TESTKEY"


def test_public_key():
    """The specification's public test key, from the fixture index it publishes it in."""
    path = os.path.join(REPO, "spec", "fixtures", "manifest", "index.json")
    with open(path, encoding="utf-8") as fh:
        index = json.load(fh)
    public = bytes.fromhex(index["test_public_key"])
    if manifestref.key_id(public) != index["test_key_id"]:
        raise Refused("%s: test_public_key and test_key_id do not agree" % path)
    return public


def refuse_test_key_build(version, images):
    """A build that trusts the test key accepts anybody's firmware: never released, whatever
    version it claims. Two witnesses: the +TESTKEY stamp in the version, and the key's 32 bytes
    in the image (a normal build has neither - tools/verify.sh holds that on every build)."""
    if TEST_KEY_STAMP in version:
        raise Refused("the version %r is a test-key build's (%s): it trusts the specification's "
                      "public test key and is never released (spec/FIRMWARE-MANIFEST.md section 3). "
                      "Build without -DTWINOAK_TRUST_TEST_KEY=1." % (version, TEST_KEY_STAMP))
    key = test_public_key()
    for label, blob in images:
        at = blob.find(key)
        if at >= 0:
            raise Refused("%s carries the specification's public test key at 0x%X: a test-key "
                          "build, which trusts a key everybody has and is never released "
                          "(spec/FIRMWARE-MANIFEST.md section 3). Build without "
                          "-DTWINOAK_TRUST_TEST_KEY=1." % (label, at))


# ------------------------------------------------------------------ the pieces of the build


def read(path):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise Refused("cannot read %s: %s" % (path, exc))


def app_slot_offset(table_path):
    """The first application slot's offset, from the partition table - not a typed constant, so
    that moving ota_0 in partitions.csv moves the manifest's load_address with it."""
    with open(table_path, encoding="utf-8") as fh:
        parts = check_partitions.parse_csv(fh.read())
    first = check_partitions.APP_SLOTS[0]
    for p in parts:
        if p.name == first:
            return p.offset, {q.name: q for q in parts}
    raise Refused("%s has no %s partition to load the application into" % (table_path, first))


def check_factory_reach(image, by_name, label):
    """tools/check_partitions.py's own rule: a factory image is flashed at 0x0 and must end below
    nvs, journal, rakfw and coredump, so that flashing one onto a unit in service keeps its
    settings and the radio images it has stored (ARCHITECTURE §11)."""
    report = check_partitions.Report()
    check_partitions.check_factory(image, by_name, report, label)
    if report.failures:
        raise Refused("the factory image is refused:\n  " + "\n  ".join(report.failures))
    return [line for line in report.lines if line.startswith("ok ")]


# esp_app_desc_t, the 256 bytes ESP-IDF puts at the start of an application's first segment:
# $IDF_PATH/components/esp_app_format/include/esp_app_desc.h (v5.5.5). The image begins with a
# 24-byte image header and an 8-byte segment header, so the structure is at 0x20, and inside it
# `version` is at 16, `project_name` at 48 and `idf_ver` at 112 - all 32-byte zero-padded ASCII
# except `project_name`'s neighbours, which this does not read.
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432


def esp_app_desc(image, label):
    """What ESP-IDF itself says the image is. A second, independent witness beside the TWOKFWD1
    descriptor: both are written by the same build, from the same PROJECT_VER, by different parts
    of it, so a release in which they disagree is not a release."""
    if len(image) < APP_DESC_OFFSET + 144:
        raise Refused("%s is %d bytes: too short to be an ESP-IDF application image" % (label, len(image)))
    magic, = struct.unpack("<I", image[APP_DESC_OFFSET:APP_DESC_OFFSET + 4])
    if magic != APP_DESC_MAGIC:
        raise Refused("%s has no esp_app_desc_t at 0x%X (magic 0x%08X, expected 0x%08X): it is not "
                      "an ESP-IDF application image" % (label, APP_DESC_OFFSET, magic, APP_DESC_MAGIC))

    def text(at, size):
        return image[APP_DESC_OFFSET + at:APP_DESC_OFFSET + at + size].split(b"\0", 1)[0].decode(
            "ascii", "replace")

    return {"version": text(16, 32), "project_name": text(48, 32), "idf_ver": text(112, 32)}


def hold_app_desc(desc, version, label):
    if desc["project_name"] != PROJECT_NAME:
        raise Refused("%s says it is the project %r, not %r" % (label, desc["project_name"], PROJECT_NAME))
    if desc["version"] != version:
        raise Refused("%s carries the version %r in its esp_app_desc_t and this release is %r "
                      "(build with -DPROJECT_VER=%s)" % (label, desc["version"], version, version))


def the_one_descriptor(image, label):
    """§4: exactly one valid descriptor, on a 4-byte boundary. None means the file does not say
    what it is; two mean it cannot be believed. manifestref.find_descriptor raises on both."""
    try:
        return manifestref.find_descriptor(image)
    except ValueError as exc:
        raise Refused("%s: %s (spec/FIRMWARE-MANIFEST.md section 4 wants exactly one)" % (label, exc))


def hold_against_entry(offset, fields, entry, version, label):
    """Check 5 of §5, applied where the release is made rather than only where it is used: the
    descriptor inside the file and the manifest entry above it say the same thing."""
    wanted = {"role": entry["role"], "board": entry["board"], "band": entry["band"],
              "version": version, "link_major": LINK["major"], "link_minor": LINK["minor"],
              "slot": SLOT_ANY}
    wrong = ["%s is %r in the descriptor and %r in the manifest" % (key, fields.get(key), want)
             for key, want in sorted(wanted.items()) if fields.get(key) != want]
    if wrong:
        raise Refused("%s: the descriptor at 0x%X does not agree with the manifest entry %s:\n  %s"
                      % (label, offset, entry["id"], "\n  ".join(wrong)))


# ------------------------------------------------------------------ what the build says about itself


def build_facts(build_dir, version):
    """ESP-IDF's project_description.json: the version the build was actually given and the ESP-IDF
    it was built with. A build whose PROJECT_VER is not --version cannot be released under it -
    the descriptor inside the image would be checked against the wrong string."""
    facts = {"idf_version": None}
    path = os.path.join(build_dir, "project_description.json")
    if not os.path.isfile(path):
        return facts
    with open(path, encoding="utf-8") as fh:
        description = json.load(fh)
    facts["idf_version"] = description.get("idf_version")
    built = description.get("project_version")
    if built is not None and built != version:
        raise Refused("the build in %s was made with PROJECT_VER %r, and this release is %r: "
                      "build with -DPROJECT_VER=%s (tools/verify.ps1 -Version) or release what was "
                      "built" % (build_dir, built, version, version))
    return facts


def modem_version():
    """The pinned modem library, from dependencies.lock - for the notes."""
    path = os.path.join(REPO, "dependencies.lock")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    match = re.search(r"dptechnics/walter-modem:.*?^\s+version:\s*(\S+)", text, re.S | re.M)
    return match.group(1) if match else None


# ------------------------------------------------------------------ the notes

# The hand-written top of a release's body, one file per tag, kept in the repository so that it is
# reviewed with the code it describes and so that a tag cannot publish a body nobody wrote
# (task p7-01). release-notes/README.md says what belongs in one.
NOTES_DIR = os.path.join(REPO, "release-notes")

NO_HANDWRITTEN_NOTES = """%s is not there, and a release's notes are not only generated.

A release of this firmware is read by a person: what it is, what has and has not been on a board,
and which repeater-firmware release it goes with. That part is hand-written, kept in the
repository beside the code it describes, and put in front of the generated NOTES.md by this
script - so a tag with no such file is refused here, before anything is signed or published, the
way a tag with no release key is.

Write a few lines in %s (release-notes/README.md says what belongs in one),
commit them on the branch that is tagged, and push the tag again."""


def handwritten_notes(tag, is_release, notes_dir=None):
    """The hand-written part of the body for this tag, or None when there is none to find.

    Required for a release (X.Y.Z+<sha7>): that is the refusal above. A dry run and a local
    0.0.0-dev build have no such file and want none - their body is the generated part alone,
    which says in its first paragraph that nothing is published."""
    path = os.path.join(notes_dir or NOTES_DIR, "%s.md" % tag)
    where = os.path.relpath(path, REPO) if path.startswith(REPO) else path
    text = ""
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    if not text.strip():
        if is_release:
            raise Refused(NO_HANDWRITTEN_NOTES % (where, where))
        return None
    return text.strip("\n")


def notes(manifest, files, facts, is_release, handwritten=None):
    version = manifest["version"]
    tag = manifest["tag"]
    factory = next(f for f in files if f["use"] == "factory")
    ota = next(f for f in files if f["use"] == "ota")
    lines = []
    # The hand-written part first, then a rule, then what this script knows: a person reads the
    # top of a release and a machine reads the rest of it.
    if handwritten:
        lines += [handwritten, "", "---", ""]
    lines += [
        "TwinOak Walter firmware %s, built from `%s` and reporting `%s` on its console and in the "
        "box link's HELLO." % (tag, manifest["commit"][:7], version),
        "",
        "| | |",
        "|---|---|",
        "| Version | `%s` |" % version,
        "| Commit | `%s` |" % manifest["commit"],
        "| ESP-IDF | `%s` |" % (facts.get("idf_version") or "see tools/idf-image.txt"),
        "| Modem library | `dptechnics/walter-modem` `%s` |" % (facts.get("modem_version") or "see dependencies.lock"),
        "| Box link | %d.%d (`spec/LINK-PROTOCOL.md`) |" % (manifest["link"]["major"], manifest["link"]["minor"]),
        "",
    ]
    if not is_release:
        lines += ["**This is a dry run.** The files are the workflow's artifacts, signed with the "
                  "specification's public test key, and nothing is published.", ""]
    lines += [
        "## Flashing a module",
        "",
        "**A Walter out of the box takes the factory image** at `0x0`: it carries the bootloader, "
        "the partition table and the OTA data with it. The module's USB-C is the ESP32-S3's own "
        "USB; hold BOOT while plugging in if the port does not appear, and `pip install esptool` "
        "if you have none.",
        "",
        "```",
        "esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 %s"
        % factory["name"],
        "```",
        "",
        "**`%s` beside it is the application image alone** (`%s`), which is what a Walter in "
        "service downloads and writes into its other application slot; it is not for esptool."
        % (ota["name"], ota["load_address"]),
        "",
        "**A factory flash keeps a unit's settings and the radio images it has stored.** This "
        "firmware's partition table puts `nvs` (the settings), `journal` and `rakfw` (the store "
        "holding each radio's golden, candidate and scanner image) *above* the two application "
        "slots, and a factory image ends where the application ends - so it cannot reach them. "
        "`tools/check_partitions.py` holds that on every build and `tools/release/build_release.py` "
        "refuses to publish a factory image that reaches any of them, so this is a property of the "
        "release and not a promise: this file was made only because the check passed.",
        "",
        "## What is signed, and how to check it",
        "",
        "`manifest.json` lists every file above with its size and SHA-256, and "
        "`manifest.json.sig` is one Ed25519 signature over its bytes "
        "(`spec/FIRMWARE-MANIFEST.md` §3). One signature therefore covers the whole release. "
        "A Walter checks the signature, then the manifest, then each file's hash, then the "
        "descriptor inside the file, before it stores anything (§5).",
        "",
        "```",
        "python spec/tools/manifestref.py verify <public key hex> manifest.json",
        "sha256sum -c SHA256SUMS",
        "```",
        "",
    ]
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------ the release


def build(version, commit, tag, app_path, factory_path, out_dir, table_path, built_at,
          build_dir=None, require_descriptor=True, notes_dir=None):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise Refused("--commit must be 40 lowercase hex characters, not %r" % commit)
    if not version:
        raise Refused("--version is required")

    # Before a byte is read: a release needs the notes a person wrote for it (task p7-01).
    is_release = bool(RELEASE_VERSION.fullmatch(version))
    handwritten = handwritten_notes(tag, is_release, notes_dir)

    load_address, by_name = app_slot_offset(table_path)
    app = read(app_path)
    factory = read(factory_path)

    # A test-key build is refused before anything else is believed about it (task p2-01).
    refuse_test_key_build(version, [("the application image", app), ("the factory image", factory)])

    # ESP-IDF's own word on the application, before anything else is believed about it.
    desc = esp_app_desc(app, "the application image")
    hold_app_desc(desc, version, "the application image")

    facts = {"modem_version": modem_version(), "idf_version": desc["idf_ver"]}
    if build_dir:
        facts.update({k: v for k, v in build_facts(build_dir, version).items() if v})

    evidence = check_factory_reach(factory, by_name, "the factory image")
    # The factory image is the same application with a bootloader, a table and OTA data in front
    # of it. Byte for byte, at the slot the manifest gives as the application's load address -
    # so the two files of one release cannot be from two builds.
    tail = factory[load_address:load_address + len(app)]
    if tail != app:
        at = next((i for i in range(min(len(tail), len(app))) if tail[i] != app[i]), len(tail))
        raise Refused("the factory image does not carry this application at 0x%X: they differ at "
                      "byte %d of %d. The two files of a release are made by one build."
                      % (load_address, at, len(app)))
    evidence.append("ok    the factory image carries this very application at 0x%X, all %d bytes"
                    % (load_address, len(app)))

    names = {"ota": "%s-%s.bin" % (PROJECT_NAME, tag), "factory": "%s-%s-factory.bin" % (PROJECT_NAME, tag)}
    files = [
        manifestref.file_entry("ota", names["ota"], app, load_address),
        manifestref.file_entry("factory", names["factory"], factory, 0x0),
    ]
    image = {
        "id": "%s.%s.%s" % (ROLE, BOARD, BAND),
        "role": ROLE, "board": BOARD, "band": BAND, "mcu": MCU,
        "requires": {"link_major": LINK["major"]},
        "files": files,
    }
    manifest = {
        "schema": manifestref.SCHEMA,
        "product": PRODUCT,
        "version": version,
        "tag": tag,
        "commit": commit,
        "built_at": built_at,
        "link": dict(LINK),
        "images": [image],
    }

    # The descriptor inside each file, against the entry above it (§5 check 5). Both files carry
    # the same application, so both must carry the same descriptor.
    descriptors = {}
    if require_descriptor:
        for use, blob in (("ota", app), ("factory", factory)):
            offset, fields = the_one_descriptor(blob, names[use])
            hold_against_entry(offset, fields, image, version, names[use])
            descriptors[use] = (offset, fields)

    try:
        manifestref.validate(manifest)
    except ValueError as exc:
        raise Refused("the manifest does not validate (spec/FIRMWARE-MANIFEST.md section 2): %s" % exc)

    os.makedirs(out_dir, exist_ok=True)
    written = []

    def write(name, data):
        path = os.path.join(out_dir, name)
        with open(path, "wb") as fh:
            fh.write(data)
        written.append(name)
        return path

    write(names["ota"], app)
    write(names["factory"], factory)
    write("manifest.json", manifestref.canonical(manifest))
    sums = "".join("%s  %s\n" % (hashlib.sha256(read(os.path.join(out_dir, n))).hexdigest(), n)
                   for n in (names["ota"], names["factory"], "manifest.json"))
    write("SHA256SUMS", sums.encode("ascii"))
    write("NOTES.md", notes(manifest, files, facts, is_release, handwritten).encode("utf-8"))

    return {"manifest": manifest, "names": names, "written": written, "descriptors": descriptors,
            "evidence": evidence, "facts": facts, "load_address": load_address,
            "handwritten": handwritten}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True, help="X.Y.Z+<sha7>, the string the firmware reports")
    ap.add_argument("--commit", required=True, help="40 hex characters")
    ap.add_argument("--tag", default=None, help="vX.Y.Z; by default v<version up to the +>")
    ap.add_argument("--build-dir", default=None, help="the ESP-IDF build directory")
    ap.add_argument("--app", default=None, help="the application image; by default <build-dir>/%s.bin" % PROJECT_NAME)
    ap.add_argument("--factory", default=None,
                    help="the merged image; by default <build-dir>/%s-factory.bin" % PROJECT_NAME)
    ap.add_argument("--out", default=os.path.join(REPO, "out", "release"))
    ap.add_argument("--partitions", default=os.path.join(REPO, "partitions.csv"))
    ap.add_argument("--built-at", default=None, help="UTC YYYY-MM-DDTHH:MM:SSZ; by default now")
    ap.add_argument("--require-descriptor", action="store_true", default=True,
                    help="refuse a file without exactly one descriptor that agrees with the "
                         "manifest. On from the start in this repository; there is no way to turn "
                         "it off")
    args = ap.parse_args(argv[1:])

    tag = args.tag or "v" + args.version.split("+", 1)[0]
    app = args.app or (os.path.join(args.build_dir, PROJECT_NAME + ".bin") if args.build_dir else None)
    factory = args.factory or (os.path.join(args.build_dir, PROJECT_NAME + "-factory.bin")
                               if args.build_dir else None)
    if not app or not factory:
        print("give --build-dir, or both --app and --factory", file=sys.stderr)
        return 2
    built_at = args.built_at or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    try:
        result = build(args.version, args.commit, tag, app, factory, args.out, args.partitions,
                       built_at, build_dir=args.build_dir, require_descriptor=args.require_descriptor)
    except Refused as exc:
        print("REFUSED: %s" % exc, file=sys.stderr)
        return 1

    for line in result["evidence"]:
        print(line)
    for use, (offset, fields) in sorted(result["descriptors"].items()):
        print("ok    %s holds one descriptor at 0x%X: role %s, board %s, band %s, version %s, link %d.%d"
              % (result["names"][use], offset, fields["role"], fields["board"], fields["band"],
                 fields["version"], fields["link_major"], fields["link_minor"]))
    print("ok    the manifest validates against spec/FIRMWARE-MANIFEST.md section 2")
    if result["handwritten"]:
        print("ok    NOTES.md begins with release-notes/%s.md, %d hand-written lines"
              % (tag, len(result["handwritten"].splitlines())))
    else:
        print("      NOTES.md is the generated part alone: %s is not a release, so it needs no "
              "hand-written notes" % args.version)
    print("%s: %s" % (args.out, ", ".join(result["written"])))
    print("the application image loads at 0x%08X (%s, from %s)"
          % (result["load_address"], check_partitions.APP_SLOTS[0], os.path.relpath(args.partitions, REPO)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
