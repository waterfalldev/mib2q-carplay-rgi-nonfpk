# MHI2Q CarPlay cluster integration

CarPlay patch set for Audi MHI2Q infotainment.
(Based on MHI2Q firmware, but may need rebuild for different versions.)

**Disclaimer:** Use at your own risk. These patches modify firmware binaries and system configurations on your infotainment unit. Always back up all original files before making any changes. The authors are not responsible for any damage, bricked devices, or warranty issues resulting from use of these patches.

## 🖼️ Gallery

<p align="center">
  <img src="assets/gallery/maneuver_demo.gif" width="90%" /><br />
  <sub>Cluster maneuver renderer driven through a demo route</sub>
</p>

**Virtual Cockpit: route guidance from the maneuver renderer**

<p align="center">
  <img src="assets/gallery/vc_day_nav.jpeg" height="200" />
  <img src="assets/gallery/vc_night_nav.jpeg" height="200" />
</p>
<p align="center">
  <img src="assets/gallery/vc_full_map.jpeg" height="200" />
  <img src="assets/gallery/vc_lane_guidance.jpeg" height="200" />
</p>

**Audi front PDC no longer hides CarPlay** · **Cover art on the cluster**

<p align="center">
  <img src="assets/gallery/pdc_over_carplay.jpeg" width="45%" />
  <img src="assets/gallery/cover_art.jpeg" width="45%" />
</p>

**Head-up display**

<p align="center">
  <img src="assets/gallery/IMG_0623.jpeg" width="30%" />
  <img src="assets/gallery/IMG_6302.jpeg" width="30%" />
  <img src="assets/gallery/IMG_0599.jpeg" width="30%" />
</p>

## 📍 Contents

