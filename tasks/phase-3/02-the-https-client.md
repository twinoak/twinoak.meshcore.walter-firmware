# 02 — The HTTPS client

**Depends on:** 01.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §9; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-8, S-12 ("TLS is verified"); [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §3 and §5 (where a release's files are and what is checked on arrival); the `walter-modem` component's `tlsConfigProfile`, `tlsWriteCredential`, `httpConfigProfile`, `httpQuery`, `httpSend`, `httpReceive`, the HTTP event handler, and its `examples/https`; the agent firmware's `src/agent/TlsRoots.h` at `51faafb427cba41b0de9983736b6621bc155f829` (read it with `git -C ../meshmaster-agent-firmware show`) for how a root is recorded with its fingerprint and the date it was checked.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

The Walter can ask a server a small question and fetch a large file, over TLS
whose certificate chain **was checked**. The modem does the TLS, and its
library's default for a TLS profile is *validate nothing*; one forgotten
argument and every unit trusts whoever answers. This task makes the verified
profile the only one this firmware can construct, and proves it with a test
rather than a sentence.

The signature on a manifest (phase 2, task 01) is what makes an image safe to
flash; TLS is what keeps a token from a stranger. Both hold, neither stands in
for the other.

## Read first

How the modem's HTTP works through the library: profiles (three), the TLS
security profiles (six) and the certificate slots, what `httpQuery` returns
and when the answer arrives (the "ring" and `httpReceive`), the size limits
on a request body and on what one `httpReceive` returns, what an extra header
line may contain and how many there may be, whether a response's headers are
available at all, and what the header calls buggy. **Find out whether a
`Range` request header can be sent and whether `Content-Range` or the status
code comes back** — the design of the download depends on it. If the modem's
HTTP cannot do ranges, say so, and build the download on the library's TLS
sockets instead (`socketConfigSecure` and friends) with a minimal HTTP/1.1
request writer and response parser in `core/` — that parser then gets the
tests below and a fuzz loop. Decide once, in the report, with the evidence.

## Deliverables

- **`components/hal/Http`** — the interface: a request (method, host, path,
  a small fixed number of headers, an optional body) → status, a small set of
  response headers the caller named, and the body **as a stream of pieces**
  handed to a sink; a deadline on the whole and on each piece.
- **Trust** (`components/core/net/Trust` + `keys/tls/`):
  - the roots this firmware trusts, each a PEM in the tree with a header
    recording its subject, its SHA-256 fingerprint, its expiry, where it was
    fetched from and the date — as the agent firmware does. **ISRG Root X1**
    (meshmaster.dk's chain ends there) and whatever GitHub's release downloads
    need: releases redirect from `github.com` to another host, so follow one
    by hand from the session, read the chains of both hosts, and record the
    roots. A host test checks that each header's fingerprint is the PEM's;
  - loading them into the modem's certificate slots: **once**, when the slot's
    content differs (compare a hash kept in settings, do not rewrite modem
    flash at every wake), with the slot numbers as named constants;
  - **one** function that makes a TLS profile, and it takes no validation
    argument: it always asks for the certificate chain **and** the host name
    to be checked, TLS 1.2 or newer.
- **`components/modem/`** — `Http` on the library, thin, using only that
  profile function.
- **`components/core/net/Client`** — portable, over `Http`:
  - `https://` only — a URL with any other scheme is refused before the modem
    is touched (the agent firmware's rule);
  - redirects: followed, at most three, **only to `https://`**, and an
    `Authorization` header is **never** carried to a different host;
  - status handling: 2xx; 3xx as above; 401/403 reported as *denied* and not
    retried in this wake; 429 and 5xx as *later*, honouring `Retry-After`
    within a cap; everything else *failed*;
  - a response larger than the caller's bound is cut off and reported, not
    buffered.
- **`components/core/net/Download`** — a file by URL into a sink, with its
  expected size and SHA-256 known beforehand (from a manifest that already
  passed checks 1–3):
  - in ranges of a named size, each with its own deadline and retries;
  - **resumable**: the sink says how many bytes it holds and with what running
    hash state (the image store's resume of phase 2, task 02); a wake that
    ends mid-file continues in the next;
  - the server must honour the range (206 with the right `Content-Range`, or
    the fallback decided above); a 200 with the whole body in answer to a
    range request restarts from zero rather than appending garbage;
  - an `ETag` or `Last-Modified` seen on the first range is sent as `If-Range`
    after — a file replaced under the same URL restarts, it does not splice;
  - the hash and size are checked at the end by the store's own check 4, not
    by this class; this class only refuses to deliver more bytes than
    expected;
  - a budget in bytes and seconds per wake, handed in.
- **`components/core/update/Fetch`** — the three steps a fetch of a release
  is, over `Client`, `Download` and phase 2's verdict: get `manifest.json` and
  `manifest.json.sig` (small, bounded) → checks 1–3 → download the one file
  into the candidate slot → checks 4–6. From a base URL the caller gives; this
  task does not decide where releases live or who says there is a new one.
- **`host_test/sim/FakeHttp`** — a scripted server: files by path, range
  support that can be switched off, redirects, statuses, a connection that
  drops after N bytes, a body one byte too long, slowness.
- **The console**: `http get <url>` (status, the named headers, the first
  bytes as hex, the size, the time), `fetch <base-url> [candidate|scanner]`
  (the three steps, printing each check as it passes), `trust tls` (the roots,
  their fingerprints and expiry, and whether the modem's slots match).

## The rail

- A test that fails if the token `WALTER_MODEM_TLS_VALIDATION_NONE`, or any
  validation level other than the full one, appears anywhere outside the
  vendored component; and that `tlsConfigProfile` is called from exactly one
  place. `tools/verify.ps1` runs it.
- No `http://` literal in the firmware's sources outside a test and a comment.
- The token for a server (task 03) is never in a URL, never in a log line; a
  test greps the log sink after a request that carried one.

## Tests (host — fake HTTP, simulated flash)

- A small GET and a small POST; each status class; `Retry-After` honoured and
  capped; a body over the bound cut off.
- Redirects: one, three, four (refused), to `http://` (refused), to another
  host with an `Authorization` header (followed without it).
- A download: whole; in ranges; **a drop at every range boundary and in the
  middle of every range** (a loop) — the file arrives intact and no byte is
  fetched twice beyond the range in flight.
- Resume across a simulated restart of the Walter.
- A server that ignores ranges; one that answers 206 with a wrong
  `Content-Range`; one whose file changes between ranges (`If-Range`).
- A file longer than its manifest says; shorter; right length, wrong hash
  (refused at check 4, nothing stored, the candidate slot empty).
- `Fetch` against the spec's fixture release served by the fake: accepted in a
  test-key build; refused at check 1 with the normal trust table; the tampered
  manifest refused; the unknown-key signature refused — all before a single
  byte of the image is requested.
- The budgets: bytes and seconds, each stopping a download cleanly at a
  resumable point.
- If the socket path was chosen: the response parser against split headers,
  folded and oversized headers, chunked transfer encoding (refuse or handle —
  say which), and ten thousand random inputs without a fault under the
  sanitizers.

## Acceptance criteria

- [ ] `tools/verify.ps1` green, including the rail's checks.
- [ ] The report states, with the evidence from the library and the modem's documentation, whether ranges go through the modem's HTTP or through TLS sockets, and why.
- [ ] Every root in `keys/tls/` has its subject, fingerprint, expiry, source and date; the report shows the chains observed for meshmaster.dk and for a GitHub release download, and the date.
- [ ] There is exactly one call that configures a TLS profile, and it cannot express "do not validate".
- [ ] Range size, retries, deadlines and both budgets are named constants, listed as "to be set on the bench".

## Not verified here (Thomas, with a SIM)

Add to `docs/bench.md`, "11. HTTPS":

- `trust tls` after the first boot: the slots were written once; after a
  reboot they are not written again.
- `http get https://meshmaster.dk/` → 200. `http get` of a host with a
  certificate from a root this firmware does not carry (`https://example.com`
  will do if its chain ends elsewhere; otherwise any such host) → refused, as
  a TLS failure. **This is the test that the rail is real on the modem and not
  only in the source.**
- `http get https://expired.badssl.com/` and `https://wrong.host.badssl.com/`
  → both refused.
- `fetch` of the repeater's dry-run release from GitHub in a test-key build:
  the image lands in the candidate slot; pull power half-way and run it again
  — it resumes. Note the time and the bytes; they set the budgets.

## Out of scope

Who decides that there is something to fetch, and from where (the backend).
Client certificates. Uploads beyond a small body.
