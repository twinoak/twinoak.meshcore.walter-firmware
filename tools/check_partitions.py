#!/usr/bin/env python3
"""Hold partitions.csv to docs/ARCHITECTURE.md section 11, and the build to partitions.csv.

    python tools/check_partitions.py --self-test
    python tools/check_partitions.py --table partitions.csv --flash-size 16MB
        [--sdkconfig <build>/sdkconfig]                       flash size and table offset from the build
        [--bin <build>/partition_table/partition-table.bin]   the table the build made
        [--app <build>/twinoak-walter.bin]                    the application image, against its slot
        [--factory <merged image>]                            the factory image, against what is above the slots

Every constraint is an assertion with a sentence that names the partition. Standard library only;
run by tools/verify.sh as step 5 (after the build) and before that with --self-test, which must
refuse the two tables in tools/partitions-selftest/ and accept the real one. Exit 0 when every
check passed, 1 when one did not, 2 on a usage error.
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# ---------------------------------------------------------------- what the architecture asks for

APP_SLOTS = ("ota_0", "ota_1")       # two application slots, no factory (S-6: A/B with rollback)
MIN_SLOT = 0x300000                  # "room to grow (3 MB each)" - section 11
TABLE_OFFSET = 0x8000                # CONFIG_PARTITION_TABLE_OFFSET; the table is one 4 KB sector
TABLE_END = TABLE_OFFSET + 0x1000
GAP = (0x9000, 0xE000)               # the span a factory image fills with 0xFF; nothing lives here
OTADATA_SIZE = 0x2000                # two sectors, ESP-IDF's requirement
RADIO_IMAGE_MAX = 0x100000           # an nRF52840 has 1 MB of flash (Nordic nRF52840 PS, FICR:
                                     # CODEPAGESIZE 4096 x CODESIZE 256); no radio image is larger
RADIOS, IMAGES_PER_RADIO = 2, 3      # section 11: two radios, golden + candidate + scanner each
RAKFW_MIN = RADIOS * IMAGES_PER_RADIO * RADIO_IMAGE_MAX
SUBTYPE_RAKFW, SUBTYPE_JOURNAL = 0x40, 0x41   # ours; ESP-IDF leaves 0x40-0xFE to the application
MIN_NVS = 0x3000                     # three pages; ESP-IDF's minimum for an NVS partition
MIN_JOURNAL = 0x2000
MIN_COREDUMP = 0x10000
APP_ALIGN, DATA_ALIGN = 0x10000, 0x1000

TYPES = {"app": 0x00, "data": 0x01}
SUBTYPES = {
    "app": {"factory": 0x00, "test": 0x20, **{"ota_%d" % i: 0x10 + i for i in range(16)}},
    "data": {"ota": 0x00, "phy": 0x01, "nvs": 0x02, "coredump": 0x03, "nvs_keys": 0x04,
             "efuse": 0x05, "undefined": 0x06, "esphttpd": 0x80, "fat": 0x81, "spiffs": 0x82,
             "littlefs": 0x83},
}


class Partition:
    def __init__(self, name, ptype, subtype, offset, size, flags=""):
        self.name, self.type, self.subtype = name, ptype, subtype
        self.offset, self.size, self.flags = offset, size, flags

    @property
    def end(self):
        return self.offset + self.size

    def key(self):
        return (self.name, self.type, self.subtype, self.offset, self.size)

    def __repr__(self):
        return "%s(%s/0x%02x) 0x%X..0x%X" % (self.name, self.type, self.subtype, self.offset, self.end)


# ---------------------------------------------------------------- reading tables

def parse_number(text):
    text = text.strip().lower()
    for suffix, mult in (("k", 1024), ("m", 1024 * 1024)):
        if text.endswith(suffix):
            return int(text[:-1], 0) * mult
    return int(text, 0)


def parse_csv(text):
    """The csv as gen_esp32part.py reads it: name, type, subtype, offset, size, flags; a blank
    offset means 'after the previous one, aligned'; sizes take K and M."""
    out, last_end = [], TABLE_END
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) < 5:
            raise ValueError("line %d: fewer than five fields: %r" % (lineno, raw))
        name, ptype, subtype, offset, size = fields[:5]
        flags = fields[5] if len(fields) > 5 else ""
        if ptype not in TYPES:
            ptype = {0: "app", 1: "data"}.get(parse_number(ptype), ptype)
        if subtype in SUBTYPES.get(ptype, {}):
            sub = SUBTYPES[ptype][subtype]
        else:
            sub = parse_number(subtype)
        size = parse_number(size)
        align = APP_ALIGN if ptype == "app" else DATA_ALIGN
        if offset:
            off = parse_number(offset)
        else:
            off = (last_end + align - 1) // align * align
        out.append(Partition(name, ptype, sub, off, size, flags))
        last_end = off + size
    return out


def parse_bin(blob):
    """The binary table (gen_esp32part.py's output): 32-byte entries, magic 0x50AA, until the
    end marker 0xEBEB; an MD5 entry may precede it."""
    out = []
    for i in range(0, len(blob), 32):
        entry = blob[i:i + 32]
        if len(entry) < 32 or entry[:2] == b"\xff\xff":
            break
        magic, = struct.unpack("<H", entry[:2])
        if magic == 0xEBEB:   # md5 checksum entry
            continue
        if magic != 0x50AA:
            raise ValueError("binary table: bad magic 0x%04x at entry %d" % (magic, i // 32))
        ptype, subtype, offset, size = struct.unpack("<BBII", entry[2:12])
        name = entry[12:28].split(b"\0", 1)[0].decode("ascii")
        flags, = struct.unpack("<I", entry[28:32])
        tname = {0: "app", 1: "data"}.get(ptype, str(ptype))
        out.append(Partition(name, tname, subtype, offset, size, "encrypted" if flags & 1 else ""))
    return out


def flash_size_from_sdkconfig(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = re.search(r'^CONFIG_ESPTOOLPY_FLASHSIZE="(\w+)"', text, re.M)
    if not m:
        raise ValueError("%s: no CONFIG_ESPTOOLPY_FLASHSIZE" % path)
    off = re.search(r"^CONFIG_PARTITION_TABLE_OFFSET=(\w+)", text, re.M)
    return parse_number(m.group(1).replace("B", "")), (parse_number(off.group(1)) if off else None)


# ---------------------------------------------------------------- the checks

class Report:
    def __init__(self):
        self.failures, self.lines = [], []

    def ok(self, msg):
        self.lines.append("ok    " + msg)

    def fail(self, msg):
        self.failures.append(msg)
        self.lines.append("FAIL  " + msg)

    def check(self, cond, msg):
        (self.ok if cond else self.fail)(msg)
        return cond

    def passed(self):
        return len([l for l in self.lines if l.startswith("ok ")])


def fmt_table(parts):
    rows = [("name", "type", "subtype", "offset", "end", "size")]
    for p in parts:
        sub = next((k for k, v in SUBTYPES.get(p.type, {}).items() if v == p.subtype), "0x%02x" % p.subtype)
        rows.append((p.name, p.type, sub, "0x%06X" % p.offset, "0x%06X" % p.end,
                     "0x%06X (%d KB)" % (p.size, p.size // 1024)))
    widths = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    return "\n".join("  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip() for r in rows)


def check_table(parts, flash_size, rep):
    by_name = {}
    for p in parts:
        if p.name in by_name:
            rep.fail("two partitions are named %s" % p.name)
        by_name[p.name] = p

    # Alignment, placement, order, overlap, the flash.
    for p in parts:
        align = APP_ALIGN if p.type == "app" else DATA_ALIGN
        rep.check(p.offset % align == 0 and p.size % align == 0 and p.size > 0,
                  "%s is aligned to 0x%X in offset and size" % (p.name, align))
        rep.check(p.offset >= TABLE_END,
                  "%s begins at or above 0x%X, after the bootloader and the table" % (p.name, TABLE_END))
        rep.check(p.end <= flash_size,
                  "%s ends at 0x%X, inside the %d MB flash" % (p.name, p.end, flash_size // (1024 * 1024)))
        rep.check(p.end <= GAP[0] or p.offset >= GAP[1],
                  "%s lies outside 0x%X-0x%X, the span a factory image fills with 0xFF"
                  % (p.name, GAP[0], GAP[1] - 1))
    ordered = sorted(parts, key=lambda p: p.offset)
    rep.check([p.name for p in parts] == [p.name for p in ordered], "the table is in order of offset")
    for a, b in zip(ordered, ordered[1:]):
        rep.check(a.end <= b.offset, "%s (ends 0x%X) does not overlap %s (begins 0x%X)"
                  % (a.name, a.end, b.name, b.offset))

    # otadata.
    ota = by_name.get("otadata")
    if rep.check(ota is not None, "otadata exists"):
        rep.check(ota.type == "data" and ota.subtype == SUBTYPES["data"]["ota"] and ota.size == OTADATA_SIZE,
                  "otadata is data/ota of 0x%X bytes" % OTADATA_SIZE)

    # The two slots.
    apps = [p for p in parts if p.type == "app"]
    rep.check([p.name for p in apps] == list(APP_SLOTS),
              "the application partitions are exactly %s and %s, no factory slot - S-6 is A/B"
              % APP_SLOTS)
    slots = [by_name.get(n) for n in APP_SLOTS]
    if all(slots):
        a, b = slots
        rep.check(a.subtype == 0x10 and b.subtype == 0x11, "ota_0 and ota_1 carry subtypes ota_0 and ota_1")
        rep.check(a.size == b.size, "the slots are equal: ota_0 0x%X, ota_1 0x%X" % (a.size, b.size))
        rep.check(a.size >= MIN_SLOT, "ota_0 is at least 0x%X (3 MB, room to grow): 0x%X" % (MIN_SLOT, a.size))
        rep.check(b.size >= MIN_SLOT, "ota_1 is at least 0x%X (3 MB, room to grow): 0x%X" % (MIN_SLOT, b.size))
    app_end = max((p.end for p in apps), default=0)

    # Above the slots: nvs, journal, rakfw.
    def above(name, what):
        p = by_name.get(name)
        if rep.check(p is not None, "%s exists (%s)" % (name, what)):
            rep.check(p.offset >= app_end,
                      "%s at 0x%X is above the application slots (which end at 0x%X), out of a factory "
                      "image's reach" % (name, p.offset, app_end))
        return p

    nvs = above("nvs", "the settings")
    if nvs:
        rep.check(nvs.type == "data" and nvs.subtype == SUBTYPES["data"]["nvs"], "nvs is data/nvs")
        rep.check(nvs.size >= MIN_NVS, "nvs holds at least three pages: 0x%X" % nvs.size)
    journal = above("journal", "the update journal")
    if journal:
        rep.check(journal.type == "data" and journal.subtype == SUBTYPE_JOURNAL,
                  "journal is data/0x%02x, its own partition and not a corner of nvs" % SUBTYPE_JOURNAL)
        rep.check(journal.size >= MIN_JOURNAL, "journal is at least 0x%X: 0x%X" % (MIN_JOURNAL, journal.size))
    rakfw = above("rakfw", "the radio image store")
    if rakfw:
        rep.check(rakfw.type == "data" and rakfw.subtype == SUBTYPE_RAKFW, "rakfw is data/0x%02x" % SUBTYPE_RAKFW)
        rep.check(rakfw.size >= RAKFW_MIN,
                  "rakfw holds %d radios x %d images x %d KB = 0x%X: 0x%X"
                  % (RADIOS, IMAGES_PER_RADIO, RADIO_IMAGE_MAX // 1024, RAKFW_MIN, rakfw.size))

    # The core dump.
    cd = by_name.get("coredump")
    if rep.check(cd is not None, "coredump exists"):
        rep.check(cd.type == "data" and cd.subtype == SUBTYPES["data"]["coredump"] and cd.size >= MIN_COREDUMP,
                  "coredump is data/coredump of at least 0x%X" % MIN_COREDUMP)
    return by_name


def check_built(parts, by_name, args, rep):
    if args.bin:
        with open(args.bin, "rb") as fh:
            built = parse_bin(fh.read())
        same = [p.key() for p in built] == [p.key() for p in parts]
        rep.check(same, "the binary table the build made (%s) is the csv, entry for entry" % args.bin)
        if not same:
            rep.lines.append(fmt_table(built))
    slot = by_name.get(APP_SLOTS[0])
    if args.app and slot:
        size = os.path.getsize(args.app)
        rep.check(size <= slot.size,
                  "the application image is %d bytes, %.1f%% of its %d KB slot (%d KB free)"
                  % (size, 100.0 * size / slot.size, slot.size // 1024, (slot.size - size) // 1024))
    if args.factory:
        with open(args.factory, "rb") as fh:
            image = fh.read()
        check_factory(image, by_name, rep, args.factory)


def check_factory(image, by_name, rep, label="the factory image"):
    """A factory (merged) image is flashed at 0x0 and ends where the application ends. Nothing
    that must survive a bench flash may lie below its end."""
    n = len(image)
    rep.check(n > 0x10000 and image[0] == 0xE9, "%s begins with a bootloader (0xE9 at 0x0)" % label)
    rep.check(n > TABLE_OFFSET + 2 and image[TABLE_OFFSET:TABLE_OFFSET + 2] == b"\xaa\x50",
              "%s carries the partition table at 0x%X" % (label, TABLE_OFFSET))
    slot = by_name.get(APP_SLOTS[0])
    if slot:
        rep.check(n > slot.offset and image[slot.offset] == 0xE9,
                  "%s carries the application at 0x%X (ota_0)" % (label, slot.offset))
    gap = image[GAP[0]:GAP[1]]
    rep.check(len(gap) == GAP[1] - GAP[0] and set(gap) == {0xFF},
              "%s fills 0x%X-0x%X with 0xFF and nothing of ours is there" % (label, GAP[0], GAP[1] - 1))
    for name in ("nvs", "journal", "rakfw", "coredump"):
        p = by_name.get(name)
        if p:
            rep.check(n <= p.offset,
                      "%s ends at 0x%X, %d KB below %s at 0x%X: flashing it cannot touch %s"
                      % (label, n, (p.offset - n) // 1024, name, p.offset, name))


# ---------------------------------------------------------------- the self-test

def run(parts, flash_size, args=None, title=""):
    rep = Report()
    if title:
        rep.lines.append(title)
    rep.lines.append(fmt_table(parts))
    by_name = check_table(parts, flash_size, rep)
    if args:
        check_built(parts, by_name, args, rep)
    return rep


def self_test():
    """Three tables and three images: the real table passes; each wrong one is refused for the
    reasons its comment names - and each refusal must name the thing that is wrong, so a check that
    failed for an unrelated reason fails this test too."""
    flash = 16 * 1024 * 1024
    with open(os.path.join(REPO, "partitions.csv"), encoding="utf-8") as fh:
        real = parse_csv(fh.read())
    failures = []

    def expect(label, rep, wanted):
        text = "\n".join(rep.failures)
        if not wanted:
            if rep.failures:
                failures.append("%s: expected to pass, refused:\n%s" % (label, text))
            return
        if not rep.failures:
            failures.append("%s: expected a refusal, passed" % label)
            return
        for w in wanted:
            if w not in text:
                failures.append("%s: no refusal mentions %r; got:\n%s" % (label, w, text))

    expect("partitions.csv", run(real, flash), [])

    for fname, wanted in (
        ("nvs-below-the-app.csv", ["nvs", "rakfw", "journal", "0xFF"]),
        ("unequal-slots-and-overlap.csv", ["ota_1", "rakfw", "overlap"]),
    ):
        with open(os.path.join(HERE, "partitions-selftest", fname), encoding="utf-8") as fh:
            parts = parse_csv(fh.read())
        expect(fname, run(parts, flash), wanted)

    # A factory image that reaches nvs is refused; one that stops a byte short is not.
    by_name = {p.name: p for p in real}
    nvs_off = by_name["nvs"].offset
    good = bytearray(b"\xff" * nvs_off)
    good[0] = 0xE9
    good[TABLE_OFFSET:TABLE_OFFSET + 2] = b"\xaa\x50"
    good[by_name["ota_0"].offset] = 0xE9
    rep = Report()
    check_factory(bytes(good), by_name, rep, "an image of exactly nvs's offset")
    expect("factory image ending at nvs", rep, [])
    rep = Report()
    check_factory(bytes(good) + b"\xe9", by_name, rep, "an image one byte longer")
    expect("factory image reaching nvs", rep, ["nvs"])
    # ... and one that put something in the gap.
    bad = bytearray(good)
    bad[0x9000] = 0x00
    rep = Report()
    check_factory(bytes(bad), by_name, rep, "an image with a byte at 0x9000")
    expect("factory image with a byte in the gap", rep, ["0x9000"])

    # The binary table decodes to the csv - the real one, as gen_esp32part.py would write it.
    blob = b"".join(struct.pack("<HBBII", 0x50AA, TYPES[p.type], p.subtype, p.offset, p.size)
                    + p.name.encode().ljust(16, b"\0") + struct.pack("<I", 0) for p in real)
    if [p.key() for p in parse_bin(blob + b"\xff" * 32)] != [p.key() for p in real]:
        failures.append("parse_bin does not round-trip the real table")

    n = 3 + 3 + 1
    if failures:
        print("self-test: %d of %d cases FAILED" % (len(failures), n))
        for f in failures:
            print("  " + f.replace("\n", "\n    "))
        return 1
    print("self-test: %d cases, 0 failed - the real table passes; nvs-below-the-app.csv, "
          "unequal-slots-and-overlap.csv, an image reaching nvs and an image with a byte in the gap "
          "are refused for the right reasons; the binary table round-trips" % n)
    return 0


# ---------------------------------------------------------------- main

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--table", default=os.path.join(REPO, "partitions.csv"))
    ap.add_argument("--flash-size", default=None, help="e.g. 16MB; else from --sdkconfig")
    ap.add_argument("--sdkconfig")
    ap.add_argument("--bin")
    ap.add_argument("--app")
    ap.add_argument("--factory")
    args = ap.parse_args(argv[1:])

    if args.self_test:
        return self_test()

    flash_size, table_offset = None, None
    if args.sdkconfig:
        flash_size, table_offset = flash_size_from_sdkconfig(args.sdkconfig)
    if args.flash_size:
        flash_size = parse_number(args.flash_size.upper().replace("B", ""))
    if flash_size is None:
        print("give --flash-size or --sdkconfig")
        return 2
    if table_offset is not None and table_offset != TABLE_OFFSET:
        print("FAIL  CONFIG_PARTITION_TABLE_OFFSET is 0x%X; this table and tools/check_partitions.py "
              "assume 0x%X" % (table_offset, TABLE_OFFSET))
        return 1

    with open(args.table, encoding="utf-8") as fh:
        parts = parse_csv(fh.read())
    rep = run(parts, flash_size, args, "%s against a %d MB flash:" % (args.table, flash_size // (1024 * 1024)))
    print("\n".join(rep.lines))
    if rep.failures:
        print("%d of %d checks FAILED" % (len(rep.failures), len(rep.failures) + rep.passed()))
        return 1
    print("%d checks passed" % rep.passed())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
