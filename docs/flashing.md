# Flashing a unit, with the unit on a desk

From a module out of its box to a unit that checks in with meshmaster.dk. One page: the Walter,
then the RAK, then *[a unit at 0.1.0](#a-unit-at-010)* — the one that already exists — then what
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
   esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 twinoak-walter-v0.1.1-factory.bin
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
twinoak-walter 0.1.1+<sha7>
running from ota_0 at 0x010000 (3072 KB), ota state valid
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.1.1+<sha7>, link 1.1
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

## A unit at 0.1.0

The first unit ran `v0.1.0` on 6 November 2026 and could not finish a radio install: the flasher
never let the idle task run, so the task watchdog reset the Walter at every attempt, and the run
ended `STUCK: no image to return to`. Four more faults came out of the same evening. `v0.1.1` is
all five, and taking a unit there is three things in this order — the Walter, then the radio's
first install, then the server.

### The Walter's own update goes by cable

The same one `esptool` line as a module out of the box, with **this release's** `-factory.bin`:

```
esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 twinoak-walter-v0.1.1-factory.bin
```

**What it keeps:** `nvs` (the settings — the APN, the SIM's PIN, the server's URL and token, the
unit's name), `journal` (the update journal, which is where the *stuck* mark on a radio slot lives:
`rak update status` says *the last update ended: stuck*) and `rakfw` (the image store — the stored
candidate, any golden image, the base). All three are above the two application slots and a factory
image ends where the application ends, so a provisioned unit stays provisioned and comes back with
the same radio images and the same journal it had (*What a factory flash keeps*, above).

**Not over LTE** — but not for the reason it looks like. A unit's own image **never comes from
github.com**: the only path to it over the air is a `twinoak.fw.offer` job whose product is the
Walter's, and that fetch is `{server.url}/…` — meshmaster.dk — which this firmware holds to **ISRG
Root X1**, the root that loaded and matched on the first board on 23 October
(`core/net/trust.cpp`: `github.com` is the one host held to USERTrust ECC, the root that modem
refused, and every other host — meshmaster.dk, and the `githubusercontent` host a GitHub download
redirects to — falls to ISRG Root X1). The console's `fetch <base-url>` is what reaches github.com,
and it only ever writes a **radio** image into the store. So a 0.1.0 unit could in principle be
offered `v0.1.1` over LTE, and from `v0.1.1` onwards that is the way: *Offer a release* on the
unit's page, then an apply, whose result the **next** wake posts ([`field.md`](field.md)).

Why the cable all the same, for this one unit:

- **0.1.0 cannot be relied on to carry its own download.** An offer is many HTTPS connections in
  one wake — the config, the heartbeat, the claim, then the manifest, its signature and a range of
  the image per wake budget — and on 0.1.0 the *second* connection of a wake was the one that
  failed: `the request did not go out`, three wakes in a row on 6 November. That is the fault
  `v0.1.1` fixes, and it is the firmware that would have to do the fetching.
- **The radio needs a cable in this session anyway** (below), so the Walter's own flash costs
  nothing extra; a 752 kB application over LTE-M on a metered SIM across several wakes is not free.

### Then the radio's first install

1. **`rak hello`** — the radio's session: its firmware version, its board, its slot, and with a
   repeater at `v0.1.1` its `band 868` (a `v0.1.0` radio says `band: not said (a 1.0 radio)`, which
   is a radio this Walter still talks to, flashes and proves). No answer at all is
   [`bench.md`](bench.md) §3, not an update.
2. **The candidate the unit holds may not be the one to use.** `image list` says what is in the
   store and `image bad` lists the hashes of candidates that failed. **A first install that failed
   put its candidate's hash on the bad list before it entered *stuck*** (`RadioUpdate::fail`: the
   hash first, then *no image to return to*), and such an image is refused before a sector is
   erased — `error: rak update: refused at check: this image failed a health check once and is
   never tried again` — and cannot be pushed back in either. So on the unit of 6 November, push the
   repeater's **`v0.1.1`**, which is a different file and a different hash:

   ```
   python tools/walterctl.py push --port COM7 --as candidate <release-dir>
   ```

   Nothing has to be typed about the conversion: `walterctl` reads the protocol off the console's
   ready line and takes a 0.1.0 Walter's CR LF back out of its answers by itself (*Pushing a
   repeater image in over the cable*, above). Against a Walter already at `v0.1.1` it says
   `protocol 2` and sends the frames as they are.
3. **`rak update --first-install`** — the run with no golden image to return to, a person's command
   only. Expect the flasher's page lines through the scan, the pages and the read-back **with no
   reboot in the middle** (that reboot was the fault), then the reset, the health check and
   `rak update: done: <version> (sha256 …) is the golden image`. Seven minutes or so for a whole
   image at 125 kHz is the order to expect.

A committed first install is what makes that image **golden**: it ends the slot's *awaiting its
first install* and it is the last word on a slot the journal had left *stuck*. From then on the
radio has a proved application to go back to, and every later image is an ordinary `rak update`.

### Then meshmaster.dk

*Read the unit's state* on the unit's page. With a golden image the slot's role, board and band
come from the image's own descriptor; with a `v0.1.1` radio and no golden image they come from the
session's `HELLO`, which now ends with the band — the one word that was missing on 6 November and
the reason the server could pick no image. Either way *Mark canary* can offer this release, and the
unit's repeater can be offered a release from then on with no cable at all.

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
  `0.1.1+<sha7>` for this release. It is the one place a person sees which release a unit out there
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
