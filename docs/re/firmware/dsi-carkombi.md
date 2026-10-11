---
title: DSICarKombi + DSIKombiSync2
tags: [re, firmware, dsi, cluster, verified]
status: verified-source
sources:
  - firmware: org.dsi.ifc.carkombi.DSICarKombi (v2.11.30)
  - firmware: org.dsi.ifc.kombisync2.DSIKombiSync (v2.11.0)
reconciles:
  - docs/reference/DSI_CARKOMBI_KOMBISYNC2_REFERENCE.md
  - docs/reference/vc_fpk_state_machine.md
---

# DSICarKombi + DSIKombiSync2

Two independent DSI interfaces expose Virtual Cockpit state to Java in `lsd.jxe`. They answer
different questions; use them together for full cluster state. Context for
[display-contexts](../../cluster/display-contexts.md) / [steering-wheel](../../input/steering-wheel.md).

## 🔍 `DSICarKombi` (v2.11.30) - preset + layout

Per-display user preset, layout and content selection. Key attributes:

| ATTR | ID | Meaning |
|---|---:|---|
| `ATTR_DCACTIVEDISPLAYPRESET` | 99 | active preset type |
| `ATTR_DCDISPLAYVIEWCONFIGURATION` | 100 | layout mode (normal / large / view1..10) |
| `ATTR_DCDISPLAY1MAINSELECTION` | 89 | content of the left display zone |

This is the **Classic vs Sport** style axis that [kdk-geometry](../../cluster/kdk-geometry.md) keys its layout table on.

## 🔄 `DSIKombiSync` (v2.11.0) - real-time sync

MMI<->Kombi display-sync state: active tab, focus, popups, and the **LVDS/HMI flags** used by the video
path (`LVDS_Available`, HMI-active bits). Complements [komo-widget-video](komo-widget-video.md)'s `gfxAvailable`.

## ⚙️ Use in the patch

The cluster context manager reads confirmed VC context to gate input (the steering-wheel OK press is
only acted on the confirmed map tab - see [steering-wheel](../../input/steering-wheel.md)) and to know when CarPlay actually owns
terminal 1. Related state machine: `vc_fpk_state_machine`.
