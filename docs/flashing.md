# Flashing a unit, with the unit on a desk

From a module out of its box to a unit that checks in with meshmaster.dk. One page: the Walter,
then the RAK, then the two ways a unit that already exists takes a newer release —
*[from 0.1.2, over LTE](#from-012-to-014-over-lte)* with no cable at all, and
*[a unit at 0.1.0](#a-unit-at-010)* by cable — then what
*checking in* looks like, and the three things to look at when it does
not. [`bench.md`](bench.md) is the long walk that proves each of these one at a time;
[`provisioning.md`](provisioning.md) is the session that gives a unit its SIM, its server and its
name; [`console.md`](console.md) is every command.

**Take the files from a release**, not from a build directory:
[*Releases*](https://github.com/twinoak/twinoak.meshcore.walter-firmware/releases) → *Latest*. A
release is five files, and its body says which commit it was built from and how to check the
signature ([`release.md`](release.md)).

## The Walter

The module's USB-C **is** the ESP32-S3's own USB, so there is no adapter and no driver to find.

1. `pip install esptool` if you have none, and plug the module in. **If no serial port appears,
   hold the module's BOOT button while plugging it in** and let go after a second.
2. Flash the release's `-factory.bin` at `0x0` — one file, which carries the bootloader, the
   partition table and the OTA data with it:

   ```
   esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 twinoak-walter-v0.1.4-factory.bin
   ```

   `--flash-mode dio` is esptool's way in; the image's own header says QIO and the bootloader
   switches to it. The other `.bin` in the release, without `-factory`, is **not for esptool**: it
   is the image a Walter in service downloads into its other application slot by itself.
3. Open the port at any baud — USB Serial/JTAG ignores it
   (`python -m serial.tools.miniterm COM5 115200`, PuTTY, `screen`) — and press reset.

**What a factory flash keeps.** The settings (`nvs`), the update journal (`journal`), the radio
images the unit has stored (`rakfw`) and a core dump (`coredump`) are all **above** the two
application slots, and a factory image ends where the application ends — so flashing one cannot
reach them. A unit that was provisioned stays provisioned; a radio's golden image is still there
afterwards. That is a property of the file and not a promise: `tools/check_partitions.py` holds
the rule on every build and the release refuses to publish a factory image that breaks it
([`release.md`](release.md), *What the tooling refuses*).

### The banner, and the three lines to look at

After a reset the port prints a dozen lines and a `>` prompt
([`console.md`](console.md), *What the port prints at boot*, has them all):

```
twinoak-walter 0.1.4+<sha7>
running from ota_0 at 0x010000 (3072 KB), ota state valid
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.1.4+<sha7>, link 1.2
...
type 'help' for the commands
>
```

Three lines decide whether this board is what you think it is. Type the command and read the
answer:

| | Command | What it must say |
|---|---|---|
| **the slot** | `self status` | the version above is **the release's**, `ota_0` is what runs, and the state is **`valid`** — not `pending-verify`, which is an image on trial. A factory flash is `valid` from its first boot |
| **the trust** | `trust` | `ed25519 self-test (…): passed, <n> ms`, then `trust: 1 key this build accepts a release signed with: 73efdd383a957596`. **`none`** means this build trusts nothing and will accept no update at all; a `WARNING` and the key `c17969deb0026955` mean somebody flashed a test-key build, which must never be left on a unit that goes out |
| **the modem** | `lte`, then `lte up` | `lte` only reads, and says the modem is not started. `lte up` is **minutes** on a cold attach and must end `up`, with a `signal:` line naming the operator and a band, and a `time:` line *from the network*. No SIM says `no-sim` in seconds; no coverage says `not-registered` after the deadline |

Then [`provisioning.md`](provisioning.md): the APN, the SIM's PIN if the card has one, the server's
URL, the token from meshmaster.dk (*Exit nodes → Register an exit node*) and a name for the unit.
Nothing of that is readable back afterwards — a secret answers `set` or `unset` and never its
value.

## The RAK

The other box is a RAK4631 running
[`twinoak.meshcore.repeater-firmware`](https://github.com/twinoak/twinoak.meshcore.repeater-firmware).
Two ways in, and the first time it is always the cable:

- **By cable, at a desk.** Its own repository is the guide (its `README.md`, *Building it*, and its
  `docs/bench.md` §1): double-tap the reset button on the adapter's USB-C, the bootloader's drive
  appears, and `firmware.uf2` is copied onto it — or
  `adafruit-nrfutil dfu serial -pkg firmware.zip -p COMx -b 115200`. Its own
  [`docs/flashing.md`](https://github.com/twinoak/twinoak.meshcore.repeater-firmware/blob/main/docs/flashing.md)
  is the page to follow, and its *Releases → Latest* is where the files come from; this release's
  notes name the release of it this one goes with.
- **Later, through this Walter**, which is why the LTE box exists: `rak update` on this console
  flashes the stored candidate image over the debug port, proves it, and puts the golden image back
  if it is not healthy ([`update.md`](update.md)). A `fetch` on the console or a
  `twinoak.fw.offer` job from meshmaster.dk is what puts a release into that slot in the first
  place — on the repeater's page, *Offer a release* — and no cable is needed on the mast.
- **A factory radio** (RUI3 out of the box, no MeshCore and no identity) needs its base first:
  `rak provision check` says what it would do and `rak provision <DEVICEID> confirm` does it. **It
  erases that radio whole**, so it is a typed command and nothing else in the firmware can reach it
  ([`provisioning.md`](provisioning.md), *A factory radio*; `spec/SYSTEM.md` S-5a).

The repeater needs none of this to repeat: it boots, repeats and answers mesh administration with
the Walter asleep, dead or absent ([`field.md`](field.md)).

### Pushing a repeater image in over the cable

With a release of the repeater firmware on the laptop, its image goes into this Walter's store over
the same USB cable, and `rak update` takes it from there
([`console.md`](console.md), *Loading a radio image over USB*):

```
python tools/walterctl.py push --port COM7 --as candidate <release-dir>
```

The release's `swd` file is 400 kB and more — about 1,740 chunks, a progress line every tenth of
the file, then `stored as the candidate image`. Expect **seconds, not hours**: until 6 November the
laptop's end waited out its whole 5 s reply timeout for every acknowledgement, which was two and a
half hours for one radio image ([`bench.md`](bench.md) §8 has the figure to write down). Then
`rak update`, or `rak update --first-install` on a radio that has no golden image yet.

**A Walter at 0.1.0 needs nothing typed.** Its console turned the transfer's answers into CR LF and
no push of more than nine chunks could finish (task p8-02; the ready line of such a Walter says no
protocol). `walterctl` reads the version off that ready line and puts the conversion back out of
the answers by itself, saying so in one line — *a Walter at 0.1.0 or older: taking its console's
CR LF back out of its answers*. It is slower than a 0.1.1 push and it finishes. `--undo-crlf` is
the same thing forced, for a Walter whose ready line cannot be read at all; it is never needed
against 0.1.1 or newer, and the hello's version word refuses it there.

## From 0.1.2 to 0.1.4, over LTE

**This is the walk a unit already in the field takes with no cable at all.** Two presses on
meshmaster.dk, on the **unit's own exit-node page**, and the unit does the rest across one or two of
its own wakes. Nothing here is typed at a console; the console is only where it can be watched.

> **It has never once finished, before v0.1.3, and here is what had to change.** The first update of
> a unit over LTE — job 22841, 8 November 2026 — failed in two seconds with *a status this firmware
> does not act on*. meshmaster.dk refuses every `/agent/v1` request that does not carry
> `X-Agent-Version` with a 400, and **the download never sent it**, in any release up to and
> including v0.1.2 (`core/update/fetch.cpp` sent the token and a `Range` and nothing else; the
> agent's own calls always sent the header). So no unit in the field could fetch an image over LTE,
> neither its own nor its radio's. Two halves fix it, and **both are in place now**:
> - **meshmaster.dk's pile 4.28, task 177** — merged 8 November 2026 15:36 UTC and **deployed the
>   same day as its release 61, 16:17 UTC** — which serves the three download routes (the manifest,
>   its signature and a file) to a request that carries no header, for the units already out there;
> - **this firmware's v0.1.3** (task p10-05), in which *every* request carries it, and a refusal
>   names its status and its file: *http 400 for manifest.json*, with the server's own sentence
>   after it and no token in it.
>
> So: a unit at **0.1.2 or 0.1.1** takes this release over LTE, because the server's exemption
> covers its headerless download; and from v0.1.3 onwards neither half is needed, because the unit
> says who it is. The exemption is temporary — meshmaster.dk deletes it the day no unit reports a version older
> than 0.1.3 — so **a unit left behind on 0.1.2 is a unit that will one day need the cable**: *If it
> is wanted by cable instead*, at the end of this section. A unit still at 0.1.0 goes by cable for a
> different reason (*A unit at 0.1.0*, below).

Before pressing anything, three facts about the file:

- **The image comes from meshmaster.dk and never from github.com.** The only path to the Walter's
  own image over the air is a `twinoak.fw.offer` whose product is `twinoak-walter-firmware`, and the
  fetch is `{server.url}{base}` — the manifest at `{base}/manifest.json`, its signature beside it
  and the file at `{base}/files/{name}` (`components/agent/firmware_jobs.cpp`). Every host but
  `github.com` is held to **ISRG Root X1** (`components/core/net/trust.cpp`), which is the root that
  loaded into the modem's slot and matched at the first request on the first board on 23 October.
  The console's `fetch <base-url>` is the one thing that reaches github.com, and it only ever writes
  a **radio** image into the store.
- **The Walter's own download does not resume across wakes.** A radio image does: the store keeps a
  progress record and the next wake continues the file. The Walter's own slot is erased when a write
  opens on it (`SelfUpdate::beginWrite` → `written_ = 0`), so a download the wake's budget cuts
  short is started again from zero at the next wake and the plan pays for those bytes twice. From
  v0.1.3 the messages say so: the job's result carries *this slot keeps no progress, so the next
  wake starts the file again from zero*, and the line the cable shows ends *the next wake starts the
  file again from zero* where a radio image's ends *the next wake continues it* (task p10-04; up to
  v0.1.2 both promised a resume, which was true of a radio image and false of this one). In
  practice it fits: the exchange is given **twenty minutes** (`kBackendBoundMs`) and a megabyte a
  wake (`kWakeDownloadBytes`), and the application is **780 528 bytes** in **twelve 64 kB ranges**
  (the last one 58 kB), each its own request with its own deadline and three attempts. A range that
  stalls is retried; three failures in a row end the download for that wake.
- **Every request says which build made it, and a refusal says what it was.** From v0.1.3 the
  HTTP client itself puts `X-Agent-Version` on every request this firmware makes — the manifest, its
  signature and each range included — so no path above it can leave it out (`core/net/client.cpp`,
  task p10-05). When a request *is* refused, the line under it names the status and the file and
  then the server's own sentence: `http 400 for manifest.json: …`, `http 404 for manifest.json`,
  `http 403 for f.bin: the server refused this unit`. *a status this firmware does not act on* was
  v0.1.2's wording for all of them and is gone. No token appears in such a line, whatever the server
  wrote in its body.

### 1. *Offer a release*, with the Walter's product

On the unit's page, *Offer a release* — a page of its own behind the re-authentication gate,
because the unit spends its data plan on the download. Pick the release whose product is the
**Walter's**, type the reason, press it. That makes one `twinoak.fw.offer` job, and the unit claims
it at its next wake. (Nothing is flashed by an offer, and **one wake updates one box, never both**:
S-7 is why the server refuses to offer the radio an image while an apply of the Walter is still out
there, and the other way round.)

What the unit does with it, and what a cable would show while it does:

```
fetch: manifest.json, 1024 bytes
fetch: manifest.json.sig, 108 bytes
fetch: check 1, the signature: passed (key this build trusts)
fetch: check 2, the manifest: passed
fetch: check 3, the image for this slot: passed - twinoak-walter-v0.1.4.bin, 780528 bytes, version 0.1.4+<sha7>
fetch: check 6, the manifest's region against this build's: passed
  range 0-65535: ok,  (attempt 1)
  ...
fetch: 780528 bytes in 12 range(s), 0 retries, 0 restart(s), 219000 ms
fetch: check 4, the file's size and hash: passed
fetch: check 5, the descriptor inside the file: passed
fetch: check 6, the region: passed
```

Checks 1, 2, 3 and the manifest's half of 6 run **before a byte of the image is asked for**, and 4,
5 and the rest of 6 over the bytes as they lie in the other slot — the laptop, the server and the
manifest are all untrusted at every one of them (`spec/FIRMWARE-MANIFEST.md` §5).

On the unit's page while it runs: the job's **Progress** line, *512 kB of 766 kB*, posted every
128 kB between two ranges (the one moment the socket is free), and the lease extended as it goes.
When it is done the job says *stored*, with the image id, the hash and the bytes, and the page's
**Firmware downloads** box gains a line for what the plan was billed. At the cable, `self status`
now says `received: 0.1.4 in slot 1 (780528 bytes), waiting for 'self switch'` — the same state a
`self receive` over USB leaves, and the same one the apply below consumes.

### 2. *Apply the candidate*

The second press, also gated and also with a reason. The unit claims the `twinoak.fw.apply` at a
wake, checks that the image in its other slot is the one the server named — the hash, not the
server's word — journals the result it will **owe**, tells the radio how long it will be gone, and
restarts. The boot after that is the trial:

```
boot 7: software reset, the Walter's own update, switching to the other slot
self-update: this image (0.1.4, slot 1) is on trial: 300 s to reach the main loop, HELLO the radio
  and reach the backend, or it is rolled back ('self status' watches, 'self confirm' / 'self reject' end it)
...
self-update: on trial -> confirmed: all three conditions hold: confirmed
```

The three conditions are the main loop with the pins in the safe state, a `HELLO` the radio answers,
and one exchange with the backend ([`update.md`](update.md), *The trial*). The heartbeat of that
boot's own wake is the third one, so the trial ends in the same wake that began it — and **the
result the apply owes is posted from the journal in that same exchange**, not an interval later. Any
reset inside the trial window is a rollback, by the bootloader and not by a decision: the previous
image comes back, the new one's hash goes on the bad list, and the journal says why.

On the unit's page: the job reads *committed* with the version it moved to, or *rolledBack* with the
journal line by line — which is the unit disagreeing with the server, and allowed to. Afterwards the
heartbeat's `agentVersion` is `0.1.4+<sha7>`, and that is the one place a person sees which release
a unit out there is really running.

### If it is wanted by cable instead

The `esptool` line of *The Walter*, above, with this release's `-factory.bin`. It keeps `nvs`, the
`journal` and the `rakfw` store, so a provisioned unit stays provisioned and its radio images
survive — the same promise, from the same property of the file.

### A Walter stuck on 0.1.3

**`v0.1.3` cannot start.** ESP-IDF could not create its main task — no heap region had a block
left for the main task's 16 kB stack once the static initialisers had taken theirs — so every boot
of it ends in `assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)` and the
board restarts, printing the ROM's `rst:0xc (RTC_SW_CPU_RST)` and nothing else. No banner, no
prompt, nothing to type at ([`release.md`](release.md), *The boot gate*, and
[`ARCHITECTURE.md`](ARCHITECTURE.md) §14).

A unit that took it **over LTE** has already dealt with it by itself: the image never reached its
main loop, so the bootloader rolled back to the slot it came from and the unit is on its old
release, which is the whole point of the trial. Nothing to do but offer it `v0.1.4`.

A Walter **flashed with `v0.1.3` by cable** has no rollback — an image flashed at `0x0` is in no
OTA state and runs no trial — so it restarts until another image is flashed over it. Flash
`v0.1.4`'s factory image with the `esptool` line of *The Walter* above; hold the module's BOOT
button while plugging it in if no port appears, because a board in a reset loop may not hold one
long enough. It keeps the unit's settings, its journal and its stored radio images, as every
factory flash does, and the banner of the first boot after it is `twinoak-walter 0.1.4+<sha7>`
with a `heap:` line under it.

## A unit at 0.1.0

A unit still at `v0.1.0` goes by cable, and the first unit is past this: it was flashed to `v0.1.1`
on 7 November 2026, took `v0.1.2` by cable on 8 November, and takes this release over LTE (above). The section stays for any other board
left at `0.1.0`. On 6 November that release could not finish a radio install — the flasher never let
the idle task run, so the task watchdog reset the Walter at every attempt and the run ended
`STUCK: no image to return to` — and four more faults came out of the same evening. Taking such a
unit to this release is three things in this order: the Walter, then the radio's first install, then
the server.

### The Walter's own update goes by cable

The same one `esptool` line as a module out of the box, with **this release's** `-factory.bin`:

```
esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 twinoak-walter-v0.1.4-factory.bin
```

**What it keeps:** `nvs` (the settings — the APN, the SIM's PIN, the server's URL and token, the
unit's name), `journal` (the update journal, which is where the *stuck* mark on a radio slot lives:
`rak update status` says *the last update ended: stuck*) and `rakfw` (the image store — the stored
candidate, any golden image, the base). All three are above the two application slots and a factory
image ends where the application ends, so a provisioned unit stays provisioned and comes back with
the same radio images and the same journal it had (*What a factory flash keeps*, above).

**Not over LTE, from 0.1.0** — and not for the reason it looks like. The path exists at `0.1.0`
already and goes nowhere near github.com: it is the `twinoak.fw.offer` of *From 0.1.2 to 0.1.4, over
LTE* above, from meshmaster.dk and held to ISRG Root X1. What `0.1.0` cannot be relied on is
**carrying its own download**:

- **its second HTTPS connection of a wake was the one that failed** — `the request did not go out`,
  three wakes in a row on 6 November, because every connection was socket 1 and the previous
  connection's close landed on the next one. An offer is the config, the heartbeat, the claim, the
  manifest, its signature and a dozen ranges, so a unit that cannot make two connections cannot
  make fifteen. The fix is `v0.1.1`'s, and the firmware that would have to do the fetching is the
  firmware with the fault.
- **the radio needs a cable in this session anyway** (below), so the Walter's own flash costs
  nothing extra; 785 kB over LTE-M on a metered SIM is not free.

### Then the radio's first install

1. **`rak hello`** — the radio's session: its firmware version, its board, its slot, and with a
   repeater at `v0.1.1` or newer its `band 868`, and at `v0.1.2` the `local-request` capability
   (a `v0.1.0` radio says `band: not said (a 1.0 radio)`, which is a radio this Walter still talks
   to, flashes and proves). No answer at all is
   [`bench.md`](bench.md) §3, not an update.
2. **The candidate the unit holds may not be the one to use.** `image list` says what is in the
   store and `image bad` lists the hashes of candidates that failed. **A first install that failed
   put its candidate's hash on the bad list before it entered *stuck*** (`RadioUpdate::fail`: the
   hash first, then *no image to return to*), and such an image is refused before a sector is
   erased — `error: rak update: refused at check: this image failed a health check once and is
   never tried again` — and cannot be pushed back in either. So push the repeater's current
   release, **`v0.1.2`**, which is a different file and a different hash:

   ```
   python tools/walterctl.py push --port COM7 --as candidate <release-dir>
   ```

   Nothing has to be typed about the conversion: `walterctl` reads the protocol off the console's
   ready line and takes a 0.1.0 Walter's CR LF back out of its answers by itself (*Pushing a
   repeater image in over the cable*, above). Against a Walter already at `v0.1.1` or newer it
   says `protocol 2` and sends the frames as they are.
3. **`rak update --first-install`** — the run with no golden image to return to, a person's command
   only. Expect the flasher's page lines through the scan, the pages and the read-back **with no
   reboot in the middle** (that reboot was the fault), then the reset, the health check and
   `rak update: done: <version> (sha256 …) is the golden image`.

   **How long it takes.** From the code's own page and poll counts, over the simulated debug port
   at the default 125 kHz (`core/swd/wire.h`; the figures are the ones
   `host_test/core/flasher/test_flasher_watchdog.cpp` prints):

   | | |
   |---|---|
   | the flash: 102 pages scanned, written and read back whole | **413 s** for 417,284 bytes, 548 of task p9-01's pauses in it (5.5 s, about 1 %) |
   | the radio's reset and its boot window | up to `kBootWindowMs`, 20 s |
   | the proving period, a `PING` every 3 s | `kProvingPeriodMs`, 180 s |
   | the check and the commit (a hash over the stored image, a pointer moved) | seconds |

   So **about eleven minutes** is what the code says, of which seven are SWD traffic. On the first
   unit on 7 November it was **about a quarter of an hour** — 19:31 to 19:49 local, between
   Thomas's two console pastes, so with a person's time in it — which says the real port is
   slower than the simulator's clean 125 kHz. That is the line to fill properly:
   [`bench.md`](bench.md) §9, *A whole image flashed with no watchdog reset, timed*, asks for the
   wall-clock time of the flash alone and the image's size.

A committed first install is what makes that image **golden**: it ends the slot's *awaiting its
first install* and it is the last word on a slot the journal had left *stuck*. From then on the
radio has a proved application to go back to, and every later image is an ordinary `rak update`.

### Then meshmaster.dk

*Read the unit's state* on the unit's page. With a golden image the slot's role, board and band
come from the image's own descriptor; with a `v0.1.1` radio or newer and no golden image they come from the
session's `HELLO`, which now ends with the band — the one word that was missing on 6 November and
the reason the server could pick no image. Either way *Mark canary* can offer this release, and the
unit's repeater can be offered a release from then on with no cable at all.

## A radio recovered by hand may come back with a new key

A radio left half-written — an install that stopped part-way — is brought back over its **own**
USB-C with the release's `.uf2`, and that is the repeater repository's procedure, not this one's:
its [`docs/flashing.md`](https://github.com/twinoak/twinoak.meshcore.repeater-firmware/blob/main/docs/flashing.md)
§3a, *A radio left half-written*, with a step 0 that is this Walter's (`rak update status` first:
a run the journal left in *flashing-candidate* is resumed at the Walter's next boot and would
flash the journalled image over whatever has just been put on the radio by hand).

**What the first unit showed on 6 November is that the radio can come back as a different node.**
Neither the Walter's flasher nor the `.uf2` writes the file system at `0xED000`–`0xF4000` where
the node's key lives — but up to repeater `v0.1.1` the first boot after the `.uf2` called the
Adafruit core's `InternalFS.begin()`, which **formats the region when the volume will not mount**,
and a torn page is what an interrupted flash leaves. It then found no key file and drew a new key,
with no word said. The reading is the repeater repository's `tasks/phase-3.9/reports/01.md`; from
its `v0.1.2` the radio **refuses** instead, says `IDENTITY LOST` on its console and on the link,
and waits for a person's `erase` and `reboot`.

**How to see it**, from this Walter and from the server:

```
> rak hello                 # which firmware answered, and its board and slot
> agent id                  # `repeater: <64 hex>` - the key IDENTITY returned
```

and on meshmaster.dk, the unit's exit-node page: the **companion key changed**, and the audit log
has `agent.companion_changed` with the old key and the new one.

**A radio in the lost mode has no key to print at all, and since 8 November this firmware says
which of the two that is** (task p10-03). Up to `v0.1.2` of this firmware `agent id` printed *the
radio has no identity yet* at it - the sentence written for a unit registered before its repeater
- because every `IDENTITY` answer that was not a key was read alike. It now says:

```
agent id: the radio refuses: its identity is lost. No key was made, so the heartbeat
  carries no key, no radio block and no job types - and a counter that says which of
  the two this is (radioIdentityLost).
  the evidence: IDENTITY answered ERR_STATE, and HELLO advertises caps 0x21 (CLI, ATTENTION) and nothing else.
  what a person does: read docs/flashing.md 3a first - the old key cannot be
  recovered - then 'rak cli erase' and 'rak cli reboot' make this radio a new node,
  whose key MeshMaster has to be told and whose settings have to be set again.
```

and the heartbeat carries `radioIdentityLost: 1` among its `errors`, which the exit node's page
shows in its *Counters* row. A radio that is **awaiting its first install** - an all-zero key, or
no answer - still says *the radio has no identity yet*, and that one needs its install and not a
person. An update applied to a radio in the lost mode ends *node-fault* and spends no rollback
(`docs/update.md`, check 1a).

**What then**, when the key is new — in this order, because each step needs the one before it:

1. **Confirm the key on meshmaster.dk** (*This is the right companion* on the exit node's page).
   Until that is done the server suspends admin routing through the unit.
2. **Register the unit's repeater again** from the new key: the old repeater record belongs to a
   node that no longer exists. On the first unit that made repeater 41 where it had been 12.
3. **Re-apply the settings**, through this console's `rak cli <line>` or at the radio's own USB
   port, line by line as the repeater's `docs/provisioning.md` §2 gives them — the `name`, the
   position, `owner.info`, the **admin password** (a fresh node draws a random one nobody knows,
   `spec/SYSTEM.md` S-13), the guest password, the regions and the ACL. Then tell MeshMaster the
   admin password; until it has one, every `admin`-class job of that repeater fails with *no admin
   password stored*, which is what job 22016 did on 7 November.

The profile restores the rest by itself on the first boot; the repeater's own guide lists which.

## Checking in

A provisioned unit is an **exit node** on meshmaster.dk — the uplink of its repeater. Register it
once (*Exit nodes → Register an exit node*), put the token into `set server.token`, and then, with
the cable still in:

```
> lte up
> agent probe        # one GET /agent/v1/config: the server answers with this agent's id and label
> agent wake         # one whole check-in: the heartbeat, the claim, and any jobs it was given
```

On the exit node's page, after that wake:

- **the heartbeat** — *last seen* moments ago, and the page reads **asleep until about HH:MM UTC**
  once a minute has passed: a repeater-v2 unit tells the server when it will be back, and the
  server judges it against that promise rather than calling it quiet between wakes;
- **the firmware version** — `agentVersion`, which is this firmware's own version string,
  `0.1.4+<sha7>` for this release. It is the one place a person sees which release a unit out there
  is actually running;
- **the repeater** — the unit's heartbeat reports its radio's public key, and the first heartbeat
  carrying a key the server knows links the exit node to that repeater by itself.

After the cable comes out the unit runs one wake and sleeps; from then on it checks in on its own
interval — a quarter of an hour on a full battery, once a day at the floor
([`power.md`](power.md)) — and **cannot be reached between wakes at all**. That is normal, and
[`field.md`](field.md) is what to expect of a unit on a mast.

## When it does not

In this order, at the console:

1. **`status`** — one screen: the version and what the descriptor says, why this boot happened, the
   radio's session and health, the link's counters, the pins, the images in the store, this
   firmware's slot and trial, the uplink's last attempt and when the next one is allowed, the
   backend and the next wake. Most answers are already here.
2. **`lte`** — what the last attach did and how far it got (`no-sim`, `sim-locked`,
   `not-registered`, `modem-silent`), whether a PIN is set and whether the card refused it, and
   when the backoff allows the next try. A unit with no coverage is a unit working correctly and
   saying so.
3. **`agent url`**, then **`agent probe`** — whether `server.url` and `server.token` are set and
   what the server answers to this unit's token. A `401` is the token, a `404` the URL, and nothing
   at all is the uplink (go back to 2). A unit needs **meshmaster.dk 4.14 or newer** to be told
   anything to do.

And when none of those explains it: **`dump`** is the last 4 kB this port printed, `journal` the
last twelve entries that survived whatever happened, `reboots` why the board last restarted, and
`wake` why it is awake. [`bench.md`](bench.md) is the rest — every step of the rig, what should be
seen and what it means when it is not — and [`console.md`](console.md) is every command there is.
