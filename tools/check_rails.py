#!/usr/bin/env python3
"""The rails, in one place (task p3-06).

Every promise this firmware makes - never chip-erase, never drive reset high, never trust an
unsigned image, never skip TLS verification, never print a secret - is a test or a grep that
fails the build. This script is the grep half and the index of both halves: one entry per rail
of `tasks/phase-3/06-the-rails-and-the-bench.md`, each naming the decision it guards, each
either checked here or pointing at the test that holds it (and then checking that the test is
still there, so that a rail cannot be lost by deleting a file).

It is run by `tools/verify.sh` as the first thing in the host-tests step - before anything is
compiled, because a rail that is broken is cheapest to find then - and so by CI, which runs
`tools/verify.ps1`. By hand:

    python tools/check_rails.py            every rail, one line each
    python tools/check_rails.py --list     the table, nothing run
    python tools/check_rails.py --verbose  every rail with what it looked at

Exit 0 when every rail holds, 1 when one does not, 2 when the script was used wrongly.

The four greps that used to sit in `tools/verify.sh` itself - pin ownership, the dual adapter's
selector, core/ and the modem library, the TLS rail - are here now, unchanged in what they
refuse; `verify.sh` calls this instead. The one rail that cannot be checked from the source
alone is R6, which needs the built image: the build and release steps of `verify.sh` do it, and
R6's entry here holds *them* to doing it.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SOURCE_EXT = (".cpp", ".h", ".hpp", ".c", ".inc")

# A line that is only a comment: a rail may be named in a sentence. The same test the greps in
# verify.sh used ("a line whose first non-blank characters are // or * or /*").
COMMENT = re.compile(r"^\s*(//|\*|/\*)")


# ---- the plumbing ------------------------------------------------------------------------------


# The copied trees: another repository's files, at a commit, never edited here (task p4-01,
# docs/upstream.md). They are not this firmware's code, so the rails below are not about them -
# `sources()` leaves them out so that every rail keeps the scope it was written with - and the one
# entry that *is* about them is A, which names them itself.
COPIED = ("components/agent-core/", "host_test/agent-core/")


def sources(*dirs):
    """Every source file of ours under those directories, repository-relative, sorted. The copied
    agent core is not ours and is not here; entry A is what scans it."""
    found = []
    for d in dirs:
        base = os.path.join(ROOT, d)
        for parent, _subdirs, names in os.walk(base):
            for name in sorted(names):
                if name.endswith(SOURCE_EXT):
                    path = os.path.join(parent, name)
                    rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                    if any(rel.startswith(prefix) for prefix in COPIED):
                        continue
                    found.append(rel)
    return sorted(found)


def copied_sources(*dirs):
    """Every source file under a copied tree, repository-relative, sorted."""
    found = []
    for d in dirs:
        base = os.path.join(ROOT, d)
        for parent, _subdirs, names in os.walk(base):
            for name in sorted(names):
                if name.endswith(SOURCE_EXT):
                    path = os.path.join(parent, name)
                    found.append(os.path.relpath(path, ROOT).replace(os.sep, "/"))
    return sorted(found)


def read(path):
    with open(os.path.join(ROOT, path), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def without_comments(text):
    """The same text with every // comment and every /* */ block blanked out, so that a signature
    read out of a header is the declaration and not a sentence about it."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", line) for line in text.splitlines())


def grep(pattern, files, code_only=True):
    """Every (file, line number, text) matching, comment lines dropped unless asked for."""
    rx = re.compile(pattern)
    hits = []
    for path in files:
        for number, line in enumerate(read(path).splitlines(), 1):
            if code_only and COMMENT.match(line):
                continue
            if rx.search(line):
                hits.append((path, number, line.strip()))
    return hits


def shown(hits, limit=8):
    out = ["%s:%d: %s" % h for h in hits[:limit]]
    if len(hits) > limit:
        out.append("... and %d more" % (len(hits) - limit))
    return out


def must_exist(*paths):
    """The files a rail held by a test lives in. A rail cannot be lost by deleting one."""
    missing = [p for p in paths if not os.path.exists(os.path.join(ROOT, p))]
    return missing


def must_contain(path, *needles):
    """The markers that say the test still does what the rail says it does."""
    if not os.path.exists(os.path.join(ROOT, path)):
        return ["%s is missing" % path]
    text = read(path)
    return ["%s no longer contains %r" % (path, n) for n in needles if n not in text]


class Result:
    def __init__(self):
        self.ok = True
        self.notes = []
        self.details = []

    def says(self, note):
        self.notes.append(note)
        return self

    def looked_at(self, detail):
        self.details.append(detail)
        return self

    def broken(self, note, lines=()):
        self.ok = False
        self.notes.append(note)
        self.notes.extend("    " + line for line in lines)
        return self

    def problems(self, problems, note):
        if problems:
            self.broken(note, problems)
        return self


# ---- R1: nRESET is never output-high -----------------------------------------------------------


def rail_reset_never_high(r):
    """docs/ARCHITECTURE.md §2 rule 2, INTERCONNECT.md (frozen): open-drain, output-low to assert,
    input to release. The nRF52 has its own pull-up and a reset driven high fights it."""
    board = "components/core/board/radio_pins.cpp"
    text = read(board)

    # The pin has one handle: RadioPins::ResetLine, constructed with kNReset.gpio. Nothing else
    # may pass that pin to the driver.
    hits = grep(r"pins_?\.(setMode|write)\s*\(\s*kNReset", sources("components", "main"))
    if hits:
        r.broken("the nRESET pin is handed to the pin driver outside ResetLine:", shown(hits))
    else:
        r.looked_at("no setMode/write of kNReset outside ResetLine")

    # And that handle can only assert low and release: no push-pull mode, no pull-up, no high
    # level. The two method bodies, read out of the file.
    bodies = re.findall(r"RadioPins::ResetLine::\w+\(\)\s*\{(.*?)\n\}", text, re.S)
    if len(bodies) != 2:
        r.broken("ResetLine's two methods (assertLow, release) were not found in %s" % board)
    else:
        body = "\n".join(bodies)
        for forbidden in ("PinMode::kOutput", "PinMode::kInputPullUp", "write(gpio_, true)"):
            if forbidden in body:
                r.broken("ResetLine names %s: the pin would be driven high" % forbidden)
        r.looked_at("ResetLine: open-drain and input only, the level written is false")

    # The ESP-IDF side: the push-pull GPIO modes are reachable only through the one switch that
    # implements interfaces::Pins, so there is no second way to configure a pad as an output.
    hits = grep(r"GPIO_MODE_(INPUT_)?OUTPUT", sources("components/platform", "main"))
    outside = [h for h in hits if h[0] != "components/platform/pins.cpp"]
    if outside:
        r.broken("a GPIO output mode is configured outside components/platform/pins.cpp:",
                 shown(outside))
    else:
        r.looked_at("the GPIO output modes appear only in platform/pins.cpp's setMode switch")

    # The other half: the fake pins check the wire on every transition, in every test of the
    # suite, and host_test/rails/ holds the suite's own tally to zero.
    r.problems(must_contain("host_test/fakes/fake_pins.cpp", "reset-driven-high"),
               "the fake pins no longer check the rule:")
    r.problems(must_contain("host_test/rails/test_rail_pins.cpp", "reset-driven-high"),
               "the suite-wide assertion is gone:")
    r.says("held by the fake pins on every transition (reset-driven-high) and by "
           "host_test/rails/test_rail_pins.cpp over the whole suite")
    return r


