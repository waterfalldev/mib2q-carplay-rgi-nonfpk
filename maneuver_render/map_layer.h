/*
 * The MOST MAP view window's content (window 99, an NV12 video window, platform_map_*): the
 * phone's cluster map as the decoder delivers it (cluster_decoder.h).  Nothing is posted before
 * the first picture, so the window is never reported ready - and the view never composed -
 * without one; a window keeps its last picture until the next.
 */
#ifndef CR_MAP_LAYER_H
#define CR_MAP_LAYER_H

/* Once per render-loop iteration. */
void map_layer_tick(void);

#endif
