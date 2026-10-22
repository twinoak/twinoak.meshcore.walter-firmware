#!/usr/bin/env python3
"""walterctl - the laptop's end of the Walter's console (task p2-02).

Today it does one thing: push a signed release's image into the Walter's image store over USB,
which is the bench's stand-in for a server until phase 3 gives the Walter an uplink.

    walterctl.py push --port COM7 --as candidate <release-dir> [--image <id>] [--slot 0]
    walterctl.py push --console ./walter_console --as candidate <release-dir>
    walterctl.py push --port COM7 --as base <release-dir>

A <release-dir> is what tools/release/build_release.py writes and a GitHub Release carries:
`manifest.json`, `manifest.json.sig` and the image files the manifest names. The three things that
go over the wire are the manifest, its signature and the one file of the chosen image: its `swd`
file for a radio's image, its `ota` file for the Walter's own, and - with `--as base` - its
`provision` file, the board's base (spec/FIRMWARE-MANIFEST.md section 4a, SYSTEM.md S-5a). A base
is stored and nothing more: writing one to a radio is `rak provision` on the console, typed by a
person.

**Nothing here is trusted by the Walter.** The six checks of spec/FIRMWARE-MANIFEST.md section 5
run on the Walter, over the bytes as they land in its flash; this program only delivers them, and
it reads the manifest itself solely to know which file to send and how long it is.

The framing is the box link's (spec/LINK-PROTOCOL.md section 2) through `spec/tools/linkref.py` -
the specification's own reference code, so that the laptop and the firmware cannot disagree about
a CRC or a COBS block. The transfer's message types are core/image/receive.h's.

`--console <command>` runs a program and talks to its standard input and output instead of a
serial port: that is how this tool is tested before a board exists (host_test/console_host/,
tools/test_walterctl.py). `--port` needs pyserial (pinned in tools/requirements-verify.txt).

This end is strictly lock-step: one chunk, one acknowledgement, and the same chunk again if none
comes. The Walter tolerates more than that - a duplicate, a gap, a chunk out of order - and says
which chunk it wants; being simple here means that whatever it says, this program is right to
send that one again.
"""

import argparse
import hashlib
import json
import os
import queue
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))

import linkref  # noqa: E402  the specification's own framing

# core/image/receive.h's numbers. A response is the request's type with bit 7 set (section 3).
T_HELLO = 0x01
T_FILE = 0x02
T_DATA = 0x03
T_END = 0x04
T_COMMIT = 0x05
T_ABORT = 0x06
RESPONSE_BIT = 0x80

KIND_MANIFEST = 1
KIND_SIGNATURE = 2
KIND_IMAGE = 3

ST_OK = 0
ST_RESEND = 1
ST_REFUSED = 2
ST_DONE = 3

STATUS_NAMES = {ST_OK: "ok", ST_RESEND: "resend", ST_REFUSED: "refused", ST_DONE: "done"}

CHUNK = 240          # the link's largest payload (section 2)
TRIES = 4            # how often one frame is sent before giving up
REPLY_TIMEOUT = 5.0  # seconds to wait for one acknowledgement
HELLO_TIMEOUT = 2.0

# The three images of a radio slot, the slot's base, and `self`: the Walter's own `ota` image into its other
# application slot (`self receive`, task p2-04).
WHICH = ("golden", "candidate", "scanner", "base", "self")

# What `--as` takes out of the release: the manifest `use` of the file to send.
USE_OF = {"golden": "swd", "candidate": "swd", "scanner": "swd", "base": "provision",
          "self": "ota"}
PROTOCOL = "walter image receive 1"


class Fault(Exception):
    """Something the Walter said, or something this program will not do."""


# ----------------------------------------------------------------------------- the two transports


class SerialPort:
    def __init__(self, port, baud=115200):
        try:
            import serial  # pyserial
        except ImportError:
            raise Fault("--port needs pyserial: pip install pyserial==3.5")
        self.port = serial.Serial(port, baud, timeout=0.1)
        self.name = port

    def write(self, data):
        self.port.write(data)
        self.port.flush()

    def read(self, timeout):
        self.port.timeout = timeout
        return self.port.read(4096) or b""

    def close(self):
        self.port.close()


