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
watching and the 30 fps rate rule are shared.

| | Arrows view | MAP view |
| --- | --- | --- |
| Stock context → CarPlay context | `73` → `81` = `{98}` | `72` → `82` = `{99}` |
| Size from | stock KDK `20` (800 × 252) | stock map `33` (800 × 298, measured) |
| Unknown size | KVS_Most 800 × 252 | wait: never composed, re-queried |
| Window between uses | kept (sized at connect) | removed when released |
| Unconfirmed after 7 s | composed once, for diagnosis | never composed |
| Requested by | CarPlay route guidance | the same guidance, while the phone's cluster stream is live |
| Renderer window | GL scene | NV12 video window the CPU writes |

Stock's listener is always told its own logical context (`73`/`72`), including
buffered-switch replays. Alternate map `76` (source `58`) is left stock. While any CarPlay
picture is composed, stock's full cluster rate is sent as 30 fps in every view.

`maneuver_render` presents both windows. Window `99` follows its own request file with the
same record format as `98` (protocol.h `CR_MOST_MAP_OUTPUT_PATH`): it appears for a request, is
resized or recreated with a new token, and is destroyed when the request is withdrawn. It is
an NV12 window (`SCREEN_FORMAT_NV12`, usage read/write, no EGL) showing the decoder's newest
picture. Nothing is posted before the first, so the window is not reported ready - and the
view not composed - until there is one; a window keeps its last picture until the next.

## The phone's cluster display

The phone renders the cluster map as its own CarPlay display (Apple Maps' instrument-cluster
view) and streams it as AirPlay type 111 beside the ordinary type-110 screen. Stock libairplay
has no type-111 support; the hook adds it through its AirPlay seams
(`framework/airplay_seams.c`) and the `altscreen` module. The MU1329 receiver in
`Build/Analysis/FullMapCarplay` and the open-source LIVI receiver are the protocol references;
none of their code is used.

- **`/info`**: `displays` gains one entry - its own UUID, type 111, 800 × 298, `maxFPS` 30,
  5 px/mm, `features` 2 and `primaryInputDevice` 3 (knob-driven, as the reference),
  `initialURL` `maps:/car/instrumentcluster/map`, one full-size view area. Top-level
  `features` gains bit 26. An extended `/info` that cannot be serialized is dropped (map04).
- **SETUP**: every successful reply gains `enabledFeatures` `["viewAreas", "altScreen"]`
  (map05: without them the phone tears the session down). A request mixing the cluster stream
  with others reaches stock without it; the reply gains `{type: 111, dataPort}` (map06: without
  it the phone tears the session down).
- **Commands** (stock `AirPlayReceiverSessionSendCommand`, from their own thread): a second
  after that reply `showUI {uuid, url: maps:/car/instrumentcluster/map}` - the map alone, as
  LIVI asks for it; `maps:/car/instrumentcluster` adds a guidance card that pushes the map
  aside (map19) - and `forceKeyFrame {uuid}`; `forceKeyFrame` again whenever the renderer has
  no decodable point.
- **Receiver**: a listener in `dio_manager` on the first free of TCP 7100, 7000, 7001, 6200,
  6100, 6001, 6000, 5010. `lsm-pf` passes TCP into `carplay0` only on those and stock's own
  ports (map07-map10 offered others and the phone never got through). One connection at a
  time, the newest.
- **Decryption**: stock's screen framing (map11) - a 128-byte header (payload size, type in
  byte 4), then the payload; type 1 is the avcC record, type 0 a frame of length-prefixed NAL
  units, AES-128-CTR with one keystream per connection. Key and IV come from stock's
  `AirPlay_DeriveAESKeySHA512ForScreen(masterKey, 16, streamConnectionID)`; the master key
  reaches the module through the `SetSecurityInfo` seam. Frames that do not parse as H.264
  after decryption are dropped.
- **Session reference**: the module holds the AirPlay session (for `forceKeyFrame`) only while
  its stream can use it and releases it when stock tears the session down
  (`AirPlayReceiverSessionTearDown` seam), the stream ends, a new phone session starts, a newer
  SETUP replaces it or no connection comes within 5 s; never under a lock of its own, never on
  stock's thread ([session-lifecycle](../deploy/session-lifecycle.md) R5).

## Decoding

The stream is decoded by the Qualcomm hardware decoder inside `maneuver_render` - never in
`dio_manager`, so a decoder fault costs the cluster picture, never the CarPlay session.

- **Hand-off**: the hook writes every access unit as Annex B into a shared-memory frame ring
  (`common/cluster_video_ring.h`, 1 MiB) and never waits. Bus `EVT_CLUSTER_VIDEO` (sticky) says
  when the stream is live; `ClusterVideo` passes that to `MostPresentation`, whose one rule
  requests the arrows during CarPlay guidance and the MAP view while the stream is also live.
