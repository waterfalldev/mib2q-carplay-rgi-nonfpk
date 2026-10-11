---
title: Java production cleanup audit - reconciled to current branch
tags: [maintenance, java, verified]
status: verified-source
sources:
  - code: java_patch/com/luka/carplay/rgd/BAPBridge.java
  - code: java_patch/com/luka/carplay/rgd/RouteGuidance.java
  - code: java_patch/com/luka/carplay/core/ScreenModule.java
  - code: java_patch/com/luka/carplay/core/SteeringWheelInputModule.java
  - code: java_patch/com/luka/carplay/rgd/RendererServer.java
  - code: java_patch/com/luka/carplay/core/FrameworkRef.java
---

# Java production cleanup audit - reconciled to current branch

Scope: `java_patch/` against stock MU1316, **verified against the current branch**. Only what is still
open or must be kept is listed; resolved findings are not repeated.

## 📋 Context

> [architecture](../architecture.md) - Java patch layer -> cluster HUD + route-info - KOMO gfx gate -> [komo-widget-video](../re/firmware/komo-widget-video.md)
> - route-info toggle publishes [bap-fctids](../rgd/bap-fctids.md) FctID 22

**Bottom line:** no cleanup item is open on this branch. The earlier findings (trace helpers, unused
accessors, the fake `ClusterService` pipeline API, the KOMO reflection ladder) are resolved and
verified absent from `java_patch/`; what remains below is what must **stay**.

## ✅ Live path - do not remove: route-info phase

The steering-wheel roller press drives the route-info phase:

`SteeringWheelInputModule` -> `ScreenModule.onSteeringWheelOkPressed()` ->
`RouteGuidance`'s `InfoModeListener.onInfoModeToggle()` -> `requestInfoModeToggle()`
(`desiredInfoPhase ^= 1`) -> `BAPBridge.infoPhase` and the trip-summary FctID 22 path.

`ScreenModule.InfoModeListener` is a registered interface, bound by `RouteGuidance` in start and
cleared on stop. `BAPBridge.infoPhase` / `buildTripSummary` / `lastDistanceToDestinationM` are
therefore reachable. See [steering-wheel](../input/steering-wheel.md).

## 📌 Keep (do not delete for size)

- The six stock replacement classes: their public/protected ABI is complete and stock code calls
  into them outside local reachability.
- `CarplayBus` (:19810) and `RendererServer` (:19800) - different peers/protocols.
- `RgdModule`, `FrameworkRef.ServiceHandle`, `Module` - ordered lifecycle + paired OSGi release.
- `TouchpadController`, mapper classes, lane fallback.
- The cover-art replacement/provider/mux chain (live; intentionally preserves the stock provider).
- Trip-summary ETA helpers (`lastEtaSeconds`, `lastTimeRemainingSeconds`,
  `lastTimeRemainingSampleSeconds`, `currentRemainingSeconds()`, `currentArrivalSeconds()`) - they
  publish the real FctID 22 absolute ETA.
- `RendererServer.isConnected()` (used in `BAPBridge` renderer recovery), `CarplayBus.Data.bool()`
  (used by `ClusterVideo`) and `ScreenModule.isConnected()` (pins the cluster while CarPlay owns it;
  called from `CombiMapController` and `ClusterService`).

## ✍️ After further Java cleanup

Rebuild, rerun the stock-ABI/linkage audit (`scripts/audit_java_stock.sh`) and the host suites
(`scripts/test_route_info.sh`, `scripts/test_java_transports.sh`), then test on the unit: cold boot,
RGI start/stop, View changes, renderer death/reconnect, stock navigation after disconnect, cover art.
