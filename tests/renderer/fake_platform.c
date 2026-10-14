/*
 * Stand-in for platform_macos.c / platform_qnx.c and for the threads main.c starts
 * (map_layer.c, cluster_video.c).  main.c is #included by the parity test and by the
 * fingerprint harness; neither calls the entry point, so these only have to link.
 */
#include <stddef.h>
#include "platform.h"
#include "map_layer.h"
#include "cluster_video.h"

int platform_init(int w, int h) { (void)w; (void)h; return 0; }
int platform_swap(void) { return 1; }
void platform_poll(void) {}
int platform_should_close(void) { return 1; }
void platform_shutdown(void) {}
void platform_get_framebuffer_size(int *w, int *h) { *w = 328; *h = 181; }
void platform_get_routing_ids(int *a, int *b, int *c) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; }
void platform_check_and_recover_window(void) {}
void platform_check_output(void) {}
unsigned platform_get_output(int *ww, int *wh, int *x, int *y, int *w, int *h, int *opaque) {
    *ww = 328; *wh = 181; *x = 0; *y = 0; *w = 328; *h = 181; *opaque = 0;
    return 0;
}
int platform_map_window(int *w, int *h, unsigned *window) { (void)w; (void)h; (void)window; return 0; }
int platform_map_nv12_buffer(unsigned char **y, int *ys, unsigned char **uv, int *uvs) {
    (void)y; (void)ys; (void)uv; (void)uvs; return 0;
}
int platform_map_nv12_post(void) { return 0; }
void platform_map_check(int recover) { (void)recover; }
void platform_release_displayable(void) {}
long platform_free_memory_kb(void) { return -1; }
int platform_key_tap(int key) { (void)key; return 0; }

void map_layer_start(void) {}
void map_layer_stop(void) {}
void map_layer_tick(void) {}
void map_layer_sleep(int64_t ns) { (void)ns; }
void cluster_video_start(void) {}
void cluster_video_stop(void) {}
void cluster_video_want(int wanted) { (void)wanted; }
