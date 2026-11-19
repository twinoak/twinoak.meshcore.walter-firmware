# Phase 17 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[Thomas, upstream] The modem library keeps a debug buffer in every image.**
  `_logEscapeBuffer`, 3 080 bytes of `.bss`, is in the builds of 16 and 19 November although
  the maximum log level is INFO. The library declares and uses it under
  `#if CONFIG_LOG_MAXIMUM_LEVEL >= ESP_LOG_DEBUG`
  (`managed_components/dptechnics__walter-modem/src/WalterModem.cpp` ~L180, ~L1782, ~L2052).
  `ESP_LOG_DEBUG` looks to be an enum value, which a preprocessor reads as 0, so the test is
  always true; not checked in the pinned ESP-IDF. If so, every line the modem says is also
  escaped into that buffer for a log line nothing prints. QuickSpot's `walter-esp-idf` 1.5.0 to
  fix; not removable here without a copy of the library.
- [ ] **[later] The modem library's other static storage is 46 kB**, by the map of 19 November:
  the event queue 12 376 (eight items of about 1 547 bytes, for a ring or a close that is a few
  bytes), the buffer pool 12 352 in `.data`, the command pool 10 944, and three task stacks of
  8 192, 4 096 and 4 096. The queue's length, the pool's size and the stacks are the library's
  Kconfig. One too few loses a line or an event without a word, so the high-water marks of a
  board come first.
- [ ] **[later, found by 01] The byte bound refuses a big release before the image bound does, and
  with the wrong reason.** `kMaxManifestBytes` is 8 192 (`core/image/receive.h` ~L92) and a manifest
  filled to task 01's bounds — six images of five files — is **10 302 bytes** of canonical JSON, so a
  release of more than about four images of four files is refused for its size before the parser sees
  it, rather than at check 2 with the bound it broke. Today's biggest release is two images of four
  files (3.4 KB), so nothing is refused today. The constant was out of 01's scope; only `receive.h`'s
  comment, which claimed 8 KB was "anything this firmware could accept", was corrected. Whoever
  takes this up: the two 8 KB buffers of the item below are the same constant.
- [ ] **[later] A manifest's bytes seem to be held twice.** `fetch` is 10 104 bytes of `.bss` and
  `receiver` 9 424, and `kMaxManifestBytes` is 8 192 (`core/image/receive.h` ~L92;
  `core/update/fetch.h` ~L145: *static storage: the manifest, its signature, the URL*). The
  cable and the server are not read at the same time. Read from the map and the two headers,
  not from the code that uses them.
- [ ] **[later] meshmaster.dk's catalogue does not know this firmware's bounds.** A release of
  seven images imports there and no Walter can read it; the unit says so at check 2 and the
  page shows the refusal. Nothing warns at the import.
- [ ] **[review] `FirmwareApplyProgress.json` in `host_test/fixtures/contracts-ahead/` is not
  meshmaster.dk's file** (compared on 19 November; from pile 14). Here it is the whole post,
  `{"data":{"journal":[…]},"note":…}`; there it is the `data` object alone. The same wire
  under one name for two things, most likely; to be confirmed against what the unit posts.
  The six waiting files of pile 16 and the two of pile 13 are the same bytes as meshmaster.dk's.
- [ ] **[later] Piles 12 to 16 are not reviewed.**
