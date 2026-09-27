# Preparing, installing and recovering a source package

The shared builder lives in `packaging/`. It builds Java, the native hook,
renderer, supervisor scripts and M.I.B. tooling from one committed Git revision.
The input profile contains stock-file identities only. It never selects patches
or embeds local test sources into the fork.

## Inputs and build

Use PowerShell 7, Git, Docker and the QNX ARMv7 toolchain image described in the
root README. Docker supplies Java 8 for compilation, class inspection and Java
tests; no host JDK is required. Prepare the host-test image once:

```sh
docker build -t carplay-rgi-host-tests:local -f tests/Dockerfile.host .
```

Keep these directories outside the repository:

- firmware: `Firmware.psd1`, reconstructed `lsd.jar`, original `lsd.jxe`, and
  byte-exact stock `smartphone_integrator.json` and `dio_manager.json`;
- dependency cache: pinned OSGi and ASM JARs (downloaded when missing);
- work: source exports, compiler output and test evidence;
- packages: completed SD overlays and their `Archive` directory.

The profile schema is 2. Required keys are `SchemaVersion`, `Name`,
`FirmwareTrain`, `MmxImage`, `LsdJxeSha256`, `LsdJarSha256`, `StockSmartphone`
and `StockDio`. Each stock table contains `Bytes`, `Cksum` (POSIX cksum) and
`Sha256`. Hashes are uppercase SHA-256. Obtain these from independently verified
stock files; do not label files from an existing modified install as stock.
Unknown or missing keys are refused. No real firmware profile is shipped here.

```powershell
./packaging/Build-Package.ps1 -FirmwareRoot <external-firmware-directory> `
    -Dependencies <external-cache> -WorkRoot <external-work> `
    -OutputRoot <external-packages> -Ref HEAD
```

The Java image defaults to `eclipse-temurin:8-jdk-jammy` and is downloaded if
missing. `-JavaImage <image-id-or-digest>` selects a reviewed image; its resolved
immutable ID is recorded in the package manifest and reused for the Java steps.
The package uses the verified `lsd.jar` for both firmware classes and the car's
Java library. Shell overrides for separate stock/JCL inputs apply to component
development, not firmware package preparation. `-JavaHome` remains accepted;
current revisions ignore it, while historical `-Ref` builds retain their original
host-JDK requirement and receive that option.

The checkout must be clean. `-Ref` resolves locally, without fetching or pushing.
The package is named `mib2q-carplay-rgi_<Name>_source_<commit>`; an optional
alphanumeric `-Label` is appended, for example a local trial number.
All source is exported with LF line endings. The recorded immutable toolchain
image ID is used by both native builds; `-ExpectedToolchainImageId` can enforce
a previously reviewed ID. Generated packages contain private stock configuration
and rollback materials and must not be committed or published.

Publication happens only after compilation, Java linkage/resource/version checks,
native checks, installer fault/recovery tests and package checksum verification.
Previously completed packages move to `Archive` only when the replacement is
ready. `-ForceRebuild` permits replacing the same named package; its prior copy
is still archived. A failed publication restores any packages already moved.

## Installation and rollback

Copy the generated `sdcard/` contents onto the M.I.B. card. The overlay includes
`mod/command.sh`, which forwards to `mod/custom.sh`, and the complete
`mod/carplay-rgi/` package. Disconnect CarPlay and run the corresponding M.I.B.
Individual Script / Custom Script menu action once.

The installer verifies payload identities and either exact stock configuration
or a complete prior managed installation. It takes and verifies SD and on-unit
stock backups, stages replacements, preserves live config permissions, commits
runtime files before configs, and writes managed state last. It restores the
firmware mounts read-only and releases the action lock before reporting success.

Before any write, the installer also checks the unit's HMI class library,
`/ifs/lsd.jxe`, against the `lsd.jxe` the package was built from. The patch JAR is on
J9's boot classpath and was compiled and link-checked against exactly that library;
on another one (a different firmware variant, or the same car after a firmware
update) it may stop the HMI from booting. Rollback does not check it, so a changed
library never blocks removal.

Only a reported **install result 0** saves and verifies `ACTION=rollback`. A
later failure saving that setting is reported separately; check the SD action
before running it again. Every unsuccessful install leaves ACTION unchanged.

The next run captures logs before rollback, then restores verified stock configs
and removes only recognized, installer-owned files. Rollback requires its exact
ownership marker and verified on-unit backups. Unknown changes are refused.
Rollback also deletes the renderer's shader cache in
`/mnt/persist/var/app/luka_carplay_maneuver`, which the renderer creates at
runtime. Only the cache's own file names are removed, never recursively; anything
else in that directory is left in place with a warning.
`-ArmRollback` can prepare an overlay already set to rollback. There is no
automatic reboot or process termination; review results and restart manually.

Older managed packages without a runtime monitor entry can be upgraded when
that file is absent. A partial upgrade can be rolled back when the file matches
the incoming package. An unrelated monitor is never adopted or removed.

The former upstream flat/tree installer used `.carplay-stock` backups without
this managed state contract. Remove those installations using their original
release before installing a source package. Keep that recovery material until
the old installation is removed. This builder does not recursively search for
or replace unknown modifications such as another navigation patch.

## Diagnostics and limits

`deploy/mib/collect-logs.sh` is the one collector source. Package preparation
copies it unchanged for the bounded, foreground pre-rollback capture. Standalone
live capture launches a detached MMX worker, acknowledges arming, delays so the
user can return to CarPlay, then takes bounded audio/system/runtime snapshots.
Follow the timing printed by the script. Check the saved status before treating
a capture as complete. A failed or timed-out diagnostic never prevents rollback.

Rollback runs from the M.I.B. menu, which needs the HMI to boot. If a JAR ever
stops the HMI from starting (upstream issue #24: a patch linked against another
firmware variant), M.I.B. is unavailable. Recovery is then an SD card that runs
M.I.B. automatically at boot (`Swdlautorun.txt`) to delete the JAR, or removing the
head unit and deleting it over a serial (UART) console. Roll back before any
firmware update: the installed JAR would meet the new library at the next boot.

Local tests verify software contracts, not the vehicle's display and audio
hardware. Test the resulting package on the intended firmware/cluster before
claiming compatibility. Review logs for personal data before publishing them.
