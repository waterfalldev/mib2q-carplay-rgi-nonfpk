/*
 * The MAP view's NV12 window content: host test of the real map_layer.c against a fake
 * platform (one padded NV12 buffer, posts recorded) and a fake decoder.  Checks: the window
 * wants pictures while it exists; nothing is posted before the first picture; each new picture
 * is copied and posted at once; a window keeps its last picture; a new window gets the newest
 * at once; a picture that cannot be copied is not posted.
 */
#define PLATFORM_QNX 1

#include <stdint.h>
#include <string.h>

#include "map_layer.c"              /* -I <renderer tree>/maneuver_render */
#include "check.h"

#define W 800
#define H 298
#define PAD 24

static unsigned char g_y[H][W + PAD], g_uv[H / 2][W + PAD];
static unsigned g_fake_window = 7, g_decoder_serial;
static int g_have_window = 1, g_posts, g_wanted = -1, g_copy_ok = 1;

int platform_map_window(int *w, int *h, unsigned *window) {
    *w = W;
    *h = H;
    *window = g_fake_window;
    return g_have_window;
}
int platform_map_nv12_buffer(unsigned char **y, int *y_stride, unsigned char **uv, int *uv_stride) {
    *y = &g_y[0][0];
    *y_stride = W + PAD;
    *uv = &g_uv[0][0];
    *uv_stride = W + PAD;
    return 1;
}
int platform_map_nv12_post(void) { ++g_posts; return 1; }
void cluster_video_want(int wanted) { g_wanted = wanted; }
int cluster_decoder_fresh(unsigned serial) { return g_decoder_serial && g_decoder_serial != serial; }
int cluster_decoder_copy(unsigned *serial, uint8_t *y, int y_stride, uint8_t *uv, int uv_stride, int w, int h) {
    int row;
    if (!g_decoder_serial || *serial == g_decoder_serial) return 0;
    *serial = g_decoder_serial;
    if (!g_copy_ok) return 0;
    for (row = 0; row < h; ++row) memset(y + (size_t)row * y_stride, 0x40 + (int)g_decoder_serial, (size_t)w);
    for (row = 0; row < h / 2; ++row) memset(uv + (size_t)row * uv_stride, 0x80, (size_t)w);
    return 1;
}

static int picture_is(unsigned serial) {
    int x, y, ok = 1;
    for (y = 0; y < H; ++y) {
        for (x = 0; x < W; ++x) ok &= g_y[y][x] == 0x40 + serial && g_uv[y / 2][x] == 0x80;
        for (x = W; x < W + PAD; ++x) ok &= g_y[y][x] == 0xcc && g_uv[y / 2][x] == 0xcc;
    }
    return ok;
}

int main(void) {
    memset(g_y, 0xcc, sizeof(g_y));
    memset(g_uv, 0xcc, sizeof(g_uv));

    g_have_window = 0;
    map_layer_tick();
    check(g_wanted == 0 && g_posts == 0, "no map window: no pictures wanted");
    g_have_window = 1;
    map_layer_tick();
    map_layer_tick();
    check(g_wanted == 1 && g_posts == 0, "a map window wants pictures; nothing is posted before the first");

    g_decoder_serial = 1;
    map_layer_tick();
    check(g_posts == 1 && picture_is(1), "the first picture is copied and posted at once, row padding untouched");
    g_decoder_serial = 2;
    map_layer_tick();
    check(g_posts == 2 && picture_is(2), "and the next");
    map_layer_tick();
    map_layer_tick();
    check(g_posts == 2 && picture_is(2), "no new picture: nothing posted, the window keeps the last");

    g_fake_window = 8;
    memset(g_y, 0xcc, sizeof(g_y));
    memset(g_uv, 0xcc, sizeof(g_uv));
    map_layer_tick();
    check(g_posts == 3 && picture_is(2), "a new window gets the newest picture at once");

    g_copy_ok = 0;
    g_decoder_serial = 3;
    map_layer_tick();
    check(g_posts == 3, "a picture that cannot be copied is not posted");
    printf("map_layer_nv12_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
