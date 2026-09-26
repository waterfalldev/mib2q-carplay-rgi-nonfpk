---
title: maneuver_render - protocol, scene engine & visible area
tags: [cluster, renderer, protocol, verified]
status: verified-source
sources:
  - code: maneuver_render/protocol.h
  - code: maneuver_render/main.c
  - code: maneuver_render/maneuver_command.h
  - code: maneuver_render/arrow_progress.h
  - code: maneuver_render/visible_area.h
  - code: maneuver_render/scene/
  - code: java_patch/com/luka/carplay/rgd/RendererServer.java
  - code: java_patch/com/luka/carplay/rgd/RendererMapper.java
  - code: java_patch/com/luka/carplay/cluster/ClusterLayerController.java
  - code: scripts/build_renderers.sh
  - test: scripts/test_maneuver_native.sh
  - test: tests/RendererViewportTest.java
  - test: tests/RendererServerTransportTest.java
reconciles:
  - mib2q-carplay-rgi-next docs/cluster-and-rgi/HUD_RENDERER.md
---

# maneuver_render - protocol, scene engine & visible area

`maneuver_render` draws the 3D maneuver arrow (plus the lane strip) into displayable 98, 328x181
(180 px content + 1 ECC row), transparent when idle. On a MOST cluster the window is instead
the arrows view's stream size, opaque, with the content rendered natively in its centre
([most-cluster](most-cluster.md)). It is a TCP **client** of Java's
`RendererServer` on `127.0.0.1:19800`; the supervisor owns the process, Java never spawns or kills it
([supervisor-lifecycle](../deploy/supervisor-lifecycle.md)). Plane routing and geometry: [display-contexts](display-contexts.md), [kdk-geometry](kdk-geometry.md).

## 📋 Context

> `BAPBridge` -> `RendererMapper` -> `RendererServer` :19800 -> **maneuver-renderer** -> displayable 98
> -> [compositing](compositing.md). Progress: [bargraph-sync](../rgd/bargraph-sync.md). Lanes: [lane-guidance](../rgd/lane-guidance.md).

## 🌐 Commands (Java -> renderer, fixed 48-byte packets `{cmd, flags, payload[46]}`)

| ID | Command | Payload |
|---:|---|---|
| 0x01 | `CMD_MANEUVER` | icon, direction, exit angle, driving side, up to 18 junction angles (below) |
| 0x02 / 0x03 / 0x05 | `SCREENSHOT` / `SHUTDOWN` / `DEBUG` | dev only |
| 0x04 | `CMD_PERSPECTIVE` | `[0]` 0 flat / 1 3D |
| 0x06 | `CMD_PROGRESS` | `[0]` remaining level 0-16, `[1]` mode, `[2]` state (flag 0x20) - [bargraph-sync](../rgd/bargraph-sync.md) |
| 0x07 | `CMD_CLEAR` | drop maneuver + progress + lanes, render transparent; link stays up |
| 0x08 | `CMD_VISIBLE_AREA` | x, y, w, h as four BE u16, source pixels, top-left origin |
| 0x0c-0x0e | `CMD_LANES_BEGIN/LANE/COMMIT` | atomic lane batch - [lane-guidance](../rgd/lane-guidance.md) |

0x09-0x0b (lane-road scene commands) are retired.

**`CMD_MANEUVER`**: `[0]` icon (`ICON_NONE/APPROACH/TURN/UTURN/MERGE/EXIT/ROUNDABOUT/ARRIVED/
LANE_CHANGE/ROUNDABOUT_EXIT`), `[1]` direction -1/0/+1, `[2..3]` exit angle i16, `[4]` driving side,
`[5]` junction count, `[6..41]` junction angles i16; `[42]` progress state (flag 0x20), `[43]`
perspective, `[44..45]` level/mode.

| Flag | Name | Meaning |
|---:|---|---|
| 0x01 | `MAN_FLAG_SET_PERSP` | apply `[43]` after the transition |
| 0x02 | `MAN_FLAG_PROGRESS` | `[44..45]` carry level/mode |
| 0x04 | `MAN_FLAG_BAP_GEOMETRY` | angles are signed **half-degrees** (keeps every 22.5 deg BAP bin); no snap |
| 0x08 | `MAN_FLAG_REFRESH` | replace the latest geometry without restarting the transition |
| 0x10 | `MAN_FLAG_SNAP_TO_ROAD` | raw roundabout roads include the active exit: snapping allowed |
| 0x20 | progress flag | explicit progress state in `[42]` (`CMD_PROGRESS`: `[2]`) |

A refresh coalesced behind a new maneuver keeps the transition (`cr_merge_maneuver_flags`).

## 🌐 Events (renderer -> Java)

