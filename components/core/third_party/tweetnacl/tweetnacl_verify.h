/* The entry points of the TweetNaCl verification-only extract (tweetnacl_verify.c), for
 * core/image/crypto/. Everything a caller needs is here; the field, point and scalar functions
 * stay static inside the .c file. See tweetnacl_verify.c's header and README.md for the origin,
 * the licence and what was kept. */
#ifndef TWINOAK_TWEETNACL_VERIFY_H
#define TWINOAK_TWEETNACL_VERIFY_H

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-512 (FIPS 180-4), as TweetNaCl keeps it: the state is the 64 digest bytes, big-endian. */

/* The initial state. */
void twinoak_tweetnacl_sha512_iv(unsigned char h[64]);

/* Compresses every whole 128-byte block of m into h and returns how many bytes were left over
 * (fewer than 128); the caller keeps them for the next call or for the padding. */
int twinoak_tweetnacl_hashblocks(unsigned char *h, const unsigned char *m, unsigned long long n);

/* The whole hash in one call - the original crypto_hash, kept so that the streaming class can be
 * held against it. */
int twinoak_tweetnacl_hash(unsigned char out[64], const unsigned char *m, unsigned long long n);

/* Ed25519 verification (RFC 8032), with the SHA-512 of R || A || M supplied by the caller:
 * returns 0 when sig (R || S) verifies for public key pk, anything else when it does not.
 * Does NOT check that S < L: core/image/crypto/ed25519.cpp does, first. */
int twinoak_tweetnacl_ed25519_check(const unsigned char pk[32], const unsigned char sig[64],
                                    const unsigned char hram[64]);

/* The original crypto_sign_open: sm is sig || message of n bytes, m a buffer of n bytes; returns 0
 * and the message length when it verifies. Kept for the host tests to hold the streaming path
 * against; the firmware does not call it. */
int twinoak_tweetnacl_sign_open(unsigned char *m, unsigned long long *mlen, const unsigned char *sm,
                                unsigned long long n, const unsigned char pk[32]);

#ifdef __cplusplus
}
#endif

#endif
