/*
 * The MAP view's NV12 window content: host test of the real map_layer.c against a fake
 * platform (one padded NV12 buffer, posts recorded) and a fake decoder.  Checks: the window
 * wants pictures while it exists; nothing is posted before the first picture; each new picture
 * is copied and posted at once; a window keeps its last picture; a new window gets the newest
 * at once; a picture that cannot be copied is not posted.  The loop's sleep posts each picture
 * as it arrives, sleeps its full time without one, and does not spin on a picture it cannot show.
 * The map thread follows the window request, posts pictures on its own, and stops on request.
 */
#define PLATFORM_QNX 1
#define MAP_CHECK_MS 20

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "map_layer.c"              /* -I <renderer tree>/maneuver_render */
#include "check.h"

#define W 800
#define H 298
#define PAD 24

static unsigned char g_y[H][W + PAD], g_uv[H / 2][W + PAD];
static unsigned g_fake_window = 7, g_decoder_serial;
static int g_have_window = 1, g_posts, g_wanted = -1, g_copy_ok = 1, g_buffer_ok = 1;
static int g_waits, g_arrive_on_wait;

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
    return g_buffer_ok;
}
int platform_map_nv12_post(void) { ++g_posts; return 1; }
static int g_map_checks, g_map_recovers;
void platform_map_check(int recover) { if (recover) ++g_map_recovers; else ++g_map_checks; }
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

/* Each of the next g_arrive_on_wait waits ends with a new picture; the rest sleep their time. */
int cluster_decoder_wait(unsigned serial, int64_t ns) {
    struct timespec t = { (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
    ++g_waits;
    if (g_arrive_on_wait > 0) {
        --g_arrive_on_wait;
        ++g_decoder_serial;
        return 1;
    }
    if (cluster_decoder_fresh(serial)) return 1;
    nanosleep(&t, NULL);
    return 0;
}

static int64_t ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)(now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
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

    {
        struct timespec start;
        g_copy_ok = 1;
        map_layer_tick();
        g_posts = g_waits = 0;
        g_arrive_on_wait = 3;
        clock_gettime(CLOCK_MONOTONIC, &start);
        map_layer_sleep(30 * 1000000LL);
        check(g_posts == 3 && g_waits == 4 && picture_is(g_decoder_serial) && ms_since(&start) >= 25,
              "the loop's sleep posts each of 3 pictures as it arrives, then sleeps out its 30 ms (%d)",
              (int)ms_since(&start));

        g_posts = g_waits = 0;
        g_buffer_ok = 0;
        ++g_decoder_serial;
        clock_gettime(CLOCK_MONOTONIC, &start);
        map_layer_sleep(30 * 1000000LL);
        check(g_posts == 0 && g_waits == 1 && ms_since(&start) >= 25,
              "a picture without a buffer to show it in: no spin, the sleep is plain (%d waits)", g_waits);
        g_buffer_ok = 1;

        g_have_window = 0;
        map_layer_tick();
        g_waits = 0;
        g_arrive_on_wait = 1;
        clock_gettime(CLOCK_MONOTONIC, &start);
        map_layer_sleep(30 * 1000000LL);
        check(g_waits == 0 && g_posts == 0 && ms_since(&start) >= 25, "no map window: a plain sleep");
    }

    {
        struct timespec pause = { 0, 150 * 1000000L };
        g_have_window = 1;
        map_layer_tick();                         /* the picture left from the no-buffer case */
        g_posts = g_waits = g_map_checks = 0;
        g_arrive_on_wait = 2;
        map_layer_start();
        nanosleep(&pause, NULL);
        map_layer_stop();
        check(g_map_checks >= 3 && g_posts == 2 && picture_is(g_decoder_serial) && g_wanted == 0,
              "map thread: follows the request (%d checks), posts both pictures itself, stops and "
              "wants no more", g_map_checks);
    }
    printf("map_layer_nv12_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
