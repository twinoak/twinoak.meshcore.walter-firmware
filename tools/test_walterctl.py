#!/usr/bin/env python3
"""tools/walterctl.py against the console behind a pipe (task p2-02's fourth acceptance
criterion), so that the laptop's end of the transfer is tested before a board exists.

    python3 tools/test_walterctl.py --console <path to walter_console>

`walter_console` is host_test/console_host/: the firmware's own console and image store over a
simulated flash, trusting the specification's public test key. host_test/CMakeLists.txt builds it
and runs this file as a ctest, so `tools/verify.ps1 -Only host-tests` covers it.

The release pushed is made here out of `spec/fixtures/manifest/`: the specification's repeater
manifest, its signature by the test key, and the image file under the name the manifest gives it -
which is what a real release directory looks like.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FIXTURES = os.path.join(REPO, "spec", "fixtures", "manifest")
WALTERCTL = os.path.join(HERE, "walterctl.py")

failures = []
checks = 0


def check(condition, what):
    global checks
    checks += 1
    if not condition:
        failures.append(what)
        print("FAIL  %s" % what)
    else:
        print("ok    %s" % what)


def make_release(directory, manifest_name="manifest-repeater.json",
                 sig_name="manifest-repeater.json.sig", image="image-repeater.bin",
                 mangle=None):
    """A release directory out of the spec's fixtures: manifest.json, its .sig, and the image
    under the name the manifest gives. `mangle` may change the image's bytes."""
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(FIXTURES, manifest_name), "rb") as fh:
        manifest_bytes = fh.read()
    with open(os.path.join(directory, "manifest.json"), "wb") as fh:
        fh.write(manifest_bytes)
    shutil.copyfile(os.path.join(FIXTURES, sig_name),
                    os.path.join(directory, "manifest.json.sig"))
    manifest = json.loads(manifest_bytes.decode("utf-8"))
    entry = None
    for img in manifest["images"]:
        for f in img["files"]:
            if f["use"] == "swd":
                entry = f
    assert entry is not None, "the fixture manifest has an swd file"
    with open(os.path.join(FIXTURES, image), "rb") as fh:
        data = fh.read()
    if mangle is not None:
        data = mangle(data)
    with open(os.path.join(directory, entry["name"]), "wb") as fh:
        fh.write(data)
    return manifest, entry


