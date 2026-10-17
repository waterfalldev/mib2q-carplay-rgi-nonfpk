# MOST Cluster

*How CarPlay route guidance reaches an analogue instrument cluster that takes its map and arrows view over MOST (sysConst 541 = 1): the stock arrows view with CarPlay's maneuver in it, the KOMO guidance text, the renderer's native-size output, and which parts of the Virtual Cockpit design stay out of the way. Status: **vehicle-tested on one MU1329 unit**; Virtual Cockpit regression checks on the combined tree are still open.*

---

## 📋 Summary

Cars without a Virtual Cockpit can still have a colour centre display in an analogue cluster. On a map-over-MOST coding the head unit H.264-encodes the cluster's map and arrows views and streams them over MOST. CarPlay's maneuver is shown in the stock **arrows view** on these cars; the stock map view keeps the Audi map, with CarPlay's guidance text in its fields, except during CarPlay navigation, when it shows the phone's own cluster map ([MOST Map View](MOST_MAP_VIEW.md)). `[code]` (`java_patch/com/luka/carplay/cluster/MostPresentation.java`, `CarPlayKOMOService.java`)

The Virtual Cockpit design in [Cluster Contexts](CLUSTER_CONTEXTS.md) does not apply here: there are no KDK backings 101/102 and no ctx 80, and the phone's map does not go through altscreen_render. See "Choosing the presentation" for where the two paths split.

## 🧭 Choosing the presentation

`ClusterPlatform` reads stock sysConst 541 (`KOMBI_MAP_MODE`) once CarPlay's framework is ready. When the coding cannot be read, stock is left alone. `[code]` (`ClusterPlatform.java`)

| 541 | Stock classification | This patch |
|---|---|---|
| `2` | `isClusterMapFPK`, Virtual Cockpit | The altScreen design: contexts 80 and 81, KDK planes 101/102, plane 99, layer controller. |
| `1` | `isClusterMapMOST`, map/arrows over MOST | Stock view selection stays in charge; CarPlay's maneuver replaces the stock arrows image during CarPlay guidance. |
| `0` | `isClusterRGI`, cluster draws BAP arrows | Stock context and view state are not taken. Not vehicle-tested. |

Only a Virtual Cockpit takes terminal-1 contexts or writes the KDK planes. On other codings `configureDM()` never creates 101/102, so the takeover would only break stock state restoration. `[code]` (`DisplayManagerMIB2High.java`, `ClusterLayerController.reapplySerialized`)

### Where the two paths split

`AltScreenModule.isPlatformSupported` accepts every cluster except G24, so a MOST car reaches the altScreen module. The split is made in the module and in the classes it feeds. `[code]`

| Place | Virtual Cockpit | MOST and RGI-only |
|---|---|---|
| `AltScreenModule.start()` | takes the DisplayManager, binds the layer controller, starts the single context writer | records the session (`isConnected`), starts nothing, `clusterContextsOwned = false`; on MOST also starts `MostPresentation` |
| `AltScreenModule.republish()`, `setVideoAvailable()` | publish the hook RGI state and the plane-99 NAVSD gate | return early: no hook traffic, no gate, decoded video does not count as plane ownership |
| `AltScreenModule.setNavActive()` | holds ctx 80 until VC withdraws the KDK (Fct44) | no hold (that hide never arrives); on MOST it drives `MostPresentation` |
| `AltScreenModule.setViewAreaMode()` | sends the mode to the hook and the renderer | only the local geometry and listeners |
| `AltScreenModule.ownsClusterContext()` | `DisplayManagerMIB2High.switchContext` drops stock writes, `CombiMapController` pins the cluster | false: stock keeps switching and the View button works |
| `AltScreenCombiBAPListener` | consumes the steering-wheel zoom step for CarPlay | stock `setMapScale` runs |
| `GatedCombiService` | hides map scale and altitude while route guidance is blocked | stock scale and altitude stay visible; ActiveRGType passes on MOST |
| `ClusterLayerController` | writes 98/101/102 | silent |