# ---- R2: inputs before KILL, and the reset spacing ---------------------------------------------


def rail_kill_and_spacing(r):
    """docs/ARCHITECTURE.md §2 rules 2, 3 and 4: every other radio-facing pin is a plain input
    before the kill line goes high, and two resets are at least a second apart (two inside half a
    second put the Adafruit bootloader into UF2 mode, where it stays)."""
    # Who may touch a pin at all (task p0-03): the GPIO driver is called from platform/ and
    # nowhere else, and the seven radio-facing pins are named by RadioPins and nowhere else.
    hits = grep(r"\b(gpio_[a-z_]+\(|GPIO_NUM_[0-9]+|GPIO_MODE_)", sources("components", "main"))
    outside = [h for h in hits if not h[0].startswith("components/platform/")]
    if outside:
        r.broken("a GPIO call outside components/platform/:", shown(outside))
    else:
        r.looked_at("no GPIO call outside components/platform/")

    allowed_pins = re.compile(r"^components/core/(board/|include/core/board/)")
    hits = grep(r"\bk(NReset|SwdClk|SwdIo|PwrKill|Spare|WalterTx|WalterRx|RadioPins)\b",
                sources("components", "main"))
    outside = [h for h in hits if not allowed_pins.match(h[0])]
    if outside:
        r.broken("one of the seven radio pins is named outside core/board/:", shown(outside))
    else:
        r.looked_at("the seven radio pins are named only in components/core/board/")

    # The dual adapter's selector - the one sanctioned "SWDCLK toggles while nRESET is asserted"
    # (ARCHITECTURE §2 rule 6) - is compiled and constructed nowhere on a one-radio unit.
    selector_files = re.compile(
        r"^components/core/(board/(dual_target|radio_pins)\.cpp"
        r"|include/core/board/(dual_target|radio_pins)\.h)$")
    hits = grep(r"\b(DualTarget|selectorProgram)\b", sources("components", "main"))
    outside = [h for h in hits if not selector_files.match(h[0])]
    if outside:
        r.broken("the dual adapter's selector is named outside the two files that define it:",
                 shown(outside))
    else:
        r.looked_at("the selector is named only where it is defined (SingleTarget is what main "
                    "constructs)")

    # The spacing itself is a constant, and it is a second or more.
    spacing = re.search(r"kResetSpacingMs\s*=\s*(\d+)",
                        read("components/core/include/core/board/radio_pins.h"))
    if spacing is None:
        r.broken("kResetSpacingMs was not found in core/board/radio_pins.h")
    elif int(spacing.group(1)) < 1000:
        r.broken("kResetSpacingMs is %s ms; §2 rule 2 says at least 1000" % spacing.group(1))
    else:
        r.looked_at("kResetSpacingMs = %s ms" % spacing.group(1))

    for rule in ("driven-while-killed", "kill-driven-low", "kill-bounds", "reset-spacing",
                 "reset-pulse-length", "swdclk-while-reset"):
        r.problems(must_contain("host_test/fakes/fake_pins.cpp", rule),
                   "the fake pins no longer check a rule:")
    r.problems(must_contain("host_test/rails/test_rail_pins.cpp", "driven-while-killed",
                            "reset-spacing"),
               "the suite-wide assertion no longer names the rules:")
    r.says("held by the fake pins on every transition (driven-while-killed, reset-spacing, "
           "kill-driven-low, kill-bounds) and by host_test/rails/test_rail_pins.cpp")
    return r


# ---- R3: no ERASEALL, no ERASEUICR, no write to UICR -------------------------------------------

# The names core/nrf52/regs.h gives the registers nothing may write. A reference anywhere else
# under components/ is a hit, whatever it is doing with it: there is no legitimate reader of them
# outside the header, the simulator and the tests - and, since task p5-02 (S-5a), the one door:
# an exact list of two files, not a pattern. The same list as
# host_test/core/nrf52/test_nrf52_forbidden.cpp - which reads the tree at run time; this is the
# grep that fails before a compiler is started.
FORBIDDEN_REGISTERS = ("kEraseAll", "kEraseUicr", "kErasePagePartial", "uicr::", "kEraseAllStatus")
REGS_HEADER = "components/core/include/core/nrf52/regs.h"
DOOR_SOURCE = "components/core/nrf52/provision_door.cpp"
MAY_NAME_FORBIDDEN = (REGS_HEADER, DOOR_SOURCE)


