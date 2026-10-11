---
title: MOST MAP view
tags: [cluster, most, prototype]
status: prototype
---

# MOST MAP view

On a map-over-MOST cluster (coding `541=1`) stock offers two cluster views, MAP and
KDK/arrows. CarPlay already replaces the arrows picture ([most-cluster](most-cluster.md)).
This branch drives the MAP view through the same engine, so that the phone's own cluster map,
decoded by the head unit's hardware decoder, replaces the Audi map during CarPlay route
guidance. It changes no vehicle coding and does not capture the MMI screen.

## One engine, two views

`MostPresentation` holds one `View` per stock picture it replaces; the renderer handshake,
extents queries, composition, stock logical-context reporting, restoration, window-token
watching and the stream-rate rule are shared.

| | Arrows view | MAP view |
| --- | --- | --- |
| Stock context → CarPlay context | `73` → `81` = `{98}` | `72` → `82` = `{99}` |
| Size from | stock KDK `20` (800 × 252) | stock map `33` (800 × 298, measured) |
| Unknown size | KVS_Most 800 × 252 | wait: never composed, re-queried |
| Window between uses | kept (sized at connect) | removed when released |
| Unconfirmed after 7 s | composed once, for diagnosis | never composed |
| Requested by | CarPlay route guidance | the same guidance, while the phone's cluster stream is live |
| Renderer window | GL scene | NV12 video window the CPU writes |
| Stream rate (stock's 10) | 30 | 15 |

Stock's listener is always told its own logical context (`73`/`72`), including
buffered-switch replays. Alternate map `76` (source `58`) is left stock. Source `33` reports
`-1 × -1` until the stock map is ready (~14 s after start, map02), so the MAP view waits for it.

`maneuver_render` presents both windows. Window `99` follows its own request file with the
same record format as `98` (protocol.h `CR_MOST_MAP_OUTPUT_PATH`): it appears for a request, is
resized or recreated with a new token, and is destroyed when the request is withdrawn. Nothing
is posted before the decoder's first picture, so the window is not reported ready - and the
view not composed - until there is one; a window keeps its last picture until the next.

## The phone's cluster display

The phone renders the cluster map as its own CarPlay display (Apple Maps' instrument-cluster
view) and streams it as AirPlay type 111 beside the ordinary type-110 screen. Stock libairplay
has no type-111 support; the hook adds it through its AirPlay seams
(`framework/airplay_seams.c`, [integration-seam](../hook/integration-seam.md)) and the
`altscreen` module. The MU1329 receiver in `Build/Analysis/FullMapCarplay` and the open-source
LIVI receiver are the protocol references; none of their code is used.

- **`/info`**: `displays` gains one entry - its own UUID, type 111, 800 × 298, `maxFPS` 60
  (at 30 the phone sent about 20 a second, map22; at 60 a steady 30, map23), 5 px/mm,
  `features` 2 and `primaryInputDevice` 3 (knob-driven, as the reference),
  `initialURL` `maps:/car/instrumentcluster/map`, one full-size view area whose safe area is
  x 150-650, y 0-278: between the dials, symmetric so a centred map stays centred (confirmed
  in the car, map28). Top-level
  `features` gains bit 26. An extended `/info` that cannot be serialized is dropped (map04).
- **SETUP**: every successful reply gains `enabledFeatures` `["viewAreas", "altScreen"]`, and a
  request for the cluster stream gains `{type: 111, dataPort}` in the reply; without either the
  phone tears the session down (map05, map06). A request mixing the cluster stream with others
  reaches stock without it.
- **Commands** (stock `AirPlayReceiverSessionSendCommand`, from their own thread):
  - a second after that reply, `showUI {uuid, url: maps:/car/instrumentcluster/map}` - the map
    alone; `maps:/car/instrumentcluster` adds a guidance card that pushes the map aside
    (map19) - and `forceKeyFrame {uuid}`; `forceKeyFrame` again whenever the renderer has no
    decodable point.
  - **Zoom**: the steering-wheel roller zooms the phone's map while the MAP view shows it
    ([steering-wheel](../input/steering-wheel.md)). Java's `CMD_ALT_ZOOM` (signed MapScale
    steps, positive out) becomes one `changeMapZoomLevel {uuid, zoomDirection}` per step, 0 in
    and 1 out, at most 8 a report.
  - **Night mode**: stock passes the MMI's night mode only for the whole session
    (`setNightMode {nightMode}` without a uuid), which the cluster map does not follow (map23).
    Java reports the MMI's value at CarPlay start, on every change and whenever the stream goes
    live (`CMD_ALT_APPEARANCE`); the hook sends `setNightMode {uuid, nightMode}` for the cluster
    display (yuedizhibo/MHI2Q-CarPlay-AltScreen issue 25). The phone ignores a value it already
    holds (map24), so after `showUI` the opposite value goes first and the MMI's 1 s later.
