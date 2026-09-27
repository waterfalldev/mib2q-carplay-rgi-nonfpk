---
title: MOST clusters - CarPlay maneuvers in the stock arrows view
tags: [cluster, most, renderer, komo]
status: verified-source
sources:
  - code: java_patch/com/luka/carplay/cluster/ClusterPlatform.java
  - code: java_patch/com/luka/carplay/cluster/MostPresentation.java
  - code: java_patch/de/audi/tghu/navi/app/cluster/CarPlayKOMOService.java
  - code: java_patch/de/audi/tghu/fwhmi/DisplayManagerMIB2High.java
  - code: maneuver_render/platform_qnx.c
  - code: maneuver_render/render.c
  - code: maneuver_render/maneuver.c
  - code: maneuver_render/protocol.h
  - test: tests/ClusterOwnershipTest.java
  - test: tests/MostArrowsTest.java
  - test: tests/MostViewHandshakeTest.java
  - test: tests/most/most_output_platform_test.c
  - test: tests/most/most_output_render_test.c
  - test: tests/most/most_mask_cache_test.c
---

# MOST clusters - CarPlay maneuvers in the stock arrows view

Cars without a Virtual Cockpit can still have a colour centre display in an analogue cluster.
On a map-over-MOST coding the head unit H.264-encodes the cluster's map and arrows views and
streams them over MOST. CarPlay's maneuver is shown in the stock **arrows view** on these
cars; the stock map view keeps the Audi map, with CarPlay's guidance text in its fields.

## 📋 Context

> [display-contexts](display-contexts.md) - which planes each context selects.
> [compositing](compositing.md) - HU -> MOST encoder -> cluster.
> [maneuver-renderer](maneuver-renderer.md) - the renderer and its protocol.

## 🧭 Choosing the presentation

`ClusterPlatform` reads stock sysConst 541 (`KOMBI_MAP_MODE`) once CarPlay's framework is ready.
When the coding cannot be read, stock is left alone.

| 541 | Stock classification | This patch |
| --- | --- | --- |
| `2` | `isClusterMapFPK`, Virtual Cockpit | Upstream behavior: context 80, KDK planes 101/102, layer controller. |
| `1` | `isClusterMapMOST`, map/arrows over MOST | Stock view selection stays in charge; CarPlay's maneuver replaces the stock arrows image during CarPlay guidance. |
| `0` | `isClusterRGI`, cluster draws BAP arrows | Stock context and view state are not taken. Not vehicle-tested. |

Only a Virtual Cockpit takes terminal-1 contexts or writes the KDK planes. On other codings
`configureDM()` never creates 101/102, so the Virtual Cockpit takeover would only break stock
state restoration.

## 🔀 Arrows-view handshake

Stock `ClusterViewMode` decides between map, arrows and compass. During CarPlay guidance
`MostPresentation` and the stock-state gate:

- hold the effective KOMO view visibility while still recording the cluster's real reports;
- let stock's ActiveRGType (BAP FctID 39) answer pass while RGStatus stays gated;
- keep CarPlay's view answer in step with stock's, and defer the phone-navigation info state
  until release.

Stock still performs the kombi context change, view selection and fade-in. On release every
latest stock value is replayed.

## 🖼️ Output size and readiness

The MOST encoder streams the leading displayable of the active context as-is, at the coding's
KOMO view size (800x252 on the tested cluster, not the Virtual Cockpit's 328x181). It does not
crop or scale the source. So the renderer window must be that size:

| File | Writer | Content |
| --- | --- | --- |
| `/tmp/carplay_most_output` | Java, at connect and when the size changes | `WWWW HHHH\n`, stock KDK's measured size (64-2048), else 800x252 |
| `/tmp/carplay_most_output_ready` | renderer, after every window it presents | `WWWW HHHH <pid>.<serial>\n`, a new token per window |

Both files are written in place: the head unit's `/tmp` cannot `rename()`. Readers accept only a
complete, valid line and otherwise keep their current state. The renderer checks the request
every second and recreates window 98 at that size, opaque. It withdraws the ready report before
any window change, at startup and at shutdown.

Java composes context 81 (`{98}`) in place of stock context 73 only once the ready report
matches its request. A new token (renderer restart, window recovery, resize) causes a fresh
73 -> 81 switch so the encoder follows the new surface. After 7 s without a matching report Java
composes once with an explicit unconfirmed-output log. That output may be blank.

## 🎨 Native-size rendering

The 328x181 frame is aspect-fitted into the window and centred: at 800x252 the content is
457x252 at x=171, and everything else is opaque black with alpha 1.

On an opaque MOST output the scene renders at the content size (457x252) with FXAA and no
supersampling, and the final pass copies it 1:1 into the window. The transition masks keep 1.6x
density (2560x1411 at this size, covering the whole slide): below about 1.5 the arrow shows dark
notches. The Virtual Cockpit keeps upstream's 1.6x supersampling. The layout stays in the same
328x181 logical units. Frames are paced evenly at 30 fps.

Each push paints the current and next maneuver's masks once into two mask sets and then only
composites them. A frame paints at most one set while the next maneuver is still invisible.

If the render targets exceed the GL size limits, fail to allocate or are incomplete, the renderer
falls back to the platform size and does not try that size again. The log records the render
size, offscreen memory and free system memory before and after.

For diagnostics the renderer saves the settled maneuver frame, exactly as streamed, to
`/tmp/carplay_most_frame.ppm` once per settled maneuver, at most every 10 s. It skips this
while animating, and does not repeat it while the same maneuver stays on screen. The M.I.B.
collector copies this file and both handshake files.

## 🎞️ Stream rate

The encoder captures the arrows view at the rate stock sets for terminal 1: 10 fps at KOMO data
rate 2, and 1 fps at data rate 1, which the cluster reports for 1-2 s after every view change.
While CarPlay's arrows view is composed, `MostPresentation` has `DisplayManagerMIB2High` send
stock's 10 as 30. Stock's 1 and 0 pass unchanged. 30 is applied when the view is composed and
stock's rate is handed back on release.

## 📝 Guidance text

`CarPlayKOMOService` claims maneuver distance, streets, arrival time, remaining time and
destination distance during guidance. It uses stock formatters and unit conversions, and keeps
the latest stock value for each field. On release it replays those values, or stock's invalid
encoding where none exists.

## ✅ Validation and limits

- Host tests cover the coding selection, handshake, text ownership and release (Java, against
  the stock JAR), the request/readiness protocol, window recreation and the final pass,
  render-target fallback and frame capture (C, with fake QNX Screen/EGL/GL).
- Vehicle-tested on one MHI2Q unit with 541=1 and an 800x252 arrows view: CarPlay maneuvers in
  the arrows view at native size, map-view round trips and stock-state release on disconnect.
  Other trains and cluster variants have not been tested.
- A rapid electronic ticking during animated guidance on that unit, never with a still arrow,
  followed the renderer's GPU load. Every frame of a slide used to repaint the 2560x1411 masks;
  painting them once per push removed nearly all of it.
- Upstream's REPLACE mode still cancels an active Audi route when the phone connects.
