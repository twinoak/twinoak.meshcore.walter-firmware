# Flashing a unit, with the unit on a desk

From a module out of its box to a unit that checks in with meshmaster.dk. One page: the Walter,
then the RAK, then what *checking in* looks like, then the three things to look at when it does
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
   esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 twinoak-walter-v0.1.0-factory.bin
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
twinoak-walter 0.1.0+<sha7>
running from ota_0 at 0x010000 (3072 KB), ota state valid
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.1.0+<sha7>, link 1.0
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
  `adafruit-nrfutil dfu serial -pkg firmware.zip -p COMx -b 115200`. That repository has no release
  of its own yet, so the images come from a build of its `main`; when it has one, its own
  `docs/flashing.md` is the page to follow and this release's notes name the release it goes with.
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
  `0.1.0+<sha7>` for this release. It is the one place a person sees which release a unit out there
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