class ConsoleProcess:
    """A program on the other end of a pipe: the console host of host_test/console_host/.

    A pipe has no read timeout, and a program that has stopped answering must not hang this one,
    so a thread does the reading and the caller takes what has arrived."""

    def __init__(self, command):
        self.name = " ".join(command)
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        bufsize=0)
        self.queue = queue.Queue()
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()

    def _pump(self):
        while True:
            data = self.process.stdout.read(1)
            if not data:
                self.queue.put(b"")  # the far end is gone
                return
            self.queue.put(data)

    def write(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def read(self, timeout):
        try:
            first = self.queue.get(timeout=max(0.01, timeout))
        except queue.Empty:
            return b""
        out = bytearray(first)
        while True:  # whatever else has already arrived
            try:
                out += self.queue.get_nowait()
            except queue.Empty:
                return bytes(out)

    def close(self):
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()


# ----------------------------------------------------------------------------- the transfer


class Transfer:
    def __init__(self, transport, verbose=True):
        self.transport = transport
        self.verbose = verbose
        self.buffer = bytearray()
        self.text = bytearray()  # bytes that were not frames: the console's own lines

    def say(self, line):
        if self.verbose:
            print(line, flush=True)

    def send_line(self, line):
        self.transport.write((line + "\n").encode("ascii"))

    def frames(self, deadline):
        """Every complete frame that arrives before the deadline, decoded. Bytes that are not a
        frame - the console's prompt, its ready line, noise - are kept for a message and ignored."""
        out = []
        while True:
            while 0 in self.buffer:
                end = self.buffer.index(0)
                piece = bytes(self.buffer[:end])
                del self.buffer[:end + 1]
                if not piece:
                    continue  # the 0x00 a sender puts in front of a frame (section 2)
                try:
                    out.append(linkref.parse_frame(piece + b"\x00"))
                except ValueError:
                    self.text += piece
            if out or time.monotonic() >= deadline:
                return out
            data = self.transport.read(max(0.05, deadline - time.monotonic()))
            if not data:
                if time.monotonic() >= deadline:
                    return out
                continue
            self.buffer += data

    def request(self, msg_type, seq, payload=b"", timeout=REPLY_TIMEOUT, tries=TRIES):
        """One frame, and the response to it. Retries while nothing comes back."""
        for attempt in range(tries):
            # The one 0x00 a sender puts in front of a frame (section 2): whatever the console had
            # half-collected - a character a person typed, noise on the line - ends as an empty
            # frame, which a receiver ignores, rather than as the start of this one.
            self.transport.write(b"\x00" + linkref.make_frame(msg_type, seq, payload))
            deadline = time.monotonic() + timeout
            while True:
                got = self.frames(deadline)
                if not got:
                    break
                for (rtype, rseq, body) in got:
                    if rtype != (msg_type | RESPONSE_BIT) or rseq != seq:
                        continue  # not ours: an answer to something already given up on
                    if len(body) < 6:
                        raise Fault("a response shorter than its header")
                    status = body[0]
                    next_seq = body[1]
                    committed = int.from_bytes(body[2:6], "little")
                    text = body[6:].decode("utf-8", "replace")
                    return status, next_seq, committed, text
            if attempt + 1 < tries:
                self.say("  no answer to type 0x%02x seq %d; sending it again" % (msg_type, seq))
        raise Fault("no answer to %d attempts: the Walter is not speaking this protocol "
                    "(is `image receive` running?)" % tries)

    def expect_ok(self, msg_type, seq, payload=b"", what=""):
        status, next_seq, committed, text = self.request(msg_type, seq, payload)
        if status == ST_REFUSED:
            raise Fault("the Walter refused %s: %s" % (what or ("type 0x%02x" % msg_type), text))
        if status not in (ST_OK, ST_DONE):
            raise Fault("the Walter answered %s to %s: %s"
                        % (STATUS_NAMES.get(status, status), what or "a frame", text))
        return status, next_seq, committed, text

    def hello(self):
        status, _, _, text = self.request(T_HELLO, 0, timeout=HELLO_TIMEOUT)
        if status != ST_OK:
            raise Fault("the Walter would not say hello: %s" % text)
        if not text.startswith(PROTOCOL):
            raise Fault("the Walter speaks another version of this transfer: %r (this tool "
                        "speaks %r)" % (text, PROTOCOL))
        return text

    def file_header(self, kind, size):
        return bytes([kind]) + size.to_bytes(4, "little")

    def send_file(self, kind, data, name, resume_hint=False):
        """One file. Returns how many bytes were sent (which is fewer than its length when the
        Walter already had some of it)."""
        _, _, committed, text = self.expect_ok(T_FILE, 0, self.file_header(kind, len(data)),
                                               "the start of %s" % name)
        start = 0
        if resume_hint and committed:
            if committed > len(data):
                raise Fault("the Walter says it has %d bytes of a %d-byte file"
                            % (committed, len(data)))
            start = committed
            self.say("  %s: the Walter already has %d of %d bytes; sending the rest"
                     % (name, start, len(data)))
        seq = 0
        at = start
        while at < len(data):
            piece = data[at:at + CHUNK]
            status, next_seq, _, reply = self.request(T_DATA, seq, piece)
            if status == ST_REFUSED:
                raise Fault("the Walter refused %s at byte %d: %s" % (name, at, reply))
            if status == ST_RESEND:
                # It wants another chunk than the one just sent. Lock-step means that can only be
                # the one it is waiting for, so say what happened and send that one.
                self.say("  %s: the Walter wants chunk %d, not %d: %s"
                         % (name, next_seq, seq, reply))
                if next_seq != seq:
                    raise Fault("the Walter is waiting for chunk %d and this tool is at %d; "
                                "start the push again" % (next_seq, seq))
                continue
            at += len(piece)
            seq = (seq + 1) & 0xFF
        self.expect_ok(T_END, seq, b"", "the end of %s" % name)
        return len(data) - start


# ----------------------------------------------------------------------------- the release


def read_release(directory, image_id=None, use="swd"):
    """The manifest's bytes, its signature line and the file of `use` (swd for a radio's image,
    provision for its base, ota for the Walter's own) of the chosen image."""
    manifest_path = os.path.join(directory, "manifest.json")
    sig_path = manifest_path + ".sig"
    for path in (manifest_path, sig_path):
        if not os.path.isfile(path):
            raise Fault("%s is not in %s: that is not a release directory" %
                        (os.path.basename(path), directory))
    with open(manifest_path, "rb") as fh:
        manifest_bytes = fh.read()
    with open(sig_path, "rb") as fh:
        signature = fh.read()
    manifest = json.loads(manifest_bytes.decode("utf-8"))

    choices = []
    for image in manifest.get("images", []):
        for entry in image.get("files", []):
            if entry.get("use") == use:
                choices.append((image, entry))
    if not choices and use == "ota":
        raise Fault("no image in this manifest has an `ota` file: there is no Walter image here "
                    "(a radio's `swd` file goes with --as golden|candidate|scanner)")
    if not choices and use == "provision":
        raise Fault("no image in this manifest has a `provision` file: this release carries no "
                    "base for a board (FIRMWARE-MANIFEST.md section 4a)")
    if not choices:
        raise Fault("no image in this manifest has an `swd` file: there is nothing a Walter can "
                    "flash over SWD here (an `ota` file is the Walter's own update: --as self)")
    if image_id is not None:
        choices = [c for c in choices if c[0].get("id") == image_id]
        if not choices:
            raise Fault("no image with id %r in this manifest" % image_id)
    if len(choices) > 1:
        ids = ", ".join(c[0].get("id", "?") for c in choices)
        raise Fault("this manifest has more than one image with an %s file (%s); say which with "
                    "--image" % (use, ids))
    image, entry = choices[0]

    name = entry.get("name")
    if not isinstance(name, str) or "/" in name or "\\" in name:
        raise Fault("the manifest's file name is not a plain name: %r" % name)
    path = os.path.join(directory, name)
    if not os.path.isfile(path):
        raise Fault("%s, which the manifest names, is not in %s" % (name, directory))
    with open(path, "rb") as fh:
        data = fh.read()
    # Not a check the Walter relies on - it hashes what lands in its own flash - but a push that
    # would be refused at check 4 is better stopped here, where the message can name the file.
    if len(data) != entry.get("size"):
        raise Fault("%s is %d bytes and the manifest says %s" % (name, len(data), entry.get("size")))
    digest = hashlib.sha256(data).hexdigest()
    if digest != entry.get("sha256"):
        raise Fault("%s does not have the SHA-256 the manifest gives (%s, not %s)"
                    % (name, digest, entry.get("sha256")))
    return manifest, manifest_bytes, signature, image, entry, data


# ----------------------------------------------------------------------------- push


def push(args):
    use = USE_OF[args.which]
    manifest, manifest_bytes, signature, image, entry, data = read_release(args.release, args.image, use)
    print("release %s %s, %s: %s, %d bytes, sha256 %s"
          % (manifest.get("product"), manifest.get("version"), image.get("id"),
             entry.get("name"), len(data), entry.get("sha256")[:16] + "..."))

    if args.console:
        transport = ConsoleProcess([args.console])
    else:
        transport = SerialPort(args.port)
    transfer = Transfer(transport, verbose=not args.quiet)
    try:
        if args.which == "self":
            line = "self receive"
        else:
            line = "image receive %s" % args.which
            if args.slot:
                line += " @%d" % args.slot
        transfer.send_line(line)
        transfer.say("sent: %s" % line)
        hello = transfer.hello()
        transfer.say("the Walter: %s" % hello)

        transfer.send_file(KIND_MANIFEST, manifest_bytes, "manifest.json")
        transfer.send_file(KIND_SIGNATURE, signature, "manifest.json.sig")
        sent = transfer.send_file(KIND_IMAGE, data, entry["name"], resume_hint=True)

        status, _, _, text = commit(transfer)
        if status != ST_DONE:
            raise Fault("the Walter did not store it: %s" % text)
        if args.which == "self":
            print("stored in the Walter's other application slot: %d of %d bytes sent, %s"
                  % (sent, len(data), text))
            print("`self status` on the console shows it; `self switch` makes it the next to boot")
            return 0
        print("stored as the %s%s: %d of %d bytes sent, %s"
              % ("base" if args.which == "base" else "%s image" % args.which,
                 " of slot %d" % args.slot if args.slot else "", sent, len(data), text))
        print("`image list` on the console shows it; `image verify %s` reads it back out of flash"
              % args.which)
        if args.which == "base":
            print("nothing is written to a radio by this: that is `rak provision`, typed by a person")
        return 0
    except Fault as fault:
        print("walterctl: %s" % fault, file=sys.stderr)
        if transfer.text:
            tail = bytes(transfer.text[-400:]).decode("utf-8", "replace")
            print("the console also said: %s" % tail.strip(), file=sys.stderr)
        try:
            transfer.request(T_ABORT, 0, timeout=1.0, tries=1)
        except Fault:
            pass
        return 1
    finally:
        transport.close()


def commit(transfer):
    """The commit, whose answer is the verdict: the six checks have run over the bytes in flash."""
    transfer.say("all three files are in; the Walter is checking them")
    return transfer.expect_ok(T_COMMIT, 0, b"", "the commit")


def main(argv):
    parser = argparse.ArgumentParser(prog="walterctl.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("push", help="send a release's image into the Walter's image store")
    p.add_argument("release", help="a release directory: manifest.json, its .sig, the image files")
    p.add_argument("--as", dest="which", required=True, choices=WHICH,
                   help="which of the slot's three images to store it as; `base` for the board's "
                        "provision file; `self` for the Walter's own ota image, into its other "
                        "application slot")
    p.add_argument("--image", help="the manifest image's id, when it has more than one file of the use")
    p.add_argument("--slot", type=int, default=0, help="the radio's position (0 today)")
    where = p.add_mutually_exclusive_group(required=True)
    where.add_argument("--port", help="the Walter's USB serial port (COM7, /dev/ttyACM0)")
    where.add_argument("--console",
                       help="a program to talk to over its standard input and output instead of a "
                            "serial port (host_test/console_host/, for testing this tool)")
    p.add_argument("--quiet", action="store_true", help="only the result")
    args = parser.parse_args(argv[1:])
    if args.command == "push":
        try:
            return push(args)
        except Fault as fault:
            print("walterctl: %s" % fault, file=sys.stderr)
            return 1
    parser.error("unknown command")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
