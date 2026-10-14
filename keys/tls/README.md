# The TLS roots

The roots this firmware trusts a server's certificate chain to end at (task p3-02; `spec/SYSTEM.md`
S-12: *TLS is verified*). One PEM a file, each with a header the build holds against the
certificate itself, after the agent firmware's `src/agent/TlsRoots.h`:

```
# <name> - one line on what it is for
# subject: <the certificate's subject, as openssl prints it>
# sha256: <64 lowercase hex: SHA-256 over the DER>
# expires: YYYY-MM-DD   (the certificate's notAfter)
# source: <where the PEM was fetched from>
# checked: YYYY-MM-DD   (when the chains below were read)
# chains: <the chains observed to end here, on that date>
-----BEGIN CERTIFICATE-----
...
```

`tools/gen_tls_roots.py` turns the directory into `tls_roots.h` at build time (both CMake
projects run it) and **refuses to write the table** when a header's fingerprint, subject or expiry
is not the certificate's - a root pasted with the wrong header fails the build, not a unit.
`host_test/core/net/test_trust.cpp` holds the fingerprints a second time through the firmware's
own SHA-256 and base64, and `trust tls` on the console prints them on a board.

| File | Root | Ends the chains of | Expires |
|---|---|---|---|
| `isrg-root-x1.pem` | ISRG Root X1 (Let's Encrypt) | `meshmaster.dk`; `release-assets.githubusercontent.com` and `objects.githubusercontent.com`, where a GitHub release download redirects to | 2035-06-04 |
| `usertrust-ecc.pem` | USERTrust ECC Certification Authority (Sectigo) | `github.com`, the first hop of a release download | 2038-01-18 |

The chains were read on 2026-10-14 with `openssl s_client -showcerts`; the report of task p3-02
has them in full. Each was then verified against its one root alone (`-no-CAfile -no-CApath
-CAfile <root>`), and each against the *other* root alone, which fails - so the host table in
`components/core/net/trust.h` (`github.com` to USERTrust, everything else to ISRG) is what the
observed chains need and nothing more.

## How the roots reach the modem

The modem does the TLS, and validates a connection against **one** certificate slot per TLS
profile (`AT+SQNSPCFG` takes one `caCertificateID`). So root *i* is written into certificate slot
12 + *i* and validated by TLS profile 2 + *i* - the numbers are `core/net/trust.h`'s, with their
reason (the library's own example says slots 0-10 and profile 1 are Sequans's and BlueCherry's) -
and a host is held to one root. The loader (`core::net::TrustLoader`) writes a slot **once**: it
keeps the SHA-256 of what it wrote in retained storage and rewrites only what differs, so a unit's
modem flash is written on its first boot, after a power cut, and when a release changes a root.

## Replacing a root

Room for three (`kMaxTlsRoots`), as with the release keys: add the new PEM with its header, ship a
build that carries both, then one that carries only the new. The file names sort into the table's
order and `trust.cpp` holds each named index to its file with a `static_assert`, so a file whose
name sorts between the two existing ones is a failed build rather than a moved root.

A host whose chain ends at a root this firmware does not carry **fails its handshake**. That is
the point, and `docs/bench.md` §12 tests it on the modem with such a host.