The gate is `ClusterPlatform.leavesContextsToStock()` for the altScreen paths (true for 541 = 1 and 0, false for 541 = 2 and for an unknown coding, so Virtual Cockpit behaviour is the default) and `AltScreenModule.ownsClusterContext()` for the terminal-1 writes.

### Context 81 means two things

Both codings define `dc[81]`, and the id has a different meaning on each. Which one exists is fixed by the coding when `DisplayManagerMIB2High.configureDM()` builds the table. `[code]`

| Coding | `dc[80]` | `dc[81]` |
|---|---|---|
| 541 = 2 | `{98, 101, 102, 99}` navigation | `{99}` map only (`AltScreenModule`) |
| 541 = 1 | built, never selected | `{98}` arrows view (`MostPresentation`) |

On 541 = 1 the "stock asked for 81, so it meant stock's arrows view (73)" rewrite in `switchContext` applies; on 541 = 2 it is skipped, because the altScreen writer selects 81 itself. `StockContextReporter` still tells stock's listener the context stock asked for (73), never 81, so stock's model 168 stays correct. A free id was not used because the arrows-view context and its tests already name 81 and the table has room for only one meaning per car.

## 🔀 Arrows-view handshake

Stock `ClusterViewMode` decides between map, arrows and compass. During CarPlay guidance `MostPresentation` and the stock-state gate:

- hold the effective KOMO view visibility while still recording the cluster's real reports;
- let stock's ActiveRGType (BAP FctID 39) answer pass while RGStatus stays gated;
- keep CarPlay's view answer in step with stock's, and defer the phone-navigation info state until release.

Stock still performs the kombi context change, view selection and fade-in. On release every latest stock value is replayed.

## 🖼️ Output size and readiness

The MOST encoder streams the leading displayable of the active context as-is, at the coding's KOMO view size (800x252 on the tested cluster, not the Virtual Cockpit's 328x181). It does not crop or scale the source. So the renderer window must be that size:

| File | Writer | Content |
|---|---|---|
| `/tmp/carplay_most_output` | Java, at connect and when the size changes | `WWWW HHHH\n`, stock KDK's measured size (64-2048), else 800x252 |
| `/tmp/carplay_most_output_ready` | renderer, after every window it presents | `WWWW HHHH <pid>.<serial>\n`, a new token per window |

Both files are written in place: the head unit's `/tmp` cannot `rename()`. Readers accept only a complete, valid line and otherwise keep their current state. The renderer checks the request every second and recreates window 98 at that size, opaque. It withdraws the ready report before any window change, at startup and at shutdown.

Java composes context 81 (`{98}`) in place of stock context 73 only once the ready report matches its request. A new token (renderer restart, window recovery, resize) causes a fresh 73 -> 81 switch so the encoder follows the new surface. After 7 s without a matching report Java composes once with an explicit unconfirmed-output log. That output may be blank.

## 🎨 Native-size rendering

The 328x181 frame is aspect-fitted into the window and centred: at 800x252 the content is 457x252 at x=171, and everything else is opaque black with alpha 1.

On an opaque MOST output the scene renders at the content size (457x252) with FXAA and no supersampling, and the final pass copies it 1:1 into the window. The transition masks keep 1.6x density (2560x1411 at this size, covering the whole slide): below about 1.5 the arrow shows dark notches. The Virtual Cockpit keeps 1.6x supersampling. The layout stays in the same 328x181 logical units. Frames are paced evenly at 30 fps.

Each push paints the current and next maneuver's masks once into two mask sets and then only composites them. A frame paints at most one set while the next maneuver is still invisible.

If the render targets exceed the GL size limits, fail to allocate or are incomplete, the renderer falls back to the platform size and does not try that size again. The log records the render size, offscreen memory and free system memory before and after.

While the `carplay_verbose` marker exists (the M.I.B. log collector sets it), the renderer saves the settled maneuver frame, exactly as streamed, to `/tmp/carplay_most_frame.ppm` once per settled maneuver, at most every 10 s. It skips this while animating, and does not repeat it while the same maneuver stays on screen. Without the marker nothing is read back or held. The M.I.B. collector copies this file and both handshake files.

## 🎞️ Stream rate

