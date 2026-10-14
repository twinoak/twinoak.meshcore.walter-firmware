#!/usr/bin/env python3
"""The TLS roots this firmware trusts, generated from keys/tls/*.pem at build time (task p3-02).

    python tools/gen_tls_roots.py keys/tls <out>/tls_roots.h

Each PEM under keys/tls/ carries a header of `# key: value` lines before its BEGIN line - the
root's subject, its SHA-256 fingerprint over the DER, its expiry, where it was fetched from and
when (keys/tls/README.md; the agent firmware's src/agent/TlsRoots.h is the model). This script
holds every header to the certificate itself - the fingerprint recomputed, the subject and the
expiry read out of the DER - and refuses to write the table when one disagrees, so that a root
whose header is a copy-paste error fails the build rather than reaching a unit. Both CMake
projects run it (components/core/CMakeLists.txt, host_test/CMakeLists.txt) and
components/core/net/trust.cpp includes what it writes; host_test/core/net/test_trust.cpp holds
the same fingerprints a second time, through the firmware's own SHA-256 and base64.

The roots are ordered by file name, and core/net/trust.h names each index with a constant and
holds it to the file name with a static_assert, so that adding a file cannot silently move a
root the host table points at.

Standard library only (a small DER walk for the subject and the validity). Deterministic.
"""

import base64
import hashlib
import os
import re
import sys


class Refused(Exception):
    pass


# ---- a minimal DER reader: enough for TBSCertificate's subject and validity -----------------------

def der_tlv(data, at):
    """(tag, value_start, value_end, next) of the TLV at `at`."""
    if at + 2 > len(data):
        raise Refused("DER: truncated at %d" % at)
    tag = data[at]
    length = data[at + 1]
    at += 2
    if length & 0x80:
        n = length & 0x7F
        if n == 0 or n > 4 or at + n > len(data):
            raise Refused("DER: bad length at %d" % at)
        length = int.from_bytes(data[at:at + n], "big")
        at += n
    if at + length > len(data):
        raise Refused("DER: value runs past the end at %d" % at)
    return tag, at, at + length, at + length


def der_children(data, start, end):
    out = []
    at = start
    while at < end:
        tag, vs, ve, nxt = der_tlv(data, at)
        out.append((tag, vs, ve))
        at = nxt
    return out


OID_NAMES = {
    "2.5.4.3": "CN", "2.5.4.6": "C", "2.5.4.7": "L", "2.5.4.8": "ST", "2.5.4.10": "O",
    "2.5.4.11": "OU", "2.5.4.5": "serialNumber", "2.5.4.9": "street",
}


