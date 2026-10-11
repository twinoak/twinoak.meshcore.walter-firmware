#!/usr/bin/env python3
"""Make, or check, the verification-only extract of TweetNaCl this firmware vendors.

    python tools/vendor_tweetnacl_verify.py make  <tweetnacl.c> <out.c>
    python tools/vendor_tweetnacl_verify.py check <tweetnacl.c> <extract.c>
    python tools/vendor_tweetnacl_verify.py selfcheck <extract.c>      (no original: tools/verify.sh)

components/core/third_party/tweetnacl/tweetnacl_verify.c holds the functions crypto_sign_open
needs - the field arithmetic, the point arithmetic, the scalar reduction, SHA-512 and
crypto_sign_open itself - copied from tweetnacl.c 20140427 **byte for byte**, in the order they
have there, and nothing of the rest: no signing, no key generation, no randombytes, no Salsa20,
Poly1305, box, secretbox or X25519. The one edit is in the table of field constants, where
`_121665` (X25519's Montgomery constant) is left out so that an unused `static const` does not
fail the build; the five curve constants around it are untouched.

`make` writes the extract from a copy of the original; `check` reads an extract and the original
and fails unless every kept span is verbatim and every dropped function is absent. The original
is not kept in the tree (it would put signing code there); its SHA-256 is in the extract's
header and in third_party/tweetnacl/README.md, and this script re-derives the file from it, so a
reviewer with the original can see the extract is what it claims to be.
"""

import hashlib
import sys

ORIGINAL_SHA256 = "02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4"

# The spans kept, as (first line, last line) of the original, matched by their text so that the
# line numbers are evidence and not a dependency. Each is a whole definition.
KEPT = [
    "#define FOR(i,n) for (i = 0;i < n;++i)",
    "#define sv static void",
    "typedef unsigned char u8;",
    "typedef unsigned long u32;",
    "typedef unsigned long long u64;",
    "typedef long long i64;",
    "typedef i64 gf[16];",
    "static u64 dl64(const u8 *x)",
    "sv ts64(u8 *x,u64 u)",
    "static int vn(const u8 *x,const u8 *y,int n)",
    "int crypto_verify_32(const u8 *x,const u8 *y)",
    "sv set25519(gf r, const gf a)",
    "sv car25519(gf o)",
    "sv sel25519(gf p,gf q,int b)",
    "sv pack25519(u8 *o,const gf n)",
    "static int neq25519(const gf a, const gf b)",
    "static u8 par25519(const gf a)",
    "sv unpack25519(gf o, const u8 *n)",
    "sv A(gf o,const gf a,const gf b)",
    "sv Z(gf o,const gf a,const gf b)",
    "sv M(gf o,const gf a,const gf b)",
    "sv S(gf o,const gf a)",
    "sv inv25519(gf o,const gf i)",
    "sv pow2523(gf o,const gf i)",
    "static u64 R(u64 x,int c) { return (x >> c) | (x << (64 - c)); }",
    "static u64 Ch(u64 x,u64 y,u64 z) { return (x & y) ^ (~x & z); }",
    "static u64 Maj(u64 x,u64 y,u64 z) { return (x & y) ^ (x & z) ^ (y & z); }",
    "static u64 Sigma0(u64 x) { return R(x,28) ^ R(x,34) ^ R(x,39); }",
    "static u64 Sigma1(u64 x) { return R(x,14) ^ R(x,18) ^ R(x,41); }",
    "static u64 sigma0(u64 x) { return R(x, 1) ^ R(x, 8) ^ (x >> 7); }",
    "static u64 sigma1(u64 x) { return R(x,19) ^ R(x,61) ^ (x >> 6); }",
    "static const u64 K[80] = ",
    "int crypto_hashblocks(u8 *x,const u8 *m,u64 n)",
    "static const u8 iv[64] = {",
    "int crypto_hash(u8 *out,const u8 *m,u64 n)",
    "sv add(gf p[4],gf q[4])",
    "sv cswap(gf p[4],gf q[4],u8 b)",
    "sv pack(u8 *r,gf p[4])",
    "sv scalarmult(gf p[4],gf q[4],const u8 *s)",
    "sv scalarbase(gf p[4],const u8 *s)",
    "static const u64 L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};",
    "sv modL(u8 *r,i64 x[64])",
    "sv reduce(u8 *r)",
    "static int unpackneg(gf r[4],const u8 p[32])",
    "int crypto_sign_open(u8 *m,u64 *mlen,const u8 *sm,u64 n,const u8 *pk)",
]

