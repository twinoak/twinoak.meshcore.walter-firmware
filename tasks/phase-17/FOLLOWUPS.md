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
  **Confirmed from the other side** by the sync pile's task 01 (20 November): the copy of
  agent-core at `v0.7.3` brings meshmaster.dk's own file for all nine of the waiting ones, and
  this is the only one whose bytes differ - in exactly that way. Nothing fails on it: the four
  tests that read the waiting files read them by name out of `host_test/fixtures/contracts-ahead/`,
  and no test or rail counts either fixture set. Moving the eight that agree over to the copy's
  own directory is a task of its own.
- [ ] **[later, found by the sync pile's 01] The kept image may grow 88 bytes before the scratch
  must.** `agent::kKeptScratchBytes` is 19 456 and the largest image agent-core can write is now
  `mm::kMaxKeptImageLen` = **19 368** (it was 19 328; upstream's p1.13-55 appended the 40 bytes of
  a pending batch's client id). The margin was 128 bytes and is 88. The figure is read off the
  library now rather than added up here, so the next growth fails the `static_assert` in
  `walter_agent/agent_backend.h` instead of quietly dropping the tail of what a sleep keeps - but
  whoever has to raise it should know that 19 456 is the top of the allocator's bucket (anything
  from 18 433 to 19 456 takes the same block), so the next byte above it costs a bigger block in
  the heap the main task's stack comes out of (rail R22, and phase 13's item above).
- [ ] **[later] Piles 12 to 16 are not reviewed.**