- **Receiver**: a listener in `dio_manager` on the first free of TCP 7100, 7000, 7001, 6200,
  6100, 6001, 6000, 5010. `lsm-pf` passes TCP into `carplay0` only on those and stock's own
  ports (map07-map10). One connection at a time, the newest.
- **Decryption**: stock's screen framing (map11) - a 128-byte header (payload size, type in
  byte 4), then the payload; type 1 is the avcC record, type 0 a frame of length-prefixed NAL
  units, AES-128-CTR with one keystream per connection. Key and IV come from stock's
  `AirPlay_DeriveAESKeySHA512ForScreen(masterKey, 16, streamConnectionID)`; the master key
  reaches the module through the `SetSecurityInfo` seam. Frames that do not parse as H.264
  after decryption are dropped.
- **Session reference**: the module holds the AirPlay session (for its commands) only while
  its stream can use it and releases it when stock tears the session down, the stream ends, a
  new phone session starts, a newer SETUP replaces it or no connection comes within 5 s; never
  under a lock of its own, never on stock's thread ([session-lifecycle](../deploy/session-lifecycle.md) R5).

## Decoding

The stream is decoded by the Qualcomm hardware decoder inside `maneuver_render` - never in
`dio_manager`, so a decoder fault costs the cluster picture, never the CarPlay session.

- **Hand-off**: the hook writes every access unit as Annex B into a shared-memory frame ring
  (`common/cluster_video_ring.h`, 1 MiB) and never waits. Bus `EVT_CLUSTER_VIDEO` (sticky) says
  when the stream is live; `ClusterVideo` passes that to `MostPresentation`, whose one rule
  requests the arrows during CarPlay guidance and the MAP view while the stream is also live.
- **Reader** (`cluster_video.c`): while window `99` wants pictures, a thread follows the ring
  from the stream's newest config and key frame - again after any unit the decoder missed -
  validating each record after copying it, and feeds the decoder. A renderer joining late
  never replays a backlog (map15). It closes the decoder when the window goes or the stream
  stops for 2 s.
- **Decoder** (`cluster_decoder.c`): stock's own recipe, from its `dio::COMXVideoDecoder` in
  `libairplay.so` - the OpenMAX core `libOmxCore.so` (loaded at run time),
  `OMX.qcom.video.decoder.avc` in decode-order mode
  (`OMX.QCOM.index.config.video.DisplayPictureBuffer`), the input port as AVC at 800 × 298 and
  30 fps, the parameter sets as a `CODECCONFIG` buffer and every frame an `ENDOFFRAME` buffer,
  output `0x7F000004`. It decodes into buffers it allocates, next to stock's own CarPlay screen
  decoder without conflict (map17-map19). Its close line logs the phone's rate, longest pause
  and the pictures shown.