`EVT_HEARTBEAT` 0x80 every 1 s (Java drops the client after 5 s of silence) - `EVT_READY` 0x81 (EGL up)
- `EVT_FRAME_READY` 0x82 (a maneuver frame was swapped) - `EVT_FRAME_CLEARED` 0x83 (CLEAR processed;
a later FRAME_READY belongs to new content). `BAPBridge.isPresentationReady()` requires the BAP session
plus FRAME_READY; CLEAR resets it, so Java never accepts an in-flight FRAME_READY for old content.
This gates presentation replay, not the context ([rgd-activation](../rgd/rgd-activation.md)).

## ⚙️ What Java sends (`RendererMapper`)

The BAP family from [maneuver-mapping](../rgd/maneuver-mapping.md) plus raw CarPlay geometry, all as half-degrees:
roundabouts keep the full raw road list (including the active exit, `snapToRoad`), turns/approaches
carry their side roads, ramps are one slight `ICON_TURN`, and off-ramps add the continuing main road
(angle 0, `withForwardRoad`). `ICON_ARRIVED` draws the animated destination flag from
`flag_atlas.rgba` (installed next to the binary).

## 📊 Visible area

The renderer frames the arrow for the part of the 328x180 frame the VC actually shows:

- `ClusterLayerController.maneuverViewport()` returns the active stage's crop - popup (Layout
  118-121) or in-tube (122-125) - with the stage taken from **VC FctID 54** once seen, else the stock
  hint ([kdk-geometry](kdk-geometry.md)).
- A stage or Layout change fires the `ViewportListener`; `RouteGuidance`'s worker calls
  `BAPBridge.refreshRendererViewport()` -> `RendererServer.sendVisibleArea` -> `CMD_VISIBLE_AREA`.
- The renderer animates to the new rect over **250 ms** (quintic ease, zero velocity at both ends);
  a retarget continues from the current rect, an identical rect is a no-op. Invalid input and the
  pre-first-area default is the popup crop `(59, 27, 210x153)`.

## ⚙️ Scene engine (C++)

`scene/` (`scene.cpp`, `geometry.cpp`, `layout.cpp`, `lane_panel.cpp`) is C++11 behind a C ABI
(`scene/scene.h`), built `-fno-exceptions -fno-rtti` into `build/libmaneuver_scene.a` and linked into
the C renderer. `build_renderers.sh` fails the build if the scene objects reference any C++ runtime
symbol (`__cxa*`, `_ZSt*`, typeinfo/vtables, `new`/`delete`, personality). A scene is prepared per
maneuver (immutable input + generation); geometry it cannot fit falls back to the built-in maneuver
family (side roads, roundabouts, arrival flag, elevation). The arrow fill/blink is `arrow_progress.h`;
the progress state comes from the wire and is only eased locally.

## 💾 Shader cache

The Adreno 320 driver compiles GLSL through `libllvm-qcom` at every launch, so the first start after
each boot paid for every program. `common/gl_program_cache.h` saves each linked program with
`GL_OES_get_program_binary` to `/mnt/persist/var/app/luka_carplay_maneuver/` and later launches load
it instead of compiling. The key is FNV-1a 64 of tag (attribute bindings) + both sources +
`GL_RENDERER` + `GL_VERSION`, so a new shader or a firmware driver update misses; a rejected, corrupt
or foreign binary is deleted and recompiled. No offline compiler exists for this driver, so the cache
fills itself on the unit on the first launch. The uninstaller removes the directory.

Renderer milestones (`starting`, `EGL`, `connected`) and every Java log line carry an
`HH:MM:SS.mmm` stamp on the same clock as the hook log; the renderer retries the Java socket every
200 ms instead of 1 s.

## ⚠️ Robustness

- **Progress watchdog** - a thread watches the render loop's progress counter; with no progress for
  5 s (15 s before the loop starts) it writes the stuck phase (`progress watchdog TIMEOUT
  phase=<stage>`) and `_exit(89)`s. A wedged `eglSwapBuffers` otherwise leaves a live PID the
  supervisor cannot tell from a healthy one; the supervisor restarts the process.
- **Window health check** every 5 s recreates the managed window if the handle is invalidated or the
  DisplayManager disowned id 98.
- No `dmdt`: `platform_ensure_focus()` is a no-op on QNX; context routing is Java's
  ([display-contexts](display-contexts.md)).

## 🧪 Build & test

`./scripts/build_renderers.sh` (Docker `qnx65-armv7-toolchain`; the script header names GCC 8.5) builds
the scene archive with `g++`, links `build/maneuver_render` against synthesized Screen/EGL/GLES stubs,
and rejects any non-ARM or emutls-carrying binary. On macOS `make -C maneuver_render` builds the GLFW
dev renderer + harness. Host tests: `scripts/test_maneuver_native.sh` (lane decoder, scene engine,
maneuver parity under ASan/UBSan) and the C tests in `scripts/run_tests.sh` (including `gl_program_cache_test`: store, hit, miss on a
changed source or binding, driver reject, corrupt file, no save after a failed link).
