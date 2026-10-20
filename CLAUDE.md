# Working in this repository

Read [`spec/SYSTEM.md`](spec/SYSTEM.md) first — what a unit is and the decisions
(`S-1`…) everything here rests on — then [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
For a task run by the driver, [`tasks/RUNBOOK.md`](tasks/RUNBOOK.md) is the
procedure and it wins over this file.

- **`spec/` is a copy.** Never edit a file under it. It is changed in
  `twinoak.meshcore.standard-repeater-design-v2/software/spec` and copied here
  with `specsync.py`; `python spec/tools/specsync.py verify spec` must pass.
- **The trunk is `main`.** One task, one branch `task/pP-NN-<name>`, one PR.
- **Verify locally with `tools/verify.ps1`** before pushing anything. It runs
  the host tests and the firmware build inside the pinned ESP-IDF image; CI
  runs the same script and is not where debugging happens.
- **Logic goes in components with no ESP-IDF include**, tested on the host
  with fakes and simulators. The code that touches a pin, the flash or the
  modem is thin and sits behind an interface.
- **Sibling repositories are read-only**, and `meshmaster.dk` and
  `meshmaster-agent-firmware` may have other sessions working in them: read
  with `git -C ../<folder> show <commit>:<path>` or `gh api`, never check out
  or write there.
- **The hardware's interfaces are frozen** (`INTERCONNECT.md` in the hardware
  repository). The pin rules are in `docs/ARCHITECTURE.md` §2 and are not
  optional: reset open-drain only, tristate before kill, resets a second
  apart.
- Never add an automatic path to a chip erase or a UICR write on the nRF52:
  S-5a's typed `rak provision` is the one path, through `core/provision/` and
  the door in `core/nrf52/provision_door`, and rails R3 and R3b hold it there.
  Never skip TLS verification, never print a token or a password, never flash
  an image that has not passed all six checks of `spec/FIRMWARE-MANIFEST.md`
  §5.