# The field constants: the original's declaration minus _121665 (and the two byte arrays _0 and
# _9, which are Salsa20's and X25519's and live in a declaration of their own).
CONSTANTS_ORIGINAL = """static const gf
  gf0,
  gf1 = {1},
  _121665 = {0xDB41,1},
  D = {"""
CONSTANTS_KEPT = """static const gf
  gf0,
  gf1 = {1},
  D = {"""

# What must not be in the extract: the functions of the original that sign, make keys, or belong
# to the other primitives.
DROPPED = [
    "randombytes", "crypto_sign_keypair", "int crypto_sign(", "crypto_box", "crypto_secretbox",
    "crypto_stream", "crypto_onetimeauth", "crypto_core_salsa20", "crypto_core_hsalsa20",
    "crypto_scalarmult", "_121665", "static const u8 sigma", "add1305", "minusp",
    "crypto_verify_16", "sv core(", "L32(", "ld32(", "st32(",
]

HEADER = """/* TweetNaCl, verification only - a verbatim extract (task p2-01).
 *
 * Origin:   TweetNaCl 20140427, https://tweetnacl.cr.yp.to/20140427/tweetnacl.c
 *           Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange, Peter Schwabe,
 *           Sjaak Smetsers: "TweetNaCl: a crypto library in 100 tweets" (2014).
 * Licence:  public domain (the authors' statement: "TweetNaCl is public domain", on the page
 *           above and in the paper).
 * SHA-256 of the original tweetnacl.c:  %(sha)s
 *
 * What is here: the functions crypto_sign_open depends on - the field arithmetic over 2^255-19,
 * the Edwards point arithmetic, the scalar reduction modulo L, SHA-512 - and crypto_sign_open
 * itself, each copied byte for byte and in the order the original has them. What is not here:
 * crypto_sign (signing), crypto_sign_keypair, randombytes, Salsa20, Poly1305, box, secretbox,
 * X25519 (crypto_scalarmult). The one edit is in the table of field constants, where X25519's
 * `_121665` is left out so that an unused static const does not fail a build with
 * -Werror=unused-variable; the curve constants around it are untouched.
 * tools/vendor_tweetnacl_verify.py re-derives this file from the original and checks it.
 *
 * The exported names are the ones tweetnacl.h gives them (crypto_sign_ed25519_tweet_open, ...),
 * so that this extract has exactly the symbols a full TweetNaCl build would have for these
 * functions and none of the others; tools/verify.sh greps the linker map for the absent ones.
 * This file is C (the original is C): the firmware and the host tests compile it with the
 * same flags as our own code except -Wno-sign-compare, which the original's loops need, and on
 * the host -fno-sanitize=shift-base, because car25519 and modL left-shift negative values -
 * defined behaviour on every compiler this project uses, undefined to UBSan.
 *
 * What TweetNaCl's crypto_sign_open does NOT check, and core/image/crypto/ed25519.cpp therefore
 * checks before calling in here: that the signature's S is below the group order L (RFC 8032
 * section 5.1.7 step 1). A signature with S + L in place of S verifies here and is refused there;
 * host_test/core/image/test_ed25519.cpp shows both.
 */

/* The names tweetnacl.h gives the four functions this file defines with external linkage. */
#define crypto_verify_32 crypto_verify_32_tweet
#define crypto_hashblocks crypto_hashblocks_sha512_tweet
#define crypto_hash crypto_hash_sha512_tweet
#define crypto_sign_open crypto_sign_ed25519_tweet_open

/* ---- From here to the marked line below: tweetnacl.c 20140427, verbatim ---------------------- */

"""

