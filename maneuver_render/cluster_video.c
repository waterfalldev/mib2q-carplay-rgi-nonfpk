/* The phone's cluster stream, read from the hook's frame ring (cluster_video.h). */
#include "cluster_video.h"
#include "cluster_decoder.h"
#include "cluster_video_ring.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define CV_BUSY_SLEEP_MS   4        /* between polls while units arrive (30 fps: one per 33 ms) */
#define CV_IDLE_SLEEP_MS   100      /* once nothing came for CV_IDLE_AFTER_MS, or without a ring */
#define CV_IDLE_AFTER_MS   2000
#define CV_ATTACH_RETRY_MS 1000

static int g_run, g_started, g_wanted;

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static int running(void) { return __sync_fetch_and_add(&g_run, 0); }

/* Sleeps `ms`, in steps short enough to notice cluster_video_stop. */
static void pause_ms(long ms) {
    while (ms > 0 && running()) {
        long step = ms < CV_IDLE_SLEEP_MS ? ms : CV_IDLE_SLEEP_MS;
        struct timespec t = { 0, step * 1000000L };
        while (nanosleep(&t, &t) != 0 && errno == EINTR) { }
        ms -= step;
    }
}

static void *video_main(void *unused) {
    static uint8_t unit[CVR_RECORD_MAX];
    cvr_reader_t r;
    cvr_record_t rec;
    int attached = 0, last_errno = 0, decoding = 0;
    uint64_t last_unit = 0, last_request = 0;
    (void)unused;
    while (running()) {
        uint64_t now = now_us();
        if (!__sync_fetch_and_add(&g_wanted, 0)) {
            if (decoding) cluster_decoder_close();
            decoding = 0;
            pause_ms(CV_IDLE_SLEEP_MS);
            continue;
        }
        if (!attached) {
            if (cvr_reader_attach(&r) != 0) {
                if (errno != last_errno)
                    fprintf(stderr, "cluster_video: no frame ring %s yet (errno=%d)\n", CVR_SHM_NAME, errno);
                last_errno = errno;
                pause_ms(CV_ATTACH_RETRY_MS);
                continue;
            }
            attached = 1;
            fprintf(stderr, "cluster_video: following the frame ring %s\n", CVR_SHM_NAME);
        }
        if (!decoding) {                                /* the decoder starts at a config and key frame */
            decoding = 1;
            cvr_reader_restart(&r);
            last_unit = now;
        }
        if (cvr_reader_next(&r, &rec, unit, sizeof(unit))) {
            if (cluster_decoder_feed(rec.session, (rec.flags & CVR_CONFIG) != 0, unit, rec.bytes, rec.time_ns) > 0)
                cvr_reader_restart(&r);                 /* it missed a unit: resume at a decodable point */
            last_unit = now;
            continue;
        }
        if (r.wants_key && (!last_request || now - last_request >= CV_KEYFRAME_RETRY_MS * 1000u)) {
            cvr_reader_request_key(&r);
            last_request = now;
            fprintf(stderr, "cluster_video: stream %u: no decodable point in the ring; key frame asked for\n", r.session);
        }
        cluster_decoder_poll();
        if (now - last_unit >= CV_IDLE_AFTER_MS * 1000u) {
            cluster_decoder_close();                    /* the stream stopped: reopen at its next config */
            decoding = 0;
        }
        pause_ms(now - last_unit < CV_IDLE_AFTER_MS * 1000u ? CV_BUSY_SLEEP_MS : CV_IDLE_SLEEP_MS);
    }
    cluster_decoder_close();
    if (attached) cvr_reader_detach(&r);
    return NULL;
}

void cluster_video_start(void) {
    pthread_t thread;
    if (g_started) return;
    __sync_lock_test_and_set(&g_run, 1);
    if (pthread_create(&thread, NULL, video_main, NULL) != 0) {
        fprintf(stderr, "cluster_video: reader thread failed errno=%d\n", errno);
        __sync_lock_test_and_set(&g_run, 0);
        return;
    }
    pthread_detach(thread);
    g_started = 1;
}

void cluster_video_stop(void) {
    __sync_lock_test_and_set(&g_run, 0);
}

void cluster_video_want(int wanted) {
    __sync_lock_test_and_set(&g_wanted, wanted ? 1 : 0);
}
