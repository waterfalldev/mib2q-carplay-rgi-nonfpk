# Preparing, installing and recovering a source package

The shared builder lives in `packaging/`. It builds Java, the native hook,
renderer, supervisor scripts and M.I.B. tooling from one committed Git revision.
The input profile contains stock-file identities only. It never selects patches
or embeds local test sources into the fork.

## Inputs and build

Use PowerShell 7, JDK 8, Git, Docker and the QNX ARMv7 toolchain image described
in the root README. Prepare the host-test image once:

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
    -OutputRoot <external-packages> -JavaHome <jdk8-directory> -Ref HEAD
```

The checkout must be clean. `-Ref` resolves locally, without fetching or pushing.
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

Only a reported **install result 0** saves and verifies `ACTION=rollback`. A
later failure saving that setting is reported separately; check the SD action
before running it again. Every unsuccessful install leaves ACTION unchanged.

The next run captures logs before rollback, then restores verified stock configs
and removes only recognized, installer-owned files. Rollback requires its exact
ownership marker and verified on-unit backups. Unknown changes are refused.
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

Local tests verify software contracts, not the vehicle's display and audio
hardware. Test the resulting package on the intended firmware/cluster before
claiming compatibility. Review logs for personal data before publishing them.
