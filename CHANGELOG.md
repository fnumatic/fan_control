# Changelog

All notable changes to this project are documented here. Versions follow
[Semantic Versioning](https://semver.org/): patch for fixes, minor for behaviour changes such as default curves
or limits, major for interface changes (module parameters, `pwmN`/`pwmN_enable` meaning).

## [1.2.0-f7bsc] - 2026-11-01

- Local fork: add board F7BSC (UM790 Pro, `Venus series`, EC 2.6) next to F7BSI. Board data (host tuple, EC
  version, CPU tables, system policy) moved into a `struct board`; the admission checks and the write
  preconditions use the selected board.
- F7BSC CPU tables: `Default`/`Balance` bases `0,16,18,21,28,34,36`, `Performance` bases `0,18,21,28,36,40,46`,
  standard bands. System policy `0,25,0 / 20,83,25 / 51,100,83` (standard).
- Live-verified on an F7BSC with BIOS 1.09: PNP `55 71 02`, controller profile `55 71 02 43 14 7f`, CPU table
  selector `0xb1`, tachometers and both handoffs.
- Review fixes: the empirical limits (minimum and spin-up codes, temperature limits) moved from file-level
  `#define`s into `struct board`; command-line parameters fall back to the strictest of them until a board is
  selected. `scripts/install-dkms.sh` now builds and installs before unloading, and fails instead of reporting
  success when the running module cannot be replaced. A host mismatch names the failing DMI field again.
- Install script hardening: it compiles the tree outside DKMS first, because DKMS unbuilds the previous build
  before compiling. It checks the running module (`/sys/module/.../version` and `srcversion`), not the file on
  disk.

## [1.1.0] - 2026-10-11

- Kernel log lines start with `minisforum_ec:` instead of `minisforum_ec minisforum_ec:`.
- README: install the latest release from a fixed download link (`releases/latest/download/minisforum-ec-dkms.deb`),
  download to `/tmp`, and document the branch and release workflow.
- CI: build `dev/**` branches; refuse a release tag that is not on `main`; publish the package also under the fixed
  name `minisforum-ec-dkms.deb`.

## [1.0.0] - 2026-10-11

First release.

- Admission checks before any EC access: exact DMI identity, EC firmware 0.5, Super-I/O and controller
  signatures, canonical CPU table and system fan policy. Refuses to load on anything else.
- `hwmon` device with both fans (`fan1` CPU, `fan2` system) and the two EC temperatures, visible in `sensors`.
- Fan control per fan through `pwmN_enable`: 1 fixed value in `pwmN` on hwmon's 0..255 scale (5 per EC code,
  100 RPM), with a 30 s watchdog back to the curve; 2 driver curve; 3 firmware.
- Temperature curves as module parameters (`cpu_curve`, `system_curve`), changeable at runtime. Defaults tuned for
  silence: CPU `0:0,25:1000,45:1400,54:1800,66:firmware`, system `0:0,25:1000,40:1600,50:firmware`.
- One code per step, a curve step is left 4 °C below its temperature, refused speeds that make the fans hunt
  (1–549 RPM CPU, 1–749 RPM system), hunting detection with back-off, and a fall back to the firmware after
  repeated failed handoffs.
- Fixed limits that hand a fan to the firmware: EC CPU 80 °C, EC system 55 °C. The CPU table's critical row is
  never written.
- Both fans returned to the firmware on unload, suspend, hibernate and shutdown; states left by an interrupted
  session are repaired at load.
- Automatic loading on the F7BSI board, DKMS packaging (Linux 6.17 or later), works with Secure Boot and kernel
  lockdown.
- Releases ship a Debian/Ubuntu DKMS package and a source tarball, built by CI against the Ubuntu 26.04
  kernel headers.