def run_push(console, release, which="candidate", extra=()):
    command = [sys.executable, WALTERCTL, "push", release, "--as", which]
    command += list(extra)
    command += ["--console", console]
    done = subprocess.run(command, capture_output=True, text=True, timeout=300)
    return done


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--console", required=True, help="the console host binary")
    parser.add_argument("--generated", help="host_test's generated manifest fixtures (the Walter's own images)")
    args = parser.parse_args(argv[1:])
    if not os.path.isfile(args.console):
        print("no console host at %s" % args.console)
        return 2

    with tempfile.TemporaryDirectory() as work:
        # 1. A good push: the image becomes the candidate.
        release = os.path.join(work, "release")
        manifest, entry = make_release(release)
        done = run_push(args.console, release)
        print(done.stdout)
        if done.returncode != 0:
            print(done.stderr)
        check(done.returncode == 0, "walterctl push --as candidate against the console: exit 0")
        check("stored as the candidate image" in done.stdout, "it says the image was stored")
        check(entry["name"] in done.stdout, "it names the file it sent")
        check(str(entry["size"]) in done.stdout, "it says how many bytes")

        # 2. The golden image of a fresh unit: allowed once (a unit's first provisioning).
        done = run_push(args.console, release, which="golden")
        check(done.returncode == 0, "walterctl push --as golden on an empty slot: exit 0")

        # 3. A file whose bytes are not the manifest's: refused by this tool before anything is
        #    sent, because the message can name the file here.
        bad = os.path.join(work, "bad")
        make_release(bad, mangle=lambda data: data[:100] + bytes([data[100] ^ 0x20]) + data[101:])
        done = run_push(args.console, bad)
        check(done.returncode == 1, "a file that is not the manifest's hash: exit 1")
        check("does not have the SHA-256 the manifest gives" in done.stderr,
              "and it says which file and which hash")

        # 4. A manifest the Walter will not accept: the walter manifest has no swd file at all
        #    (it is the Walter's own `ota` image), which is this tool's own refusal.
        walter = os.path.join(work, "walter")
        os.makedirs(walter)
        shutil.copyfile(os.path.join(FIXTURES, "manifest-walter.json"),
                        os.path.join(walter, "manifest.json"))
        shutil.copyfile(os.path.join(FIXTURES, "manifest-walter.json.sig"),
                        os.path.join(walter, "manifest.json.sig"))
        done = run_push(args.console, walter)
        check(done.returncode == 1, "a manifest with no swd file: exit 1")
        check("no image in this manifest has an `swd` file" in done.stderr, "and it says why")

        # 5. A real signature by a key the Walter does not hold: refused at check 1, and this
        #    tool reports what the Walter said rather than deciding for itself.
        unknown = os.path.join(work, "unknown-key")
        make_release(unknown, sig_name="manifest-repeater-unknown-key.json.sig")
        done = run_push(args.console, unknown)
        check(done.returncode == 1, "a manifest signed by a key this build does not hold: exit 1")
        check("check 1, the signature" in done.stderr,
              "and the refusal is the Walter's own words, at check 1")

        # 6. A directory that is not a release.
        done = run_push(args.console, work)
        check(done.returncode == 1, "a directory with no manifest.json: exit 1")
        check("that is not a release directory" in done.stderr, "and it says so")

        # 7. The Walter's own image (task p2-04): --as self takes the manifest's `ota` file and
        #    says `self receive`. The spec's walter fixture loads at 0x20000, which is not one of
        #    this firmware's application slots, so the Walter refuses it and this tool says so in
        #    the Walter's words.
        shutil.copyfile(os.path.join(FIXTURES, "image-walter-ota.bin"),
                        os.path.join(walter, "twinoak-walter-v0.3.0.bin"))
        done = run_push(args.console, walter, which="self")
        print(done.stdout)
        print(done.stderr)
        check(done.returncode == 1, "the spec's walter fixture --as self: refused, exit 1")
        check("the file's load_address is not an application slot's offset" in done.stderr,
              "and the refusal is the Walter's own: the load_address")
        # And a radio release --as self: this tool's own refusal, no ota file.
        done = run_push(args.console, release, which="self")
        check(done.returncode == 1, "a repeater release --as self: exit 1")
        check("no image in this manifest has an `ota` file" in done.stderr, "and it says why")

        # 8. A Walter image that fits: the generated fixture self-a, loading at ota_0's offset.
        if args.generated:
            own = os.path.join(work, "self")
            os.makedirs(own)
            with open(os.path.join(args.generated, "self-a.json"), "rb") as fh:
                own_manifest = fh.read()
            with open(os.path.join(own, "manifest.json"), "wb") as fh:
                fh.write(own_manifest)
            shutil.copyfile(os.path.join(args.generated, "self-a.json.sig"), os.path.join(own, "manifest.json.sig"))
            ota = next(f for img in json.loads(own_manifest.decode("utf-8"))["images"] for f in img["files"]
                       if f["use"] == "ota")
            shutil.copyfile(os.path.join(args.generated, "self-a.bin"), os.path.join(own, ota["name"]))
            done = run_push(args.console, own, which="self")
            print(done.stdout)
            if done.returncode != 0:
                print(done.stderr)
            check(done.returncode == 0, "walterctl push --as self against the console: exit 0")
            check("stored in the Walter's other application slot" in done.stdout, "it says where it went")
            check("`self switch` makes it the next to boot" in done.stdout, "and what comes next")

    print("\n%d checks, %d failed" % (checks, len(failures)))
    for failure in failures:
        print("  FAILED: %s" % failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