The encoder captures the arrows view at the rate stock sets for terminal 1: 10 fps at KOMO data rate 2, and 1 fps at data rate 1, which the cluster reports for 1-2 s after every view change. While CarPlay's arrows view is composed, `MostPresentation` has `DisplayManagerMIB2High` send stock's 10 as 30. Stock's 1 and 0 pass unchanged. 30 is applied when the view is composed and stock's rate is handed back on release.

## 📝 Guidance text

`CarPlayKOMOService` claims maneuver distance, streets, arrival time, remaining time and destination distance during guidance. It uses stock formatters and unit conversions, and keeps the latest stock value for each field. On release it replays those values, or stock's invalid encoding where none exists.

## 🚦 Guidance at CarPlay start

**Start burst.** When guidance starts, the phone sends route states 3, 1, 0, 3, 1 within about 60 ms, and its reset frame (0) comes after maneuvers it never sends again. A reset frame within 2 s (`RGD_START_BURST_MS`) of the subscription's first message, not from rerouting, therefore keeps the route aside, and the route comes back - list, maneuvers and generation - when the state returns before a different list or new maneuver data. A repeated reset frame leaves the kept route; any other reset, a different list, new 0x5202/0x5204 data, a route end or a hard clear drops it (map23-map24, `rgd_start_burst_test`). Without it a start showed no maneuver beyond `icon=1` until guidance was restarted, because Java also drops its copy when the route generation changes. `[code]` (`hook/routeguidance/rgd_hook.c`) `[car]`

**The navigator's late "no route" is held while CarPlay guides.** When CarPlay guidance starts before the Audi navigation has finished starting, the navigator reports no route guidance (`dsiRgActive=false`) a few seconds later. That report overwrote CarPlay's `rgActive` claim: `rgiValid` fell and stock showed COMPASS instead of the arrows (map23). While `carPlayRgiValidOverride` is set, `ClusterService.updateRgActive(false)` is held (`dsiNoRouteHeld`) and replayed when the override is released (`NativeGuidanceStateTest`). `[code]` `[car]`

## ✅ Validation and limits

- Host tests cover the coding selection, handshake, text ownership and release (Java, against the stock JAR: `MostArrowsTest`, `MostViewHandshakeTest`, `ClusterOwnershipTest`, `ClusterCoreTest`), and the request/readiness protocol, window recreation and the final pass, render-target fallback, frame capture and the mask cache (C with fake QNX Screen/EGL/GL: `tests/most/run-native-tests.sh`).
- Vehicle-tested on one MHI2Q unit with 541 = 1 and an 800x252 arrows view: CarPlay maneuvers in the arrows view at native size, map-view round trips and stock-state release on disconnect. Other trains and cluster variants have not been tested. `[car]`
- A rapid electronic ticking during animated guidance on that unit, never with a still arrow, followed the renderer's GPU load. Every frame of a slide used to repaint the 2560x1411 masks; painting them once per push removed nearly all of it.
- The Virtual Cockpit paths in this tree are upstream's, with the platform splits above. Both codings need a vehicle check on the combined tree. `[open]`
- REPLACE mode still cancels an active Audi route when the phone connects.

## 🔗 Related

- [MOST Map View](MOST_MAP_VIEW.md): the phone's cluster map in the MAP view.
- [Cluster Contexts](CLUSTER_CONTEXTS.md): the Virtual Cockpit contexts 74, 80 and 81, and the single writer.
- [HUD Renderer](HUD_RENDERER.md): the maneuver renderer and its protocol.
- [RGI and BAP Bridge](RGI_AND_BAP_BRIDGE.md): the BAP side of route guidance.
- [NavSD FctID Matrix](NAVSD_FCTID_MATRIX.md): who owns each Navigation BAP function while CarPlay drives.

Provenance: `java_patch/com/luka/carplay/cluster/` (`ClusterPlatform`, `MostPresentation`, `ClusterStateTrace`), `java_patch/de/audi/tghu/navi/app/cluster/CarPlayKOMOService.java`, `java_patch/de/audi/tghu/fwhmi/DisplayManagerMIB2High.java`, `maneuver_render/` (`platform_qnx.c`, `render.c`, `protocol.h`) and the tests named above.