def rail_no_chip_erase(r):
    """spec/SYSTEM.md S-5: no chip erase, ever, by anything automatic; S-5a: a person's typed
    `rak provision` is the one exception, through one door. The forbidden registers are named in
    exactly two files - regs.h, for the simulator and the reader, and provision_door.cpp, the door
    - and nowhere else under components/ and main/. Rail R3b holds the door to its one caller."""
    pattern = "|".join(re.escape(name) for name in FORBIDDEN_REGISTERS)
    files = [p for p in sources("components", "main") if p not in MAY_NAME_FORBIDDEN]
    hits = grep(pattern, files, code_only=False)
    if hits:
        r.broken("a forbidden register is named outside %s:" % " and ".join(MAY_NAME_FORBIDDEN), shown(hits))
    else:
        r.looked_at("%d files under components/ and main/, none naming one of %s"
                    % (len(files), ", ".join(FORBIDDEN_REGISTERS)))

    # And the header does name them, so that a grep which has stopped matching cannot pass; and
    # the door names the two it exists for (the CTRL-AP's ERASEALL and a UICR address) and never
    # the NVMC's erases, which S-5a does not need.
    in_header = grep(pattern, [REGS_HEADER], code_only=False)
    if len(in_header) < len(FORBIDDEN_REGISTERS):
        r.broken("%s names only %d of the %d forbidden registers: the grep is looking for the "
                 "wrong words" % (REGS_HEADER, len(in_header), len(FORBIDDEN_REGISTERS)))
    else:
        r.looked_at("%d lines of regs.h name them (the grep matches something)" % len(in_header))
    r.problems(must_exist(DOOR_SOURCE), "the door is gone:")
    if os.path.exists(os.path.join(ROOT, DOOR_SOURCE)):
        door_uses = grep(r"ctrl_ap::kEraseAll\b|uicr::", [DOOR_SOURCE])
        door_nvmc = grep(r"nvmc::kEraseAll\b|nvmc::kEraseUicr\b|kErasePagePartial", [DOOR_SOURCE])
        if not door_uses:
            r.broken("%s no longer writes the CTRL-AP's ERASEALL or UICR: the door is not the door" % DOOR_SOURCE)
        else:
            r.looked_at("%d lines of the door name the CTRL-AP's ERASEALL or a UICR address" % len(door_uses))
        if door_nvmc:
            r.broken("%s names an NVMC erase, which S-5a does not need:" % DOOR_SOURCE, shown(door_nvmc))

    r.problems(must_contain("host_test/sim/nrf52_chip.cpp", "NVMC.ERASEALL written",
                            "NVMC.ERASEUICR written", "CTRL-AP ERASEALL written", "a write to UICR",
                            "allowProvisioning"),
               "the simulated chip's tripwire no longer fires on all four, or cannot be disarmed by a door test:")
    r.problems(must_exist("host_test/core/nrf52/test_nrf52_forbidden.cpp",
                          "host_test/core/nrf52/test_provision_door.cpp"),
               "the test that shows the tripwire firing, or the door's, is gone:")
    r.says("held by the simulated chip's tripwire (armed by default: any of the four fails the "
           "running test unless it called allowProvisioning()), by "
           "host_test/core/nrf52/test_nrf52_forbidden.cpp and by the door's own tests")
    return r


# ---- R3b: the door has one caller --------------------------------------------------------------