- **Reader** (`maneuver_render/cluster_video.c`): while window `99` is up, a thread follows the
  ring from the stream's newest config and key frame - again after any unit the decoder
  missed - validating each record after copying it, and feeds the decoder. It closes the
  decoder when the window goes or the stream stops for 2 s.
- **Decoder** (`maneuver_render/cluster_decoder.c`): stock's own recipe, from its
  `dio::COMXVideoDecoder` in `libairplay.so` - the OpenMAX core `libOmxCore.so` (loaded at run
  time), `OMX.qcom.video.decoder.avc` in decode-order mode
  (`OMX.QCOM.index.config.video.DisplayPictureBuffer`), the input port as AVC at 800 × 298 and
  30 fps, the parameter sets as a `CODECCONFIG` buffer and every frame an `ENDOFFRAME` buffer,
  output `0x7F000004`. Stock decodes into its Screen window's own buffers (Screen format
  `0x1000C`); ours decodes into buffers the decoder allocates and the render thread copies the
  newest picture into window `99`, so the window keeps its own life.
- **TILE_4x2** is the only output this decoder writes. `MMIVdecController::SetColorFormat`
  (`libmmiVdec_asic_AF.so`) maps OMX `39`, `0x7F000004`, `0x7F000003` and `0x7F000001` to
  Qualcomm VCD's NV12, TILE_4x2, NV12_16M2KA and TILE_1x1; the component lists only
  `0x7F000004` (map17) and its driver refuses NV12 (map18: "Failed to set output format 1 with
  error 3"). The layout: 64 × 32-byte tiles of 2 KB, tile columns rounded up to even, each pair
  of tile rows stored as T0 T1, B0-B3, T2-T5, B4-B7 ... (top and bottom row), an unpaired last
  row straight, chroma after luma rounded up to 8 KB. 1024 × 480 takes 753664 bytes, within
  stock's 753712-byte buffers (map17); 800 × 298 takes 430080, in 434224-byte buffers (map19).
- **Beside stock**: ours is a second hardware decoder session next to stock's CarPlay screen
  decoder; no conflict was seen (map17-map19).

Still open: stall handling, roller zoom (`changeMapZoomLevel`), the cluster layout
(`maneuverLayout`), the safe area - the cluster draws its dials, distance box and street label
over the picture's sides and bottom - and a setting.

## Findings

- **map02**: source `33` reports `-1 × -1` until the stock map is ready (~14 s), then
  800 × 298 in context `72`; context `82` registers; during guidance MAP streams at 30 fps.
- **map05**: window `99` composed as `82` replaces the Audi map, crop and orientation right.
- **map04-map11**: `/info` serialization, `enabledFeatures`, the reply's data port, showUI and
  a filter-allowed port brought the phone to connect and stream (H.264 High, 800 × 298).
- **map12-map13**: the stream decrypts to H.264 from its first frame.
- **map14**: the MOST capture shows the NV12 window, colours right. The phone streams the
  cluster display for the whole session, so the MAP view follows CarPlay guidance. A pulled
  cable left the session held and SI killed `dio_manager` (`TIMEOUT_SHUTDOWN`).
- **map15**: after a cable pull the hook released the session 0.3 s later and CarPlay stopped
  cleanly in about 1 s. A renderer joining 2 s late replayed that backlog - now it starts at
  the newest key frame.
- **map16-map18**: the stock decoder libraries, then the decoder's output formats (above).
- **map19**: the phone's map in the MAP view, about 630 pictures a session, none dropped. With
  `maps:/car/instrumentcluster` the phone added a guidance card at the top right and moved the
  position marker left; hence `/map`.

## Validation boundary

Host tests cover the shared engine for both views against the shipping display manager and
stock core; the renderer's windows against fake Screen/EGL (window `99` as NV12: format,
planes, reported after its first frame, resize, post-failure recreation) and the map window's
content against a fake decoder; the altscreen module against fake CoreFoundation-lite, stock
commands, key derivation and AES-CTR, and bus, with its receiver on real sockets; the decoder
client against a fake OpenMAX core and component (port set-up, buffer flags, TILE_4x2 untiling
against the layout's literal tile order and stock's buffer size, buffer recycling, close,
set-up failures, decode errors, output changes) and the ring reader's feeding of it; the
TearDown seam against a fake of stock's in a shared library; and the frame ring (order, late
joining, overrun, wrap, oversize, new streams and writers, a writer racing a slow reader with
no torn record). The decryption and the decoder recipe are RE of the car's libraries,
confirmed by the car (map13, map19); the guidance-and-stream rule is tested against the stock
jar (`MostMapViewTest`).
