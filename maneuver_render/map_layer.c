/* The MOST MAP view window's content (map_layer.h). */
#include "map_layer.h"
#include "cluster_decoder.h"
#include "cluster_video.h"
#include "platform.h"

#include <stdio.h>

static unsigned g_window, g_serial;

void map_layer_tick(void) {
    unsigned char *y, *uv;
    int w, h, y_stride, uv_stride, have;
    unsigned window;
    have = platform_map_window(&w, &h, &window);
    cluster_video_want(have);
    if (!have) return;
    if (window != g_window) g_serial = 0;           /* a new window gets the newest picture at once */
    if (!cluster_decoder_fresh(g_serial) || !platform_map_nv12_buffer(&y, &y_stride, &uv, &uv_stride)
            || !cluster_decoder_copy(&g_serial, y, y_stride, uv, uv_stride, w, h) || !platform_map_nv12_post())
        return;
    if (window != g_window) fprintf(stderr, "map_layer: the phone's cluster map on map window %u\n", window);
    g_window = window;
}
