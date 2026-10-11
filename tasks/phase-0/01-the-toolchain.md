# 01 — The toolchain

**Depends on:** nothing.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-6, S-9, S-10; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §1, §11; [`tasks/README.md`](../README.md).
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

An ESP-IDF project for the Walter that builds **inside a pinned Docker image**
— the same image on this machine and in CI — with the modem library resolved
and compiled, a host test suite beside it, and one script, `tools/verify.ps1`,
that is the definition of *verified* from here on. The firmware it builds
boots, prints what it is, and does nothing else.

It is Fable's because every later task inherits its decisions: the ESP-IDF
version, how the tree separates logic from platform, the flash layout, and what
"green" means.

## Read first

- `QuickSpot/walter-esp-idf` (the component `dptechnics/walter-modem`): its
  `idf_component.yml`, `Kconfig`, `CMakeLists.txt`, and the `https` and
  `modem_firmware_flash` examples with their `sdkconfig.defaults` and
  partition tables — for the ESP-IDF versions it supports and what the modem
  needs of the flash.
- The Walter's datasheet (`QuickSpot/walter-hardware`, `walter/datasheet/`):
  the flash size and mode, which pins the modem's UART and reset occupy
  inside the module, the USB port.
- The agent firmware's `boards/README.md` and
  `tasks/phase-1.2/11-the-partition-table.md` with its report, at
  `51faafb427cba41b0de9983736b6621bc155f829`: why settings must sit above the
  application, and what a merged image fills.

## Deliverables

- **The build image, pinned.** Espressif's `espressif/idf` at one exact patch
  release of a stable line that `walter-modem` 1.5.0 builds on, referenced
  **by tag and by digest** in one place (`tools/idf-image.txt`) that both
  `tools/verify.ps1` and CI read. The report says which and why.
- **The project** — ARCHITECTURE §1's layout, with `components/core/`
  (no ESP-IDF include; a CMake check or a grep in the build fails if one
  appears), `components/hal/` (interfaces only), `components/platform/`,
  `components/modem/`, and `main/`. For now `core` holds the version string
  (`twinoak-walter <version>`, from the build's project version, default
  `0.0.0-dev`) and `main` prints it on the console and idles. C++17, no
  exceptions, no RTTI; `-Wall -Wextra -Werror` for our components, not for
  ESP-IDF's or the managed ones.
- **The modem library** as a managed component at **exactly** 1.5.0, with
  `dependencies.lock` committed, compiled into the image and called by
  nothing — proving the component manager works in the container and the
  library builds at this ESP-IDF. Its unused protocols are switched off in
  `sdkconfig.defaults` where its Kconfig allows (keep HTTP and sockets).
- **`sdkconfig.defaults`**, each line with its reason: the target and flash
  size and mode from the datasheet; the console on the module's USB; **app
  rollback enabled** in the bootloader (S-6); the task watchdog and the
  brownout detector on; a core dump to flash; log level; nothing that enables
  WiFi or Bluetooth at boot.
- **The partition table** — ARCHITECTURE §11's constraints made concrete:
  two application slots, OTA data, NVS **above** the application slots, the
  image store (`rakfw`) sized for two radios, a journal partition, what the
  modem library needs, a core dump. And **`tools/check_partitions.py`**, run
  by `verify`: every constraint as an assertion — nothing overlaps, the
  factory image built by this task ends below NVS and the image store, the
  slots are equal, the table fits the flash — with a self-test of two tables
  it must refuse.
- **`host_test/`** — a CMake project of its own that compiles
  `components/core/` and the tests with the container's host compiler, under
  AddressSanitizer and UndefinedBehaviorSanitizer, and runs them with `ctest`.
  One test framework, pinned, available without the network at test time; say
  which and why. One test: the version string.
- **`tools/verify.sh`** (runs inside the image) and **`tools/verify.ps1`**
  (runs it there, from Windows PowerShell or `pwsh` on Linux):
  1. `python spec/tools/specsync.py verify spec`
  2. both spec self-checks (`linkref.py check`, `manifestref.py check` — the
     second needs `cryptography`; install it pinned, inside the container)
  3. the host tests
  4. `idf.py build`
  5. `tools/check_partitions.py` against what was built
  6. each step's output to `out/verify/<step>.log`, a one-line summary per
     step, a non-zero exit when anything failed.
  The build directory lives **in the container's own file system or a named
  volume, not on the bind-mounted source tree** — a build over a Windows bind
  mount is several times slower — and the images are copied to `out/` at the
  end. A compiler cache volume makes the second run quick; report both
  timings. `-Only <step>` re-runs one step.
- **`.github/workflows/ci.yml`** — on `pull_request` and on `push` to `main`,
  `concurrency` per ref, and one step that matters: `pwsh tools/verify.ps1`,
  which pulls the same image by digest. The images kept as an artifact for 14
  days.
- **Branch protection on `main`**, as the agent firmware has it (a pull
  request required with 0 approvals, admins not enforced, no force pushes, no
  deletions, the CI job required). If `gh` has no right to set it, say so.
- **`README.md`**: "State" replaced by "Building it".

## Acceptance criteria

- [ ] `tools/verify.ps1` is green on this machine from a clean clone; the report gives the image tag and digest, the firmware's size against its slot, and the cold and warm timings.
- [ ] `dependencies.lock` pins `walter-modem` 1.5.0 and the build log shows it compiled.
- [ ] An ESP-IDF include added to a file under `components/core/` fails the build with a sentence, and is removed again.
- [ ] A deliberate warning in a component of ours fails the build; one in a managed component does not.
- [ ] `check_partitions.py` passes on the real table and refuses its two self-test tables; the report prints the table with offsets.
- [ ] Changing one byte under `spec/` fails step 1 with the file's name, and is reverted.
- [ ] CI is green on the pull request, ran once for the commit, and ran `tools/verify.ps1` and nothing parallel to it.

## Not verified here (Thomas, with a Walter)

Start `docs/bench.md` with "1. The Walter alone on USB":

- Flash the factory image over the module's USB-C (`esptool` from the host,
  the command in the README); the console prints the version string.
- After a reset the same image runs (rollback is enabled: an image that is
  never marked valid must not be abandoned at the second boot — say in the
  report what this task's image does about that, and check it here).

## Out of scope

The release (02). Any pin (03). Any use of the modem.
