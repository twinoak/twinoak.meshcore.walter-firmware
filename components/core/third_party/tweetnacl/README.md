# TweetNaCl, verification only

| | |
|---|---|
| Origin | TweetNaCl 20140427, <https://tweetnacl.cr.yp.to/20140427/tweetnacl.c> and `tweetnacl.h` beside it; Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers, *TweetNaCl: a crypto library in 100 tweets* (2014) |
| Version | 20140427, the only release |
| Licence | public domain (the authors' statement on the page and in the paper) |
| SHA-256 of the original `tweetnacl.c` | `02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4` |
| SHA-256 of the original `tweetnacl.h` | `43f29ad721d9927b747b0100ab4160c119e7bb180c7c98a66e4bf79d31244287` |
| Vendored here | `tweetnacl_verify.c`: the verification path, verbatim; `tweetnacl_verify.h`: our declarations of its entry points |
| Made and checked by | `tools/vendor_tweetnacl_verify.py make|check <tweetnacl.c> tweetnacl_verify.c` |

## What is kept, and what is not

`tweetnacl_verify.c` holds the 45 definitions `crypto_sign_open` depends on — the field
arithmetic over 2^255−19, the Edwards point arithmetic, the scalar reduction modulo L, SHA-512 —
and `crypto_sign_open` itself, copied byte for byte and in the original's order. The script
`tools/vendor_tweetnacl_verify.py check` fails unless every one of them is a verbatim span of a
`tweetnacl.c` with the hash above, unless the verbatim part contains nothing else, and unless
every dropped function is absent.

Not here: `crypto_sign` (signing), `crypto_sign_keypair`, `randombytes`, Salsa20, Poly1305,
`crypto_box`, `crypto_secretbox`, `crypto_stream`, `crypto_onetimeauth`, `crypto_scalarmult`
(X25519). The one edit: the declaration of the field constants omits X25519's `_121665`, which
would otherwise be an unused `static const` and a failed build under `-Werror=unused-variable`;
the five curve constants around it are untouched.

The four exported functions keep the names `tweetnacl.h` gives them (`crypto_verify_32_tweet`,
`crypto_hashblocks_sha512_tweet`, `crypto_hash_sha512_tweet`, `crypto_sign_ed25519_tweet_open`),
so that the linker map of a firmware build has exactly the symbols a full TweetNaCl would have for
these functions and none of the others; `tools/verify.sh` greps it for `crypto_sign_ed25519_tweet`
(the signer), `crypto_sign_ed25519_tweet_keypair` and `randombytes`.

Below the marked line, `tweetnacl_verify.c` has five small functions of ours: the SHA-512 initial
state and block function and the one-shot hash handed out under our names, the original
`crypto_sign_open` handed out for the tests, and `twinoak_tweetnacl_ed25519_check`, which is the
tail of `crypto_sign_open` with the hash of `R || A || M` supplied by the caller — so that
`core/image/crypto/sha512.cpp` can stream a manifest into the hash instead of copying it into the
`sm` buffer the original wants.

## What it does not check, and where that is done

`crypto_sign_open` does not reject a signature whose scalar `S` is not below the group order
`L` (RFC 8032 §5.1.7, step 1: "decode the second half as an integer S, in the range 0 ≤ s < L
... if any of the decodings fail (including S being out of range), the signature is invalid").
`S + L` verifies as `S` does. `core/image/crypto/ed25519.cpp` checks `S < L` before calling in
here, and `host_test/core/image/test_ed25519.cpp` shows both: the extract accepting `S + L`, the
wrapper refusing it.

## How it is compiled

As C, by the firmware build and by the host tests, with the same flags as our own code except
`-Wno-sign-compare` (the original's loops compare an `int` or `u32` index with a `u64` count) and,
on the host only, `-fno-sanitize=shift-base`: `car25519` and `modL` left-shift negative `long
long` values, which every compiler this project uses defines as the obvious thing and which
UndefinedBehaviorSanitizer reports. The rest of ASan and UBSan apply to it as to everything else.