def der_oid(data, start, end):
    first = data[start]
    parts = [str(first // 40), str(first % 40)]
    value = 0
    for b in data[start + 1:end]:
        value = (value << 7) | (b & 0x7F)
        if not b & 0x80:
            parts.append(str(value))
            value = 0
    return ".".join(parts)


def der_string(data, tag, start, end):
    raw = data[start:end]
    if tag in (0x0C, 0x13, 0x16, 0x1A):  # UTF8String, PrintableString, IA5String, VisibleString
        return raw.decode("utf-8")
    if tag == 0x1E:  # BMPString
        return raw.decode("utf-16-be")
    raise Refused("DER: a name string of tag 0x%02x is not handled" % tag)


def der_name(data, start, end):
    """An X.501 Name as `C=US, O=..., CN=...` - openssl's one-line form (RFC 4514 order kept)."""
    parts = []
    for tag, rs, re_ in der_children(data, start, end):  # each RDN is a SET
        if tag != 0x31:
            raise Refused("DER: an RDN that is not a SET")
        for atag, as_, ae in der_children(data, rs, re_):  # AttributeTypeAndValue
            if atag != 0x30:
                raise Refused("DER: an attribute that is not a SEQUENCE")
            items = der_children(data, as_, ae)
            if len(items) != 2 or items[0][0] != 0x06:
                raise Refused("DER: an attribute without an OID and a value")
            oid = der_oid(data, items[0][1], items[0][2])
            value = der_string(data, items[1][0], items[1][1], items[1][2])
            parts.append("%s=%s" % (OID_NAMES.get(oid, oid), value))
    return ", ".join(parts)


def der_time(data, tag, start, end):
    """notAfter as YYYY-MM-DD (UTCTime YYMMDDhhmmssZ or GeneralizedTime YYYYMMDDhhmmssZ)."""
    text = data[start:end].decode("ascii")
    if tag == 0x17:
        yy = int(text[0:2])
        year = 2000 + yy if yy < 50 else 1900 + yy  # RFC 5280 section 4.1.2.5.1
        return "%04d-%s-%s" % (year, text[2:4], text[4:6])
    if tag == 0x18:
        return "%s-%s-%s" % (text[0:4], text[4:6], text[6:8])
    raise Refused("DER: a time of tag 0x%02x" % tag)


def certificate_facts(der):
    """(subject one-line, notAfter YYYY-MM-DD) from a DER certificate."""
    tag, cs, ce, _ = der_tlv(der, 0)
    if tag != 0x30:
        raise Refused("DER: the certificate is not a SEQUENCE")
    tbs_tag, ts, te, _ = der_tlv(der, cs)
    if tbs_tag != 0x30:
        raise Refused("DER: tbsCertificate is not a SEQUENCE")
    fields = der_children(der, ts, te)
    at = 0
    if fields[at][0] == 0xA0:  # [0] EXPLICIT version
        at += 1
    # serialNumber, signature, issuer, validity, subject
    serial, sigalg, issuer, validity, subject = fields[at:at + 5]
    del serial, sigalg, issuer
    times = der_children(der, validity[1], validity[2])
    if len(times) != 2:
        raise Refused("DER: validity without two times")
    not_after = der_time(der, times[1][0], times[1][1], times[1][2])
    return der_name(der, subject[1], subject[2]), not_after


# ---- the PEM files and their headers -------------------------------------------------------------

REQUIRED = ("subject", "sha256", "expires", "source", "checked")


def read_root(path):
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    header = {}
    name_lines = []
    pem = []
    in_pem = False
    for line in lines:
        if line.startswith("-----BEGIN CERTIFICATE-----"):
            in_pem = True
        if in_pem:
            if line:
                pem.append(line)
            if line.startswith("-----END CERTIFICATE-----"):
                in_pem = False
            continue
        if not line.strip():
            continue
        if not line.startswith("#"):
            raise Refused("%s: a line before the certificate that is not a '#' header: %r" % (path, line))
        text = line[1:].strip()
        m = re.match(r"^([a-z0-9]+):\s*(.*)$", text)
        if m and m.group(1) in REQUIRED + ("chains",):
            header[m.group(1)] = m.group(2).strip()
        else:
            name_lines.append(text)
    if not pem or pem[0] != "-----BEGIN CERTIFICATE-----" or pem[-1] != "-----END CERTIFICATE-----":
        raise Refused("%s: no single PEM certificate" % path)
    for key in REQUIRED:
        if key not in header:
            raise Refused("%s: the header lacks '%s:'" % (path, key))
    if not name_lines:
        raise Refused("%s: the header's first line should name the root" % path)
    body = "".join(pem[1:-1])
    try:
        der = base64.b64decode(body, validate=True)
    except Exception as e:  # noqa: BLE001
        raise Refused("%s: the PEM body is not base64: %s" % (path, e))
    fingerprint = hashlib.sha256(der).hexdigest()
    if not re.fullmatch(r"[0-9a-f]{64}", header["sha256"]):
        raise Refused("%s: sha256 must be 64 lowercase hex characters" % path)
    if fingerprint != header["sha256"]:
        raise Refused("%s: the header says sha256 %s but the certificate's is %s" % (path, header["sha256"], fingerprint))
    subject, not_after = certificate_facts(der)
    if subject != header["subject"]:
        raise Refused("%s: the header says subject %r but the certificate's is %r" % (path, header["subject"], subject))
    if not_after != header["expires"]:
        raise Refused("%s: the header says expires %s but the certificate says %s" % (path, header["expires"], not_after))
    for key in ("checked",):
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", header[key]):
            raise Refused("%s: %s must be YYYY-MM-DD" % (path, key))
    for line in pem:
        if any(ord(c) < 0x20 or ord(c) > 0x7E for c in line):
            raise Refused("%s: a PEM line with a character that is not printable ASCII" % path)
    return {
        "file": os.path.basename(path),
        "name": name_lines[0].split(" - ")[0].strip(),
        "subject": header["subject"],
        "sha256": header["sha256"],
        "expires": header["expires"],
        "source": header["source"],
        "checked": header["checked"],
        "chains": header.get("chains", ""),
        "pem_lines": pem,
        "der_size": len(der),
    }


def c_string(value):
    """A C string literal: printable ASCII, a newline escaped, nothing else."""
    out = []
    for ch in value:
        if ch in ('"', "\\"):
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif " " <= ch <= "~":
            out.append(ch)
        else:
            raise Refused("a header string with a character that is not printable ASCII: %r" % value)
    return '"' + "".join(out) + '"'


def render(roots, source_dir):
    lines = [
        "// Generated by tools/gen_tls_roots.py from %s/*.pem. Do not edit; it is rewritten by every build." % source_dir,
        "//",
        "// The roots this firmware trusts a server's certificate chain to end at (task p3-02,",
        "// core/net/trust.h). Each with its subject, SHA-256 fingerprint over the DER, expiry, source and",
        "// the date it was checked - held against the certificate itself by the generator - and its PEM,",
        "// which is what the modem's certificate slot is written with. Ordered by file name.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "",
        '#include "core/net/trust.h"',
        "",
        "namespace core {",
        "namespace net {",
        "namespace generated {",
        "",
        "inline constexpr std::size_t kTlsRootCount = %d;" % len(roots),
        "",
        "inline constexpr TlsRoot kTlsRoots[kTlsRootCount] = {",
    ]
    for r in roots:
        lines.append("    {")
        lines.append("        %s," % c_string(r["file"]))
        lines.append("        %s," % c_string(r["name"]))
        lines.append("        %s," % c_string(r["subject"]))
        lines.append("        %s," % c_string(r["sha256"]))
        lines.append("        %s," % c_string(r["expires"]))
        lines.append("        %s," % c_string(r["source"]))
        lines.append("        %s," % c_string(r["checked"]))
        lines.append("        %s," % c_string(r["chains"]))
        lines.append("        %d," % r["der_size"])
        for i, pem_line in enumerate(r["pem_lines"]):
            end = "," if i == len(r["pem_lines"]) - 1 else ""
            lines.append("        %s%s" % (c_string(pem_line + "\n"), end))
        lines.append("    },")
    lines += [
        "};",
        "",
        "}  // namespace generated",
        "}  // namespace net",
        "}  // namespace core",
        "",
    ]
    return "\n".join(lines)


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    source_dir, out = argv[1], argv[2]
    try:
        files = sorted(f for f in os.listdir(source_dir) if f.endswith(".pem"))
        if not files:
            raise Refused("%s: no .pem files" % source_dir)
        roots = [read_root(os.path.join(source_dir, f)) for f in files]
        seen = set()
        for r in roots:
            if r["sha256"] in seen:
                raise Refused("%s appears twice" % r["file"])
            seen.add(r["sha256"])
        text = render(roots, source_dir.replace("\\", "/").rstrip("/"))
    except Refused as e:
        print("gen_tls_roots: refused: %s" % e, file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as fh:
        fh.write(text)
    for r in roots:
        print("tls root %s: %s, sha256 %s, expires %s, checked %s" % (
            r["file"], r["name"], r["sha256"][:16], r["expires"], r["checked"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