FOOTER = """
/* ---- End of the verbatim extract. Below this line is TwinOak's (task p2-01) ----------------- */

/* The entry points core/image/crypto uses, so that the static functions above stay static.
 * twinoak_tweetnacl_ed25519_check is the tail of crypto_sign_open with the SHA-512 of
 * R || A || M supplied by the caller (core/image/crypto/sha512.cpp streams it, so that a manifest
 * need not be copied into a second buffer): it returns 0 when the signature verifies. */

void twinoak_tweetnacl_sha512_iv(u8 h[64])
{
  int i;
  FOR(i,64) h[i] = iv[i];
}

int twinoak_tweetnacl_hashblocks(u8 *x,const u8 *m,u64 n)
{
  return crypto_hashblocks(x,m,n);
}

int twinoak_tweetnacl_hash(u8 *out,const u8 *m,u64 n)
{
  return crypto_hash(out,m,n);
}

int twinoak_tweetnacl_sign_open(u8 *m,u64 *mlen,const u8 *sm,u64 n,const u8 *pk)
{
  return crypto_sign_open(m,mlen,sm,n,pk);
}

int twinoak_tweetnacl_ed25519_check(const u8 pk[32],const u8 sig[64],const u8 hram[64])
{
  int i;
  u8 t[32],h[64];
  gf p[4],q[4];

  if (unpackneg(q,pk)) return -1;

  FOR(i,64) h[i] = hram[i];
  reduce(h);
  scalarmult(p,q,h);

  scalarbase(q,sig + 32);
  add(p,q);
  pack(t,p);

  return crypto_verify_32(sig, t);
}
"""


def spans(lines):
    """The kept spans of the original, as lists of lines, in order."""
    out = []
    i = 0
    n = len(lines)
    wanted = list(KEPT)
    while i < n and wanted:
        line = lines[i]
        if line.rstrip("\n") == wanted[0]:
            wanted.pop(0)
            if line.startswith("#define") or line.startswith("typedef") or (line.startswith("static u64 ") and "{ return" in line) or line.startswith("static const u64 L["):
                out.append([line])
                i += 1
                continue
            # a multi-line definition: to the first line that is exactly "}" or "};" or "} ;"
            span = [line]
            i += 1
            while i < n:
                span.append(lines[i])
                if lines[i].rstrip("\n") in ("}", "};", "} ;"):
                    break
                i += 1
            out.append(span)
            i += 1
            continue
        i += 1
    if wanted:
        raise SystemExit("not found in the original: %r" % wanted[0])
    return out


def constants(text):
    start = text.index(CONSTANTS_ORIGINAL)
    end = text.index("};", start) + len("};")
    block = text[start:end]
    return block.replace(CONSTANTS_ORIGINAL, CONSTANTS_KEPT, 1)


def make(original_path, out_path):
    with open(original_path, "rb") as fh:
        data = fh.read()
    sha = hashlib.sha256(data).hexdigest()
    if sha != ORIGINAL_SHA256:
        raise SystemExit("%s is not tweetnacl.c 20140427: sha256 %s, expected %s" % (original_path, sha, ORIGINAL_SHA256))
    text = data.decode("ascii")
    lines = text.splitlines(keepends=True)
    kept = spans(lines)
    body = []
    for span in kept:
        if span[0].startswith("sv ts64"):
            # the constants come after the typedefs and before the first function, as in the original
            pass
        body.append("".join(span))
    # the typedef block ends with gf; the constants follow it in the original
    pieces = []
    for span_text in body:
        pieces.append(span_text)
        if span_text.startswith("typedef i64 gf[16];"):
            pieces.append("\n" + constants(text) + "\n")
    joined = "\n".join(p.rstrip("\n") + "\n" for p in pieces)
    out = HEADER % {"sha": sha} + joined + FOOTER
    with open(out_path, "w", encoding="ascii", newline="\n") as fh:
        fh.write(out)
    print("wrote %s: %d spans kept" % (out_path, len(kept)))


