/*
 * The MOST MAP view window's content (window 99, an NV12 video window, platform_map_*): the
 * phone's cluster map as the decoder delivers it (cluster_decoder.h).  Nothing is posted before
 * the first picture, so the window is never reported ready - and the view never composed -
 * without one; a window keeps its last picture until the next.
 */
#ifndef CR_MAP_LAYER_H
#define CR_MAP_LAYER_H

#include <stdint.h>

#ifndef MAP_CHECK_MS
#define MAP_CHECK_MS   250      /* follow Java's window request this often */
#endif
#define MAP_RECOVER_MS 5000     /* and probe the window for loss */

/* The map thread: owns window 99 (platform_map_check) and posts each picture as it arrives,
 * apart from the render loop, whose scene frames can take hundreds of milliseconds (map22).
 * map_layer_stop() ends it and waits, within about MAP_CHECK_MS. */
void map_layer_start(void);
void map_layer_stop(void);

/* One pass: the newest picture into the window, if there is a newer one. */
void map_layer_tick(void);

/* Sleeps `ns`; while the map window is up each new picture is copied and posted as it
 * arrives. */
void map_layer_sleep(int64_t ns);

#endif