- [Gallery](#-gallery)
- [Features](#-features)
- [Repository layout](#-repository-layout)
- [Build](#-build)
- [Deployment](#-deployment)
- [Logging](#-logging)
- [Documentation](#-documentation)
- [Help wanted](#-help-wanted)
- [References](#-references)

## ✨ Features

There is nothing to switch on: plug in the iPhone and CarPlay starts as usual; the cluster
features below follow it automatically.

- **Turn-by-turn on the cluster.** During CarPlay navigation the Virtual Cockpit shows a 3D maneuver
  arrow drawn over the cluster's own native map (the stock map stays; there is no CarPlay map on the
  cluster). The arrow fills as the turn approaches and blinks just before it, lane arrows appear under
  it, and the cluster also shows distance to the turn, arrival time and remaining distance. Needs an
  app that sends CarPlay route guidance: Apple Maps and Google Maps do, AMap does with its CarPlay
  guidance setting on, Waze does not
  ([details](docs/rgd/rgd-activation.md#-which-navigation-apps-send-route-guidance)).
- **Turn-by-turn on MOST clusters.** On an analogue cluster with a colour centre display coded
  for map-over-MOST, CarPlay's maneuver appears in the stock arrows view, rendered at the view's
  native size. The map view keeps the Audi map with CarPlay's distance, street and arrival text.
  Stock view selection stays in charge, and Audi guidance returns on disconnect
  ([details](docs/cluster/most-cluster.md)).
- **Route text in the Virtual Cockpit.** A text line names the exit sign or the next road (the
  current road when there is nothing else); long names scroll. Press **OK** (the left steering-wheel
  roller) to switch it to arrival time and time left, and press again to go back; it returns by itself
  after 20 s ([details](docs/rgd/vc-route-text.md)).
- **Head-up display.** The same maneuver icons, lane arrows and distance appear on the HUD.
- **Steering-wheel roller** keeps zooming the stock cluster map, as without CarPlay.
- **Cover art on the cluster.** The now-playing album art shows on the cluster media screen.
- **Parking popups no longer hide CarPlay.** When the Audi front PDC / parking view pops up beside it,
  CarPlay stays on screen instead of being replaced ([details](docs/hmi/pdc-small-stage.md)).
- **MMI touchpad → DPAD bridging** so finger drags navigate CarPlay menus.

## 🗂️ Repository layout

| Path | Purpose |
| --- | --- |
| `hook/` | Shipping native `libcarplay_hook.so` source |
| `java_patch/` | The only supported Java patch source |
| `java_resources/` | Resources packed into the jar (VC glyph-width / Unicode table `vc-text.bin`) |
| `maneuver_render/` | GLES maneuver overlay renderer (C, plus the C++11 `scene/` engine) |
| `common/` | Shared renderer code: QNX Screen surface, GL program-binary cache, log timestamps |
| `deploy/smartphone_integrator/` | Runtime scripts and child-process configuration for the HU |
| `install_MoreIncredibleBash/`, `uninstall_MoreIncredibleBash/`, `logging_MoreIncredibleBash/` | M.I.B. installation, removal and diagnostic scripts |
| `scripts/` | Docker build entry points (Java / hook / renderer) and host test runners |
| `tests/` | Host tests (C, Java, Python) for the hook, Java bridge and renderer |
| `toolchain/qnx65-abi/` | QNX Screen ABI headers used only for cross-compilation |
| `docs/` | Markdown knowledge base (also opens in Obsidian) - validated RE + implementation notes (open [`docs/INDEX.md`](docs/INDEX.md)) |
| `assets/` | Screenshots and visual reference material |
| `build/` | Canonical deployable artifacts |

Raw unit logs and generated class trees are intentionally kept outside Git.

## 🔧 Build

Native code needs the QNX 6.5 ARMv7 cross-toolchain image from
[luka-dev/qnx65-armv7-toolchain](https://github.com/luka-dev/qnx65-armv7-toolchain). Build it once:

```sh
git clone https://github.com/luka-dev/qnx65-armv7-toolchain
cd qnx65-armv7-toolchain
./host-scripts/qnx-run.sh build        # qnx65-armv7-toolchain:latest (GCC 8.5)
```

Java compilation and tests use `eclipse-temurin:8-jdk-jammy` through Docker;
no host JDK is required. The shell commands and companion package builder share
the compiler and test procedures in `scripts/java/`. Compilation targets Java
1.4 against the car's own Java library when one is supplied (otherwise against
JDK 8's classes), and includes and checks the JAR resources.

For the maintainer's existing macOS/Linux layout, the component command stays:

```sh
./scripts/build_java.sh             # build/carplay_hook.jar
```

It uses `../../Tools/jxe2jar`: `out/MU1316-final.jar`, OSGi JARs in `libs/`, and
the existing car library at `libs/jcl/MHI2Q_US_AUG22_P5087_MU1316/jcl.jar`. The
car library is optional for the build but required by the stock linkage audit.
Tests use `out/MU1316-combined.jar` where executable stock bytecode is required;
the linkage audit uses ASM from `tools/uninline/lib/`. No PowerShell is needed
for these component commands. Set `CARPLAY_TOOLS_DIR` to relocate that layout.

For other firmware, set `STOCK_JAR` and `CARPLAY_DEPENDENCIES` to external paths.
`STOCK_BOOT_JAR` supplies a separate car Java library when it is not embedded in
the stock JAR; `STOCK_RUNTIME_JAR` supplies a separate executable stock JAR for
tests. With an explicit `STOCK_JAR`, both default to that JAR. `ASM_JAR` and
`ASM_TREE_JAR` can override the audit dependencies. `CARPLAY_BUILD_ID` overrides
the Git-derived build ID; `JAVA_OUTPUT` overrides `build/`.
`CARPLAY_JAVA_IMAGE` can select an immutable Java image ID or digest; each run
records its resolved ID in `java-image-id.txt` in the output directory.

Optional checked SD-package preparation lives in the separate
[mib2q-rgi-tooling](https://github.com/waterfalldev/mib2q-rgi-tooling) repository.
It consumes these component commands and records separate application/tooling
commits. Component builds and tests do not require that repository or PowerShell.
Firmware files and generated packages stay outside Git. For the standard M.I.B.
installation workflow, see [installation](docs/deploy/install.md).
`scripts/build_hook.sh` and `scripts/build_renderers.sh` retain the native build;
`QNX_TOOLCHAIN_IMAGE` can pin its immutable image ID.

`scripts/run_tests.sh` runs native, supervisor, installer and logger checks on Linux/macOS.
`tests/most/run-native-tests.sh <checkout>` runs the renderer's MOST output checks against
the real QNX sources inside the toolchain image.
`scripts/test_route_info.sh`, `scripts/test_java_transports.sh` and
`scripts/test_pdc.sh` retain their respective test groups; the parking command
checks an existing JAR. `scripts/check_java.sh` builds and runs all Java groups
and the linkage audit. The suites run two JVMs at a time by default;
`JAVA_TEST_JOBS` overrides that limit. `scripts/test_java_build.sh` checks
reproducible builds, resource failures and the original macOS input layout. The route and complete shell checks also need Python 3
and a host C compiler for their native contract probe; the package builder runs
that part in Docker.
`scripts/audit_java_stock.sh` retains the MU1316 source inventory and separate
final/combined JAR audits. For explicit firmware inputs, set `JAVA_STOCK_SOURCES`
to include that firmware's source inventory.
The companion tooling tests its own managed package installation and recovery.
The application retains tests for its standard M.I.B. scripts. No test connects
to a vehicle. The macOS/MU1316 entry points
have not yet been execution-tested on that setup.

## 🚀 Deployment

**Compatibility.** The patch is not limited to US, EU or CN units, nor to one MU train: it is
meant for any MHI2Q MU firmware (developed on MU1316). The cluster presentation follows the
unit's coding (sysConst 541):

- **Virtual Cockpit** (541=2): the full feature set above.
- **Map-over-MOST analogue cluster** (541=1): maneuvers in the stock arrows view and guidance
  text. Tested on one MU1329 unit with an 800x252 arrows view; other trains and cluster variants
  are untested. A rapid ticking sound during animated guidance has been reported there and is
  not yet explained ([details](docs/cluster/most-cluster.md#-validation-and-limits)).
- **RGI-only cluster** (541=0): stock cluster state is left alone. Untested.

Preferably flash the latest firmware available for the unit before installing the patch. The
patch JAR must be built against the matching stock library. Neither compilation nor
local tests prove behavior on a firmware or cluster that has not been tested in a car.

A release is eight files plus two config edits; nothing stock is replaced and no firewall profile is
touched:

| On-unit path | Files |
| --- | --- |
| `/mnt/app/root/hooks/` | `libcarplay_hook.so`, `maneuver_render` (from `build/`), `flag_atlas.rgba` (from `maneuver_render/resources/`), `carplay_startup.sh`, `carplay_monitor.sh`, `carplay_processes.sh`, `carplay_cleanup.sh` (from `deploy/smartphone_integrator/`) |
| `/mnt/app/eso/hmi/lsd/jars/` | `carplay_hook.jar` (from `build/`) |
| `/mnt/system/etc/eso/production/smartphone_integrator.json` | `children.carplay` replaced by [`carplay_child.json`](deploy/smartphone_integrator/carplay_child.json) |
| `/mnt/system/etc/eso/production/dio_manager.json` | `MessagesSentByAccessory` += `0x5200`, `0x5203`; `MessagesReceivedFromDevice` += `0x5201`, `0x5202`, `0x5204` |

Both the `dio_manager.json` IDs and the hook's runtime Identify patch are required: without the IDs
iOS sends route guidance and the SDK silently drops it.

**With M.I.B.** Copy `install_MoreIncredibleBash/` to the card and put all eight
release files plus `carplay_child.json` into `mod/carplay/`. With CarPlay
disconnected, run M.I.B.'s Custom Script / Individual Script action. The script
checks that the release is complete, copies files using atomic renames, patches
the two configurations and keeps `.carplay-stock` backups. It never stops
processes or reboots. Use `uninstall_MoreIncredibleBash/` to remove that install.
See [installation and recovery](docs/deploy/install.md) for card layouts and
manual installation.

Packages made by the companion tooling use a different managed backup/state
format. Remove an existing installation using its own matching installer before
switching workflows; the upstream and companion uninstallers are not interchangeable.

**Reboot.** Disconnect CarPlay, run `sync` and wait a few seconds, then reboot normally: a forced
reboot (the MMI button combo) right after copying can leave the files truncated or missing. The jar is
on j9's boot classpath, so it only loads after a full restart. On boot `smartphone_integrator` launches
everything; check `/tmp/carplay_hook.log` and `/tmp/carplay_java.log` (see [Logging](#-logging)).

Exact ownership rules, the `LD_PRELOAD`/env constraints and the MU1316 QNX-compat audit are in
[`deploy/smartphone_integrator/README.md`](deploy/smartphone_integrator/README.md).

## 📝 Logging

Everything logs to `/tmp` on the unit:

| File | Source |
| --- | --- |
| `/tmp/carplay_hook.log` | native hook (inside `dio_manager`) |
| `/tmp/carplay_java.log` | Java patch (bounded + rotated, `.1` = previous) |
| `/tmp/maneuver_render.log` | cluster maneuver renderer |
| `/tmp/carplay_wrapper.log` | startup wrapper and renderer monitor |

By default only warnings and errors are recorded. To capture **everything** (lift hook and Java to
`INFO`), drop a marker file on the unit - no rebuild needed:

```sh
touch /mnt/app/carplay_verbose        # survives reboot; /tmp/carplay_verbose does not
```

Hook and Java read the marker at every CarPlay session start, so it takes effect on the next phone
connect - no reboot. Remove the marker to return to the quiet default. Logs reset on reboot, so pull
them before restarting.

For raw route-guidance packet dumps, rebuild the hook with `LOG_RGD_PACKET_RAW=1` (see [Build](#-build)).

**No shell? Use M.I.B.** Copy `logging_MoreIncredibleBash/` to the card and run it
like the installer. Each run saves logs into `<card>/carplay_logs/NNN/` and
enables verbose logging for the next phone connection. Run it again after
reproducing the issue to save that session. Captures can contain private
identifiers; review and redact them before sharing.

## 📚 Documentation

`docs/` is a Markdown knowledge base (also opens in Obsidian) - one note per topic, each fact validated against code /
firmware / iOS binary. Start at [`docs/INDEX.md`](docs/INDEX.md): architecture & threading, the hook
and bus, route guidance (TLV → BAP → cluster, lanes, route text), cluster compositing and the maneuver
renderer, input, deploy/connect, build & host tests, the reverse-engineering references, and a
per-note verification status.

## 🤝 Help wanted

PRs are welcome - bug fixes, new maneuver cases, docs, on-car test reports.

**Reporting a bad maneuver icon.** The iAP2→BAP mapping covers all 54 CarPlay maneuver types but has
only been exercised on a limited set of real routes. A snippet of `/tmp/carplay_hook.log` from the
moment plus a note on what was expected helps a lot. The hook logs unrecognised route-guidance messages
as `[HOOK] Unknown 0x52xx msgid=0xNNNN dir=IN len=N` followed by a hex dump - that line is the best
starting point when iOS sends a maneuver type we don't handle yet.

## 🔗 References

Thanks for the prior work and knowledge that helped figure this out.

- https://github.com/ludwig-v/wireless-carplay-dongle-reverse-engineering
- https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore
- https://github.com/adi961/mib2-android-auto-vc
- [@fifthBro](https://t.me/fifthBro)

---

<sub>What are you doing all the way down here? There's nothing to see…</sub>

<details>
<summary>…or is there?</summary>

<br>

### Coming soon. Maybe. Someday. No promises.

It was just the warm-up, next:

<p align="center">
  <img src="assets/coming-soon.jpg" width="70%" />
</p>

- **AltScreen** - full CarPlay map, right in cluster
- **Multichannel audio support** - from stereo up to 6- or even 8-channel
- **Apple Spatial Audio**
- **Dolby Atmos** - High Quality 5.1.2 masters
- **Video playback** - an Apple TV on wheels

Stay tuned. 👀

</details>

---