- **TILE_4x2** is the only output this decoder writes. `MMIVdecController::SetColorFormat`
  (`libmmiVdec_asic_AF.so`) maps OMX `39`, `0x7F000004`, `0x7F000003` and `0x7F000001` to
  Qualcomm VCD's NV12, TILE_4x2, NV12_16M2KA and TILE_1x1; the component lists only
  `0x7F000004` (map17) and its driver refuses NV12 (map18: "Failed to set output format 1 with
  error 3"). The layout: 64 × 32-byte tiles of 2 KB, tile columns rounded up to even, each pair
  of tile rows stored as T0 T1, B0-B3, T2-T5, B4-B7 ... (top and bottom row), an unpaired last
  row straight, chroma after luma rounded up to 8 KB. 800 × 298 takes 430080 bytes, in
  434224-byte buffers (map19).
- **Showing a picture** (`map_layer.c`): window `99` belongs to its own map thread, never the
  render loop, whose arrow scene can take 400 ms a frame (map22). The window has its own Screen
  context and no EGL, so the thread creates, probes and recreates it alone
  (`platform_map_check`). Each decoded picture wakes the thread through a pipe, and the thread
  untiles it into the window's buffer outside the decoder's lock and posts it at once (~1 ms,
  map27); the decoder keeps that picture until the copy ends. Every 30 s it logs posts, rate,
  copy and post times and the longest gap.
  - **Colour**: the phone's pictures are video range and the encoder's compositor reads an NV12
    window as full range, so the untiling expands both planes to 0-255 (map27 looked dull,
    map28 right).
  - **Format**: NV12. RGBA, converted by the CPU (~6 ms a picture), was no smoother and tore
    more (map25-map27).
  - **Buffers**: four, and the CPU write has no fence: each picture goes into the buffer
    least recently posted, never one the encoder's compositor may still be reading (two tore,
    map23; with three, Screen offered only the last two posted about 2 of every 31 pictures,
    map27).
- **Stream rate**: stock's full cluster rate (10) is sent as 15 while the phone's map is
  composed and 30 for the arrows alone (`MostPresentation.substituteRate`); stock's 1 (the
  cluster's KOMO data rate 1) passes. At 30 the cluster kept dropping its data rate to 1 on
  the map view - stock then streams 1 fps - and once its graphics altogether (map24, map25);
  60 showed about a picture a second (map24). At 15 the map is close to stock's smoothness
  (map26).

Still open: the last of the tearing (rarer with four buffers, map28). No stall timer is
planned: the view goes when guidance ends or the stream's connection closes, and a phone that
keeps the connection open but sends nothing (a still map) cannot be told from a stalled one,
so the window keeps its last picture. The URL stays `maps:/car/instrumentcluster/map`, without a `maneuverLayout` parameter. No setting
is planned: the Audi map is only usable while CarPlay navigation is not running, and the MAP
view only replaces it while it is.

## Run notes

Observations the sections above do not already state; the raw logs are kept per run.

- **map14-map15**: the phone streams the cluster display for the whole session, so the MAP view
  follows CarPlay guidance. A pulled cable never closes the stream's connection
  ([session-lifecycle](../deploy/session-lifecycle.md) R5).
- **map19**: about 630 pictures a session, none dropped.
- **map21**: Apple Maps centres its position marker; Google Maps draws no guidance card and,
  without a safe area, put its marker behind the left dial. Waze sends no route guidance
  ([rgd-activation](../rgd/rgd-activation.md)), so neither view is taken.
- **map22**: with the safe area Google Maps centres its marker too, but still labels the street
  under it (its own drawing). Apple Maps kept one display north-up and zoomed out while the
  other followed.
- **map23-map24**: two start-up faults with guidance active before the Audi navigation was up -
  the start burst's reset frame and the navigator's late no-route
  ([rgd-activation](../rgd/rgd-activation.md)) - fixed and confirmed.

## Validation boundary

Host tests cover the shared engine for both views against the shipping display manager and
stock core; the renderer's windows against fake Screen/EGL (window `99` as NV12: format,
planes, the buffer written, reported after its first frame, resize, post-failure recreation)
and the map thread's posting against a fake decoder; the altscreen module against fake
CoreFoundation-lite, stock commands, key derivation and AES-CTR, and bus, with its receiver on
real sockets; the decoder client against a fake OpenMAX core and component (port set-up,
buffer flags, TILE_4x2 untiling against the layout's literal tile order and stock's buffer
size, the full-range expansion, buffer recycling, close, set-up failures, decode errors,
output changes, the wake on a new picture, a picture held from the decoder while it is
copied, a close waiting for that copy) and the ring reader's feeding of it; the TearDown seam
against a fake of stock's in a shared library; and the frame ring (order, late joining,
overrun, wrap, oversize, new streams and writers, a writer racing a slow reader with no torn
record). The decryption and the decoder recipe are RE of the car's libraries, confirmed by
the car (map13, map19); the guidance-and-stream rule is tested against the stock jar
(`MostMapViewTest`).
