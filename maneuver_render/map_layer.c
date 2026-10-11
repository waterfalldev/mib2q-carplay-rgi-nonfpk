/* The MOST MAP view window's content (map_layer.h). */
#include "map_layer.h"
#include "cluster_decoder.h"
#include "cluster_video.h"
#include "platform.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef MAP_STATS_S
#define MAP_STATS_S 30          /* the posting log line, this often */
#endif

static unsigned g_window, g_serial;
static int g_have;

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* Every MAP_STATS_S: posts, their rate, the copy and the post itself, and the longest gap
 * between two posts - whether pictures reach the window evenly (map22-map26). */
static struct {
    int64_t since, last_post;
    unsigned posts;
    int64_t copy_total, post_total, copy_max, post_max, gap_max;
} g_stats;

static void count_post(int64_t start, int64_t copied, int64_t posted) {
    if (!g_stats.since) g_stats.since = start;
    if (g_stats.last_post && posted - g_stats.last_post > g_stats.gap_max) g_stats.gap_max = posted - g_stats.last_post;
    g_stats.last_post = posted;
    ++g_stats.posts;
    g_stats.copy_total += copied - start;
    g_stats.post_total += posted - copied;
    if (copied - start > g_stats.copy_max) g_stats.copy_max = copied - start;
    if (posted - copied > g_stats.post_max) g_stats.post_max = posted - copied;
    if (posted - g_stats.since >= MAP_STATS_S * 1000000000LL) {
        int64_t span_ms = (posted - g_stats.since) / 1000000;
        fprintf(stderr, "map_layer: %u posts in %lld ms (%lld.%lld/s); copy avg %lld max %lld us, "
                "post avg %lld max %lld us, longest gap %lld ms\n", g_stats.posts, (long long)span_ms,
                (long long)(g_stats.posts * 1000LL / span_ms), (long long)(g_stats.posts * 10000LL / span_ms % 10),
                (long long)(g_stats.copy_total / g_stats.posts / 1000), (long long)(g_stats.copy_max / 1000),
                (long long)(g_stats.post_total / g_stats.posts / 1000), (long long)(g_stats.post_max / 1000),
                (long long)(g_stats.gap_max / 1000000));
        memset(&g_stats, 0, sizeof(g_stats));
    }
}

void map_layer_tick(void) {
    unsigned char *y, *uv;
    int w, h, y_stride, uv_stride, have;
    unsigned window;
    int64_t start, copied;
    have = g_have = platform_map_window(&w, &h, &window);
    cluster_video_want(have);
    if (!have) return;
    if (window != g_window) g_serial = 0;           /* a new window gets the newest picture at once */
    if (!cluster_decoder_fresh(g_serial) || !platform_map_nv12_buffer(&y, &y_stride, &uv, &uv_stride))
        return;
    start = now_ns();
    if (!cluster_decoder_copy(&g_serial, y, y_stride, uv, uv_stride, w, h)) return;
    copied = now_ns();
    if (!platform_map_nv12_post()) return;
    count_post(start, copied, now_ns());
    if (window != g_window) fprintf(stderr, "map_layer: the phone's cluster map on map window %u\n", window);
    g_window = window;
}

void map_layer_sleep(int64_t ns) {
    const int64_t deadline = now_ns() + ns;
    int64_t left = ns;
    while (g_have && left > 0) {
        unsigned before = g_serial;
        if (!cluster_decoder_wait(g_serial, left)) return;     /* it slept until the deadline */
        map_layer_tick();
        left = deadline - now_ns();
        if (g_serial == before) break;          /* not shown (no buffer): sleep the rest, no spin */
    }
    if (left > 0) {
        struct timespec t = { (time_t)(left / 1000000000), (long)(left % 1000000000) };
        while (nanosleep(&t, &t) != 0 && errno == EINTR) { }
    }
}

static int g_run, g_started;
static pthread_t g_thread;

static int running(void) { return __sync_fetch_and_add(&g_run, 0); }

static void *map_main(void *unused) {
    int64_t next_recover = now_ns() + MAP_RECOVER_MS * 1000000LL;
    (void)unused;
    while (running()) {
        int64_t now = now_ns();
        platform_map_check(0);
        if (now >= next_recover) {
            next_recover = now + MAP_RECOVER_MS * 1000000LL;
            platform_map_check(1);
        }
        map_layer_tick();
        map_layer_sleep(MAP_CHECK_MS * 1000000LL);
    }
    cluster_video_want(0);
    return NULL;
}

void map_layer_start(void) {
    if (g_started) return;
    __sync_lock_test_and_set(&g_run, 1);
    if (pthread_create(&g_thread, NULL, map_main, NULL) != 0) {
        fprintf(stderr, "map_layer: map thread failed errno=%d; no MAP view pictures\n", errno);
        __sync_lock_test_and_set(&g_run, 0);
        return;
    }
    g_started = 1;
}

void map_layer_stop(void) {
    if (!g_started) return;
    __sync_lock_test_and_set(&g_run, 0);
    pthread_join(g_thread, NULL);
    g_started = 0;
}
