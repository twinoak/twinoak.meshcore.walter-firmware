**Which repeater firmware this goes with.** A unit is two boxes and one key signs both: this
release runs beside
[`twinoak.meshcore.repeater-firmware` v0.1.2](https://github.com/twinoak/twinoak.meshcore.repeater-firmware/releases/tag/v0.1.2),
key `73efdd383a957596`. Nothing in the box link changed with this release, so a radio at that
release needs nothing done to it.

**Taking it from the release before, over LTE.** *Offer a release* on the unit's page on
meshmaster.dk with the Walter's product, then *Apply the candidate*; the unit fetches it on its
next check-in, writes it into its other slot, switches, and is brought back by the bootloader if
the trial fails. A unit that will not come up at all goes by cable, with the factory image and the
`esptool` line below.