def check(original_path, extract_path):
    with open(original_path, "rb") as fh:
        data = fh.read()
    sha = hashlib.sha256(data).hexdigest()
    if sha != ORIGINAL_SHA256:
        raise SystemExit("%s is not tweetnacl.c 20140427 (sha256 %s)" % (original_path, sha))
    original = data.decode("ascii")
    with open(extract_path, "r", encoding="ascii") as fh:
        extract = fh.read()
    if sha not in extract:
        raise SystemExit("the extract's header does not name the original's sha256")
    begin = extract.index("verbatim ----")
    begin = extract.index("\n", begin) + 1
    end = extract.index("/* ---- End of the verbatim extract")
    verbatim = extract[begin:end]
    # Every definition in the verbatim part is a span of the original.
    kept = spans(original.splitlines(keepends=True))
    problems = 0
    for span in kept:
        s = "".join(span)
        if s not in verbatim:
            print("NOT VERBATIM: %s" % span[0].rstrip())
            problems += 1
        if s not in original:
            print("NOT IN THE ORIGINAL: %s" % span[0].rstrip())
            problems += 1
    if constants(original) not in verbatim:
        print("NOT VERBATIM: the field constants (minus _121665)")
        problems += 1
    # Nothing in the verbatim part that is not from a kept span or the constants.
    rest = verbatim
    for span in kept:
        rest = rest.replace("".join(span), "", 1)
    rest = rest.replace(constants(original), "", 1)
    if rest.strip():
        print("UNEXPLAINED TEXT in the verbatim part:\n%s" % rest.strip())
        problems += 1
    # The header above the code names what was dropped; the code itself must not contain it.
    code = extract[begin:]
    for word in DROPPED:
        if word in code:
            print("DROPPED FUNCTION PRESENT: %s" % word)
            problems += 1
    if problems:
        raise SystemExit("%d problem(s)" % problems)
    print("%s: %d spans verbatim from tweetnacl.c 20140427, the dropped functions absent" % (extract_path, len(kept)))


def selfcheck(extract_path):
    """Without the original (which is not in the tree): the extract names the original's hash,
    defines every kept function and none of the dropped ones. tools/verify.sh runs this; `check`
    with the original is the stronger form a reviewer runs once."""
    with open(extract_path, "r", encoding="ascii") as fh:
        extract = fh.read()
    problems = 0
    if ORIGINAL_SHA256 not in extract:
        print("the extract's header does not name the original's sha256")
        problems += 1
    begin = extract.index("verbatim ----")
    code = extract[begin:]
    for signature in KEPT:
        if signature not in code:
            print("KEPT DEFINITION MISSING: %s" % signature)
            problems += 1
    if CONSTANTS_KEPT not in code:
        print("the field constants are not as the extract describes them")
        problems += 1
    for word in DROPPED:
        if word in code:
            print("DROPPED FUNCTION PRESENT: %s" % word)
            problems += 1
    if problems:
        raise SystemExit("%d problem(s)" % problems)
    print("%s: %d kept definitions present, the %d dropped ones absent, the original's sha256 named"
          % (extract_path, len(KEPT), len(DROPPED)))


def main(argv):
    if len(argv) == 4 and argv[1] == "make":
        make(argv[2], argv[3])
        return 0
    if len(argv) == 4 and argv[1] == "check":
        check(argv[2], argv[3])
        return 0
    if len(argv) == 3 and argv[1] == "selfcheck":
        selfcheck(argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