# Who may include the door and the grant (task p5-02). The door's header: the sequence
# (core/provision/, task p5-03) and the door's own source. The grant's header: the sequence, the
# file where a parsed console verb runs (console.cpp - the parser only recognises the words), and
# the door's two files, which declare the type the door takes. Exact lists, not patterns; the
# directories that must include neither are named too, and one that does not exist yet is a pass,
# said so in the output.
DOOR_HEADER = "components/core/include/core/nrf52/provision_door.h"
GRANT_HEADER = "components/core/include/core/nrf52/provision_grant.h"
DOOR_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]core/nrf52/provision_door\.h[>"]')
GRANT_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]core/nrf52/provision_grant\.h[>"]')
SEQUENCE_DIR = "components/core/provision/"
# The sequence's two homes: its sources, and its headers where every header of core/ lives
# (tasks/phase-5/reports/02.md, deviation 2).
SEQUENCE_DIRS = (SEQUENCE_DIR, "components/core/include/core/provision/")
MAY_INCLUDE_DOOR = (DOOR_SOURCE,)
MAY_INCLUDE_GRANT = (DOOR_HEADER, DOOR_SOURCE, "components/core/console/console.cpp")
MUST_INCLUDE_NEITHER = ("components/core/supervisor", "components/core/update", "components/core/flasher",
                        "components/core/cycle", "components/core/agent", "components/core/link",
                        "components/core/net", "components/core/uplink", "components/core/image",
                        "components/core/board", "components/platform", "components/modem", "main")


def rail_one_door(r):
    """spec/SYSTEM.md S-5a: nothing automatic leads to the erase - not the supervisor, not an
    update, not the agent, not a job from the server. provision_door.h is included by
    core/provision/ (the console command's sequence, task p5-03: its sources there, its headers
    under include/core/provision/) and by nothing else; the grant's header by core/provision/ and
    console.cpp (where the typed verb runs) and nothing else."""
    r.problems(must_exist(DOOR_HEADER, GRANT_HEADER, DOOR_SOURCE), "the door or the grant is gone:")
    if must_exist(DOOR_HEADER, GRANT_HEADER, DOOR_SOURCE):
        return r
    files = sources("components", "main")

    def includes_of(rx):
        return [p for p in files if any(rx.match(line) for line in read(p).splitlines())]

    door_users = includes_of(DOOR_INCLUDE)
    grant_users = includes_of(GRANT_INCLUDE)

    def allowed(path, may):
        return path in may or any(path.startswith(d) for d in SEQUENCE_DIRS)

    bad_door = [p for p in door_users if not allowed(p, MAY_INCLUDE_DOOR)]
    bad_grant = [p for p in grant_users if not allowed(p, MAY_INCLUDE_GRANT)]
    if bad_door:
        r.broken("the door's header is included outside %s and %s:" % (SEQUENCE_DIR, ", ".join(MAY_INCLUDE_DOOR)),
                 bad_door)
    else:
        r.looked_at("provision_door.h is included by: %s" % ", ".join(door_users))
    if bad_grant:
        r.broken("the grant's header is included outside %s and %s:" % (SEQUENCE_DIR, ", ".join(MAY_INCLUDE_GRANT)),
                 bad_grant)
    else:
        r.looked_at("provision_grant.h is included by: %s" % ", ".join(grant_users))

    # The grep must still match something: the door's own source includes both.
    if DOOR_SOURCE not in door_users or DOOR_SOURCE not in grant_users:
        r.broken("%s does not include the door's and the grant's headers: the grep is looking for "
                 "the wrong lines" % DOOR_SOURCE)

    # The directories that must include neither, each by name, whether it exists yet or not.
    for d in MUST_INCLUDE_NEITHER:
        if not os.path.isdir(os.path.join(ROOT, d)):
            r.looked_at("%s/: does not exist yet, so includes neither (pass)" % d)
            continue
        prefix = d + "/"
        offenders = [p for p in door_users + grant_users if p.startswith(prefix) and not allowed(p, MAY_INCLUDE_GRANT)]
        if offenders:
            r.broken("%s/ reaches the door:" % d, offenders)
        else:
            r.looked_at("%s/: includes neither" % d)
    sequence_exists = os.path.isdir(os.path.join(ROOT, SEQUENCE_DIR))
    r.says("%s %s; the door's header is included by %d file(s), the grant's by %d"
           % (SEQUENCE_DIR, "exists" if sequence_exists else "does not exist yet (task p5-03)",
              len(door_users), len(grant_users)))
    return r


# ---- R4: the flasher writes inside the region and the settings page ----------------------------


def rail_flash_region(r):
    """docs/ARCHITECTURE.md §5: the region is this firmware's own table per board, a manifest's
    app_region must lie inside it, and the settings page is reached through one function whose
    region is that page alone. It never touches the file system, the SoftDevice, the bootloader or
    UICR."""
    # By construction: there is no erasePage or writeWords without a FlashRegion.
    nrf = without_comments(read("components/core/include/core/nrf52/nrf52.h"))
    for call in ("erasePage", "writeWords"):
        declarations = re.findall(r"\b%s\s*\(([^)]*)\)" % call, nrf, re.S)
        if not declarations:
            r.broken("%s is not declared in core/nrf52/nrf52.h" % call)
        for args in declarations:
            if "FlashRegion" not in args:
                r.broken("%s(%s) takes no FlashRegion: a write with no region guard" % (call, args))
    r.looked_at("core/nrf52/nrf52.h: every erase and write takes a FlashRegion")

    # And the simulated chip refuses to be flashed by a test that has not said which region may
    # be touched, so "asserted at the end of every test that flashes" is a property of the
    # simulator and not of everybody's care.
    r.problems(must_contain("host_test/sim/nrf52_chip.cpp", "no test may flash a chip without"),
               "the simulated chip no longer insists on a declared region:")
    r.problems(must_contain("host_test/core/flasher/flasher_bench.h", "onlyTouch", "alsoTouch"),
               "the flasher's bench no longer declares the region:")
    r.problems(must_contain("host_test/core/update/update_bench.h", "onlyTouch", "alsoTouch"),
               "the update's bench no longer declares the region:")
    r.says("held by the simulated chip: an erase or a write outside the declared window fails the "
           "running test, and a test that flashes without declaring one fails at the chip's "
           "destructor")
    return r


# ---- R5: nothing reaches the store or a radio without the six checks ---------------------------


def rail_six_checks(r):
    """spec/FIRMWARE-MANIFEST.md §5 and docs/ARCHITECTURE.md §6: the two tokens have no public
    constructor and are the only arguments the store's write doors take, so nothing can make the
    store erase a sector without a signed manifest naming that file for that slot."""
    verdict = without_comments(read("components/core/include/core/image/verdict.h"))
    for token in ("ExpectedFile", "AcceptedImage"):
        block = re.search(r"class %s\b.*?\n\};" % token, verdict, re.S)
        if block is None:
            r.broken("class %s was not found in core/image/verdict.h" % token)
            continue
        body = block.group(0)
        friends = re.findall(r"friend\s+class\s+(\w+)", body)
        if friends != ["ImageVerdict"]:
            r.broken("%s's friends are %s; only ImageVerdict may make one"
                     % (token, friends or "none"))
        if not re.search(r"private:.*?\b%s\(\)" % token, body, re.S):
            r.broken("%s's default constructor is not private: a token could be forged" % token)
        r.looked_at("%s: one friend (ImageVerdict), its constructor private" % token)

    # Nothing else in the firmware befriends them.
    hits = [h for h in grep(r"friend\s+class\s+ImageVerdict", sources("components"))
            if h[0] != "components/core/include/core/image/verdict.h"]
    if hits:
        r.broken("ImageVerdict is befriended outside verdict.h:", shown(hits))

    store = without_comments(read("components/core/include/core/image/store.h"))
    for door, token in (("beginWrite", "ExpectedFile"), ("commit", "AcceptedImage")):
        signature = re.search(r"StoreError\s+%s\s*\(([^;{]*)\)" % door, store, re.S)
        if signature is None:
            r.broken("the store's %s() was not found in core/image/store.h" % door)
        elif token not in signature.group(1):
            r.broken("the store's %s() does not take an %s: %s"
                     % (door, token, signature.group(1).strip()))
        else:
            r.looked_at("store::%s() takes an %s and nothing else will do" % (door, token))

    r.problems(must_exist("host_test/core/image/test_verdict.cpp",
                          "host_test/rails/test_rail_tokens.cpp"),
               "the tests that hold the token are gone:")
    r.says("held by the type: host_test/rails/test_rail_tokens.cpp fails to compile the forgeries "
           "and test_verdict.cpp walks the six checks over the spec's fixtures")
    return r


# ---- R6: a normal build trusts no test key and has no signing code -----------------------------

# The one source file that may read the build switch: core/image/trust.cpp, where two
# static_asserts hold the generated table and the switch to each other - a table with the test key
# can only come from a build that asked for it, and a build that asked for it must have got it. It
# is the opposite of a path that trusts a key quietly, and it is why it is allowed.
TEST_KEY_READER = "components/core/image/trust.cpp"


def rail_no_test_key(r):
    """spec/FIRMWARE-MANIFEST.md §3 and §5, task p2-01: a normal build carries the keys of
    keys/release-keys.txt and nothing else, and no build of any kind can sign anything."""
    # The source side: the build-time switch is read in one place, and only to hold the generated
    # table to it.
    hits = [h for h in grep(r"TWINOAK_TRUST_TEST_KEY", sources("components", "main"))
            if h[0] != TEST_KEY_READER]
    if hits:
        r.broken("the test-key switch is read outside %s:" % TEST_KEY_READER, shown(hits))
    else:
        r.looked_at("the switch is read only in %s, by two static_asserts" % TEST_KEY_READER)
    r.problems(must_contain(TEST_KEY_READER, "static_assert(generated::kTrustsTestKey",
                            "static_assert(!generated::kTrustsTestKey"),
               "the build switch and the generated table are no longer held to each other:")

    # The image side cannot be checked from the source: verify.sh's build step searches the two
    # built images for the key's bytes and the +TESTKEY stamp and the linker map for a signing
    # symbol, and its release step expects the tooling to refuse a test-key build. This rail
    # holds *those* to existing.
    r.problems(must_contain("tools/verify.sh", "check_trust_in_image",
                            "crypto_sign_ed25519_tweet", "test key bytes",
                            "build_release.py accepted a test-key build"),
               "verify.sh no longer searches the built image and the map:")
    r.looked_at("verify.sh: the built images are searched for the key's bytes and the stamp, the "
                "map for a signing symbol, and build_release.py is made to refuse a test-key build "
                "twice")
    r.problems(must_exist("tools/release/test_release.py"),
               "the release tooling's own tests are gone:")
    r.says("held by tools/verify.sh over the built image (the key's bytes, the +TESTKEY stamp, the "
           "linker map) and by the release tooling's refusal - neither reachable from the source "
           "alone")
    return r


# ---- R7: TLS is always verified, and no http:// ------------------------------------------------

TLS_FILE = "components/modem/walter_tls.cpp"


def rail_tls_verified(r):
    """spec/SYSTEM.md S-12 and docs/ARCHITECTURE.md §9: the modem's TLS validation levels other
    than "the chain and the host name" are named nowhere, the one that is appears in one file,
    tlsConfigProfile is called from exactly one code line, and no http:// literal is in the code.
    Task p3-02's checks, which lived in verify.sh; the rail is the same."""
    files = sources("components", "main")

    hits = [h for h in grep(r"WALTER_MODEM_TLS_VALIDATION_", files)
            if "WALTER_MODEM_TLS_VALIDATION_URL_AND_CA" not in h[2]]
    if hits:
        r.broken("a TLS validation level other than URL_AND_CA is named:", shown(hits))
    else:
        r.looked_at("no validation level but URL_AND_CA (the chain and the host name) is named")

    hits = [h for h in grep(r"WALTER_MODEM_TLS_VALIDATION_URL_AND_CA", files) if h[0] != TLS_FILE]
    if hits:
        r.broken("the TLS validation level is named outside %s:" % TLS_FILE, shown(hits))
    else:
        r.looked_at("URL_AND_CA appears only in %s" % TLS_FILE)

    calls = grep(r"tlsConfigProfile", files)
    if len(calls) != 1 or calls[0][0] != TLS_FILE:
        r.broken("tlsConfigProfile appears in %d code lines; it must be exactly one, in %s:"
                 % (len(calls), TLS_FILE), shown(calls))
    else:
        r.looked_at("tlsConfigProfile is called once, at %s:%d" % (calls[0][0], calls[0][1]))

    hits = grep(r'"http://', files)
    if hits:
        r.broken("an http:// literal in the firmware's code:", shown(hits))
    else:
        r.looked_at("no http:// literal")

    r.problems(must_exist("host_test/core/net/test_trust_rail.cpp"),
               "the test half of the TLS rail is gone:")
    r.says("held here and by host_test/core/net/test_trust_rail.cpp, which names the file and line")
    return r


# ---- R8, R9: nothing identifying, and nothing the mesh said, is written down --------------------


def rail_no_secrets(r):
    """spec/SYSTEM.md S-12: a password is in RAM for the length of the exchange and nowhere else;
    the IMEI, ICCID and IMSI identify a subscription and `lte id` is the only thing that prints
    them."""
    r.problems(must_contain("host_test/rails/test_rail_secrets.cpp",
                            "every command of the table", "the journal", "the wake record",
                            "the retained records"),
               "the capture test no longer covers all of it:")
    r.problems(must_exist("host_test/core/console/test_console_secrets.cpp",
                          "host_test/core/uplink/test_uplink_privacy.cpp"),
               "the tests this rail was built on are gone:")
    # The settings table is where a key becomes a secret; a secret that stopped being one would
    # make the capture test pass by printing something nobody minds. The `secret` field is the
    # fifth of a KeyDef row (core/settings/keys.h), after the kind.
    table = without_comments(read("components/core/settings/keys.cpp"))
    for key in ("kLtePin", "kServerToken"):
        row = re.search(r"\{Key::%s,[^}]*?Kind::\w+,\s*(true|false)" % key, table, re.S)
        if row is None:
            r.broken("%s has no row in core/settings/keys.cpp: the capture test provisions it" % key)
        elif row.group(1) != "true":
            r.broken("%s is no longer marked secret in core/settings/keys.cpp" % key)
    r.looked_at("core/settings/keys.cpp marks lte.pin and server.token secret")
    r.says("held by host_test/rails/test_rail_secrets.cpp: two secrets provisioned, a simulated "
           "day of wakes, every command of the table, and the console, the log ring, `status`, the "
           "journal's bytes, the retained records and the wake records all searched")
    return r


def rail_nothing_the_mesh_said(r):
    """spec/SYSTEM.md S-12: nothing the mesh says is logged as text. The scanner's lines and a
    mesh reply's payload are offered to whoever asked for them and written down nowhere."""
    r.problems(must_contain("host_test/rails/test_rail_secrets.cpp", "what the mesh said"),
               "the capture test no longer searches for the radio's payloads:")
    r.says("held by the same capture: the scripted radio's payloads are searched for in "
           "everything that was written down")
    return r


# ---- R10: never both updates in one wake -------------------------------------------------------


def rail_one_update(r):
    """spec/SYSTEM.md S-7: one wake updates the radio or the Walter, not both; and a Walter image
    on trial or unconfirmed refuses to start either."""
    r.problems(must_contain("components/core/cycle/wake_cycle.cpp", "one update step a wake (S-7)"),
               "the wake cycle no longer refuses the second update step:")
    r.problems(must_contain("host_test/rails/test_rail_updates.cpp", "S-7", "journal"),
               "the month that asserts S-7 on the journal is gone:")
    r.problems(must_exist("host_test/core/cycle/test_wake_backend.cpp"),
               "the test of a backend asking for both is gone:")
    r.says("held by host_test/rails/test_rail_updates.cpp: a month of wakes in which both updates "
           "are asked for at every one, asserted on the journal")
    return r


# ---- R11: the supervisor's daily caps ----------------------------------------------------------


def rail_daily_caps(r):
    """docs/ARCHITECTURE.md §8: the caps are per day, they survive a restart, and a cap is
    forgiven only when a whole day can be shown to have passed."""
    r.problems(must_exist("host_test/core/supervisor/test_supervisor_property.cpp",
                          "host_test/rails/test_rail_caps.cpp"),
               "the property test or its longer horizon is gone:")
    r.says("held by host_test/rails/test_rail_caps.cpp: phase 2 task 05's property test over a "
           "longer horizon, with the clock re-anchored and the power cut")
    return r


# ---- R12: core/ knows nothing of ESP-IDF or the modem library ----------------------------------

# The ESP-IDF tree's own prefixes plus the two neighbours core/ must not know about. The same list
# as tools/cmake/no_idf_include.cmake, which runs as a build step; this is the same check before
# anything is configured, so a bad include is a sentence and not a compiler error.
IDF_INCLUDE_PREFIXES = (
    "esp_", "esp32", "esp-idf", "freertos/", "driver/", "soc/", "hal/", "rom/", "xtensa",
    "riscv/", "sdkconfig", "nvs", "spi_flash", "bootloader", "lwip/", "mbedtls/", "esp_log",
    "Arduino", "fatfs", "WalterModem", "platform/", "modem/",
)
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]*)[>"]')


def rail_core_is_portable(r):
    """docs/ARCHITECTURE.md §1: the logic lives in components/core/ with no ESP-IDF include and no
    modem symbol, so that everything which can go wrong in the field has a host test that makes it
    go wrong."""
    files = sources("components/core")
    bad = []
    for path in files:
        for number, line in enumerate(read(path).splitlines(), 1):
            m = INCLUDE.match(line)
            if m and any(m.group(1).startswith(p) for p in IDF_INCLUDE_PREFIXES):
                bad.append((path, number, line.strip()))
    if bad:
        r.broken("components/core/ includes something of the board:", shown(bad))
    else:
        r.looked_at("%d files under components/core/, no ESP-IDF or modem include" % len(files))

    hits = grep(r"\b(WalterModem|WALTER_MODEM_[A-Z_]+)\b", files)
    if hits:
        r.broken("a WalterModem symbol in components/core/:", shown(hits))
    else:
        r.looked_at("no WalterModem symbol in components/core/")

    r.problems(must_exist("tools/cmake/no_idf_include.cmake"),
               "the build step that checks the same thing is gone:")
    r.says("held here and by tools/cmake/no_idf_include.cmake, a build step of both the firmware "
           "build and host_test/")
    return r


# ---- R13: spec/ is the copy it claims to be ----------------------------------------------------


def rail_spec_is_a_copy(r):
    """spec/README.md and tasks/README.md: spec/ is copied from the design repository, never
    edited here, and held byte for byte against spec/MANIFEST.txt."""
    try:
        done = subprocess.run([sys.executable, "spec/tools/specsync.py", "verify", "spec"],
                              cwd=ROOT, capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.SubprocessError) as e:
        return r.broken("specsync.py could not be run: %s" % e)
    last = [line for line in (done.stdout + done.stderr).splitlines() if line.strip()]
    if done.returncode != 0:
        return r.broken("specsync.py verify spec failed:", last[-8:])
    r.looked_at(last[-1] if last else "specsync.py verify spec: ok")
    r.says("held by spec/tools/specsync.py against spec/MANIFEST.txt (verify.sh's first step runs "
           "it too)")
    return r


# ---- R14: no mesh request is sent by this firmware ---------------------------------------------

# The request types of LINK-PROTOCOL.md §6 and §7 - everything that needs the MESH_CLIENT or
# OBSERVATIONS capability. The codec encodes them and the client has a call for each (that is
# phase 1's work, and phase 4's to use); nothing above the link may name one until the agent
# exists (spec/SYSTEM.md §8).
MESH_REQUESTS = ("kJobBegin", "kJobEnd", "kPathSet", "kPathGet", "kSendLogin", "kSendRequest",
                 "kSendCli", "kSendDiscovery", "kSendTrace", "kReplyGet", "kObsPull",
                 "kMeshCounters")


def rail_no_mesh_request(r):
    """spec/SYSTEM.md §8 and tasks/phase-3/README.md: this pile ends at a seam, and the mesh
    client's calls are not used at all. The agent goes behind the seam when the server side is
    built."""
    pattern = r"\b(%s)\b" % "|".join(MESH_REQUESTS)
    link = re.compile(r"^components/core/(link/|include/core/link/)")
    hits = [h for h in grep(pattern, sources("components", "main")) if not link.match(h[0])]
    if hits:
        r.broken("a mesh request type is named above the link:", shown(hits))
    else:
        r.looked_at("the %d mesh request types are named only under components/core/link/"
                    % len(MESH_REQUESTS))

    r.problems(must_contain("host_test/core/cycle/test_wake_month.cpp", "No mesh request"),
               "the month's assertion is gone:")
    r.problems(must_contain("host_test/rails/test_rail_updates.cpp", "mesh"),
               "the rails' own month no longer counts mesh requests:")
    r.says("held here and by the simulated month, which counts what the radio was asked over "
           "every wake of it")
    return r


# ---- the rails, in one table -------------------------------------------------------------------

RAILS = [
    ("R1", "nRESET is never output-high", rail_reset_never_high),
    ("R2", "radio-facing pins are inputs before KILL; two resets a second apart", rail_kill_and_spacing),
    ("R3", "no ERASEALL, no ERASEUICR, no write to UICR - outside regs.h and the one door", rail_no_chip_erase),
    ("R3b", "the door has one caller: core/provision/, opened by a typed console command", rail_one_door),
    ("R4", "the flasher writes only inside the region and the settings page", rail_flash_region),
    ("R5", "nothing reaches the store or a radio without the six checks", rail_six_checks),
    ("R6", "a normal build trusts no test key and has no signing code", rail_no_test_key),
    ("R7", "TLS is always verified; no http://", rail_tls_verified),
    ("R8", "no secret, token, PIN, IMEI or ICCID is written down", rail_no_secrets),
    ("R9", "nothing the mesh or a scanner said is logged as text", rail_nothing_the_mesh_said),
    ("R10", "never both updates in one wake, never on an unconfirmed image", rail_one_update),
    ("R11", "the supervisor's daily caps", rail_daily_caps),
    ("R12", "core/ includes nothing of ESP-IDF or the modem library", rail_core_is_portable),
    ("R13", "spec/ is the copy it claims to be", rail_spec_is_a_copy),
    ("R14", "no mesh request is sent by this firmware", rail_no_mesh_request),
]


# ---- the bench's own table ----------------------------------------------------------------------

# The two phrases the tasks used for a number that only a bench can set. A section header
# containing one makes every constant under it, up to the next header, a bench constant; so does a
# comment block immediately above one constant.
BENCH_PHRASES = ("TO BE SET ON THE BENCH", "PLACEHOLDER")
SECTION = re.compile(r"^\s*//\s*--+\s*(.*?)\s*--+\s*$")
CONSTANT = re.compile(r"^\s*(?:inline\s+)?constexpr\s+[\w:<>, ]+?\s(k\w+)\s*(?:=|\{)")
COMMENT_LINE = re.compile(r"^\s*//")
BENCH_TABLE = "docs/bench.md"
# Where a bench phrase is about something that is not a constant of this kind.
BENCH_PHRASE_EXEMPT = {
    "components/core/link/codec/cobs.cpp",          # "a placeholder: the next block's code byte"
    "components/core/include/core/console/commands.h",  # "the words alone, no placeholder"
    "components/core/console/commands.cpp",         # the help text of `power`
    "components/core/console/console.cpp",          # the line `power` prints
}


def says_bench(text):
    return any(p.lower() in text.lower() for p in BENCH_PHRASES)


def file_header(lines):
    """The comment block at the very top of a file, before the first #pragma or #include."""
    block = []
    for line in lines:
        if COMMENT_LINE.match(line) or not line.strip():
            block.append(line)
            continue
        if line.lstrip().startswith("#"):
            break
        break
    return "\n".join(block)


def bench_constants():
    """Every named constant any task marked as one for the bench, with its file and its value.

    Three ways a constant is marked, strongest first, because that is how the tasks wrote them:
    the **file's** own header says every number in it is a placeholder (core/power/interval_policy.h
    does, in those words); a **section** header says so (`// -- the numbers ... TO BE SET ON THE
    BENCH --`), and then every constant until the next section header is one; or the **comment
    block** above a constant says so, and it carries until another comment block replaces it - so
    a sentence about three thresholds marks all three, and one about a struct marks the constant
    declared after it (core/board/radio_pins.h's kKillTiming).
    """
    found = []
    for path in sources("components"):
        if path in BENCH_PHRASE_EXEMPT:
            continue
        lines = read(path).splitlines()
        whole_file = says_bench(file_header(lines))
        section_is_bench = False
        block = []
        in_block = False
        i = 0
        while i < len(lines):
            line = lines[i]
            section = SECTION.match(line)
            if section:
                # The header, and the paragraph of comment under it: the tasks wrote the sentence
                # either on the dashes or in the lines below them.
                intro = [section.group(1)]
                j = i + 1
                while j < len(lines) and (COMMENT_LINE.match(lines[j]) or not lines[j].strip()):
                    intro.append(lines[j])
                    j += 1
                section_is_bench = says_bench("\n".join(intro))
                block = []
                in_block = False
                i += 1
                continue
            if COMMENT_LINE.match(line):
                if not in_block:
                    block = []
                    in_block = True
                block.append(line)
                i += 1
                continue
            in_block = False
            constant = CONSTANT.match(line)
            if constant:
                marked = whole_file or section_is_bench or says_bench("\n".join(block))
                if marked:
                    body = re.sub(r"//.*$", "", line)
                    value = body.split("=", 1)[1] if "=" in body else body.split("{", 1)[1].split("}")[0]
                    found.append((constant.group(1), path, value.strip().rstrip(";").strip()))
            i += 1
    return found


def check_bench_table(r):
    """Every bench constant has a row in docs/bench.md's *What to bring back* (the task's fourth
    acceptance criterion: a grep for the phrase the tasks used finds nothing that is not in it)."""
    constants = bench_constants()
    if not constants:
        return r.broken("no bench constant was found at all: the extraction is broken")
    if not os.path.exists(os.path.join(ROOT, BENCH_TABLE)):
        return r.broken("%s is missing" % BENCH_TABLE)
    table = read(BENCH_TABLE)
    heading = "## What to bring back"
    if heading not in table:
        return r.broken("%s has no *%s* section" % (BENCH_TABLE, heading.strip("# ")))
    section = table.split(heading, 1)[1]
    missing = sorted({name for name, _p, _v in constants if ("`%s`" % name) not in section})
    if missing:
        r.broken("%d of %d bench constants have no row in %s's *What to bring back*:"
                 % (len(missing), len(constants), BENCH_TABLE),
                 ["%s (%s)" % (name, path) for name, path, _v in constants if name in missing][:12])
    else:
        r.looked_at("%d bench constants, every one with a row" % len(constants))
    # The other direction: a row for a constant that no longer exists is a table that lies.
    named = set(re.findall(r"`(k[A-Za-z0-9_]+)`", section))
    known = {name for name, _p, _v in constants}
    stale = sorted(named - known)
    if stale:
        r.broken("the table names constants that are not marked for the bench in the source:",
                 stale[:12])
    return r


# ---- A: the copied agent core keeps its own boundary, and the copy is a copy ------------------

AGENT_CORE = "components/agent-core"
AGENT_CORE_HOST = "host_test/agent-core"

# What a file of the library may include, as an allow list and not a deny list, which is how
# upstream's own test/test_library states it: a header of Arduino's, the ESP-IDF's, FreeRTOS's or
# MeshCore's that nobody thought to forbid is forbidden anyway. The deny list is kept beside it so
# that a failure says which rule was broken rather than merely that a name was unexpected.
AGENT_CORE_FORBIDDEN_HEADERS = (
    ("Arduino.h", "Arduino"), ("Stream.h", "Arduino"), ("Print.h", "Arduino"),
    ("SPIFFS.h", "Arduino"), ("Preferences.h", "NVS (Preferences)"),
    ("HTTPClient.h", "the Arduino HTTP client"), ("WiFiClientSecure.h", "the Arduino TLS client"),
    ("MeshCore.h", "MeshCore"), ("Mesh.h", "MeshCore"),
)
AGENT_CORE_FORBIDDEN_PREFIXES = (
    ("esp_", "the ESP-IDF"), ("freertos/", "FreeRTOS"), ("helpers/", "MeshCore"),
)
# Names of the board that may not appear in the library's code (comments stripped: a comment may
# name one to say what the library's own constant stands for). MeshCore's three buffer constants
# are here because the library has constants of its own for them (docs/library.md §3.1).
AGENT_CORE_FORBIDDEN_NAMES = (
    "Serial", "ESP.", "esp_", "freertos", "MAX_PACKET_PAYLOAD", "MAX_PATH_SIZE", "PUB_KEY_SIZE",
    "delay(",
)

# Every way of not verifying a certificate, as upstream's test_tls_policy writes them - split so
# that a grep of this tree for one of them finds only the file that forbids it.
TLS_BYPASSES = (
    "set" + "Insecure", "INSE" + "CURE", "verify" + "=false", "VERIFY" + "_NONE",
    "VERIFY" + "_OPTIONAL", "skip_cert_" + "common_name_check", "setFinger" + "print",
    "setCACert(" + "NULL", "setCACert(" + "nullptr", "esp_tls_" + "insecure",
)
# A store or a file system: the library reads no setting and writes no file, and a password that
# passed through it must have nowhere to land (docs/library.md §8, upstream's
# Credential.NoModuleThePasswordPassesThroughNamesTheStoreOrAFileSystem).
STORAGE_APIS = ("nvs_", "Preferences", "fopen(", "ofstream", "SPIFFS", "LittleFS", "FatFs")


def is_standard_header(name):
    """A C++ standard header: no dot and no slash. Every header of a platform has one or the
    other, which makes this exact enough (upstream's test_library says the same)."""
    return "." not in name and "/" not in name


def rail_agent_core(r):
    """docs/ARCHITECTURE.md §13 and docs/upstream.md: the agent core is another repository's
    library at one commit, copied and never edited here. It keeps its own boundary - nothing of
    Arduino, the ESP-IDF, WiFi, NVS or MeshCore, and nothing but <agent/...>, ArduinoJson and the
    standard library - it skips no certificate verification and it names no store; and the copy is
    held to its manifests by tools/sync-agent-core.ps1 -Verify, which tools/verify.ps1 runs."""
    if not os.path.isdir(os.path.join(ROOT, AGENT_CORE)):
        return r.broken("%s is not there: the agent core has not been copied "
                        "(tools/sync-agent-core.ps1 -Ref <tag>)" % AGENT_CORE)
    files = copied_sources(AGENT_CORE)

    # -- the boundary, as an allow list
    bad = []
    for path in files:
        for number, line in enumerate(read(path).splitlines(), 1):
            m = INCLUDE.match(line)
            if not m:
                continue
            name = m.group(1)
            rule = None
            for header, why in AGENT_CORE_FORBIDDEN_HEADERS:
                if name == header:
                    rule = why
            for prefix, why in AGENT_CORE_FORBIDDEN_PREFIXES:
                if name.startswith(prefix):
                    rule = why
            if "WiFi" in name or "MeshCore" in name:
                rule = "WiFi" if "WiFi" in name else "MeshCore"
            if rule is not None:
                bad.append((path, number, "%s - that is %s" % (line.strip(), rule)))
                continue
            allowed = name.startswith("agent/") or name == "ArduinoJson.h" or is_standard_header(name)
            if not allowed:
                bad.append((path, number, "%s - neither <agent/...>, nor ArduinoJson, nor the "
                                          "standard library" % line.strip()))
    if bad:
        r.broken("the agent core includes something that is not its own:", shown(bad))
    else:
        r.looked_at("%d files under %s, every include <agent/...>, ArduinoJson or the standard "
                    "library" % (len(files), AGENT_CORE))

    # Inside the library a header is reached as <agent/X.h> too, so that a consumer's include path
    # is the only one that has to be right.
    quoted = [h for h in grep(r'#\s*include\s*"', files, code_only=False)]
    if quoted:
        r.broken("the agent core includes a header with quotes:", shown(quoted))
    else:
        r.looked_at("no quoted include inside the library")

    # -- the names of the board
    named = []
    for path in files:
        code = without_comments(read(path))
        for name in AGENT_CORE_FORBIDDEN_NAMES:
            if name in code:
                named.append((path, 0, "names %s" % name))
    if named:
        r.broken("the agent core names something of the board:", shown(named))
    else:
        r.looked_at("none of %d forbidden names of the board in the library's code"
                    % len(AGENT_CORE_FORBIDDEN_NAMES))

    # -- TLS, over the library and the copied test support alike
    copied = files + copied_sources(AGENT_CORE_HOST)
    bypasses = []
    for pattern in TLS_BYPASSES:
        bypasses.extend(grep(re.escape(pattern), copied, code_only=False))
    if bypasses:
        r.broken("a way of skipping certificate verification in the copied trees:", shown(bypasses))
    else:
        r.looked_at("%d copied files, none of the %d ways of skipping certificate verification"
                    % (len(copied), len(TLS_BYPASSES)))

    # -- a store or a file system the library could leave a password in
    storage = []
    for api in STORAGE_APIS:
        storage.extend(grep(re.escape(api), files))
    if storage:
        r.broken("the agent core names a store or a file system (a password passes through it):",
                 shown(storage))
    else:
        r.looked_at("the library names no store and no file system")

    # -- the copy is a copy, and something checks that it is
    r.problems(must_exist("tools/sync-agent-core.ps1",
                          AGENT_CORE + "/MANIFEST.txt",
                          AGENT_CORE_HOST + "/MANIFEST.txt",
                          AGENT_CORE + "/CMakeLists.txt",
                          AGENT_CORE + "/idf_component.yml"),
               "the copy or its copier is gone:")
    r.problems(must_contain("tools/verify.ps1", "sync-agent-core.ps1", "-Verify"),
               "tools/verify.ps1 no longer holds the copy against its manifests:")
    r.problems(must_contain("host_test/rails/test_rail_agent_core.cpp",
                            "the agent core's commit is in two places"),
               "the test that holds the two manifests' commits equal is gone:")
    r.problems(must_contain("host_test/CMakeLists.txt", "agent_core", "GTest::gtest",
                            "test_${suite}.cpp"),
               "the host build no longer compiles the library and its two suites:")
    r.says("held here, by tools/sync-agent-core.ps1 -Verify (every file of both trees against its "
           "manifest, before the container starts) and by "
           "host_test/rails/test_rail_agent_core.cpp (the pin in two places, held equal)")
    return r


EXTRA = [
    ("A", "the copied agent core keeps its boundary, and the copy is a copy", rail_agent_core),
    ("B", "docs/bench.md's *What to bring back* is complete", check_bench_table),
]


# ---- the run -----------------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(description="the rails, in one place")
    parser.add_argument("--list", action="store_true", help="the table, nothing run")
    parser.add_argument("--verbose", "-v", action="store_true", help="what each rail looked at")
    parser.add_argument("--only", metavar="ID", help="one entry (R1 ... R14, R3b, A, B)")
    args = parser.parse_args(argv)

    entries = RAILS + EXTRA
    if args.only:
        entries = [e for e in entries if e[0] == args.only]
        if not entries:
            print("unknown entry %r; one of: %s"
                  % (args.only, " ".join(i for i, _n, _f in RAILS + EXTRA)))
            return 2

    if args.list:
        for ident, name, fn in entries:
            print("%-4s %s" % (ident, name))
            for line in (fn.__doc__ or "").strip().splitlines():
                print("       " + line.strip())
        return 0

    print("rails: %d entries - one per rail of tasks/phase-3/06-the-rails-and-the-bench.md, plus what later piles added"
          % len(entries))
    failed = []
    for ident, name, fn in entries:
        r = fn(Result())
        print("%-5s %-4s %s" % ("ok" if r.ok else "FAIL", ident, name))
        for note in r.notes:
            print("           " + note)
        if args.verbose:
            for detail in r.details:
                print("           . " + detail)
        if not r.ok:
            failed.append(ident)
    print()
    if failed:
        print("rails: %d of %d broken: %s" % (len(failed), len(entries), " ".join(failed)))
        return 1
    print("rails: all %d hold" % len(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
