/*
 * altScreen - the phone's second CarPlay display for the MOST cluster's MAP view.
 *
 * /info advertises a second display (its own UUID, type 111, the MOST map's size,
 * maps:/car/instrumentcluster/map) and every successful SETUP reply enables "viewAreas" and
 * "altScreen" (map05: without them the phone tears the session down).  Stock has no type-111
 * support: a SETUP mixing the cluster stream with others reaches stock without it, and the
 * reply gains the stream with this module's receiver port, then showUI and forceKeyFrame.
 * The receiver decrypts the video as stock does its own screen's (stock's key derivation and
 * AES-128-CTR) and hands every access unit to the renderer through the frame ring
 * (common/cluster_video_ring.h), never waiting for it.  The bus says when the stream is live
 * (EVT_CLUSTER_VIDEO); the renderer's key-frame requests become forceKeyFrame, Java's
 * steering-wheel zoom (CMD_ALT_ZOOM) changeMapZoomLevel and the MMI's night mode
 * (CMD_ALT_APPEARANCE) setNightMode, all for the cluster display.
 */
#ifndef ALTSCREEN_HOOK_H
#define ALTSCREEN_HOOK_H

#include "../framework/hook_framework.h"

#define ALT_PACKET_MAX          (2 * 1024 * 1024)          /* largest payload accepted (map11: 43 KB) */
/* TCP ports lsm-pf lets in on carplay0 that stock never binds (it binds 5000, 5001, 6030):
 * map07-map10 offered others and the phone never got through. */
#define ALT_STREAM_PORTS        { 7100, 7000, 7001, 6200, 6100, 6001, 6000, 5010 }
#ifndef ALT_NIGHT_TOGGLE_MS
#define ALT_NIGHT_TOGGLE_MS     1000                       /* after showUI: the opposite night mode, then the MMI's */
#endif
#ifndef ALT_SHOW_UI_DELAY_MS
#define ALT_SHOW_UI_DELAY_MS    1000                       /* showUI after the stream's SETUP reply */
#endif
#define ALT_POLL_MS             250                        /* receiver: key-frame requests, session end */
#define ALT_KEYFRAME_MIN_MS     1000                       /* at most one forceKeyFrame this often */
#ifndef ALT_HOLD_CONNECT_MS
#define ALT_HOLD_CONNECT_MS     5000                       /* a session with no stream connection is let go */
#endif
#define ALT_STREAM_TYPE         111                        /* AirPlay: alternate screen video */
#define ALT_DISPLAY_UUID        "e0f3b2a4-7c51-4d2e-9b6a-3f1c5d8a2b90"
#define ALT_DISPLAY_URL         "maps:/car/instrumentcluster/map"   /* the map alone: no guidance card */
#define ALT_WIDTH               800                        /* stock MAP source 33, measured (map02) */
#define ALT_HEIGHT              298
/* The part the driver sees: the dials cover ~150 px each side (map21 photo and a stock-map
 * photo).  Symmetric, so a map centred in it stays centred on the display.  Only 20 px off
 * the bottom: 50 px left the map visibly short there (map22). */
#define ALT_SAFE_X              150
#define ALT_SAFE_Y              0
#define ALT_SAFE_WIDTH          500
#define ALT_SAFE_HEIGHT         278
/* The reference asks for 60.  At 30 the phone sent ~20 a second (map22), at 60 a steady 30
 * (map23); the MOST stream takes the newest at 15 (MostPresentation.CARPLAY_MAP_RATE). */
#define ALT_MAX_FPS             60
#define ALT_PIXELS_PER_MM       5                          /* physical size estimate */
#define ALT_DISPLAY_FEATURES    2                          /* knob-driven, as the reference */
#define ALT_PRIMARY_INPUT       3                          /* knob, as the reference */
#define ALT_FEATURES_BIT        (1LL << 26)                /* /info features, as the reference sets */
#define ALT_ZOOM_MAX_STEPS      8                          /* changeMapZoomLevel per wheel report, at most */

extern const hook_module_def_t altscreen_module_def;

#endif
