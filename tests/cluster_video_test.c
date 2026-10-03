/* The renderer's reader of the cluster stream (maneuver_render/cluster_video.c) against a
 * writer in this process and a fake decoder: while the map window wants pictures it attaches
 * to the ring once the hook sets it up, feeds the decoder from a config and key frame, asks for
 * a key frame when the stream has none it can start at, starts over at a decodable point when
 * the decoder missed a unit, and closes the decoder when the window no longer wants. */
#include <stdio.h>
#include <time.h>

#include "../maneuver_render/cluster_video.c"
#include "most/check.h"

static unsigned g_fed, g_closes, g_drop_at;
static uint8_t g_first[8];                /* NAL types of the first units fed since the last reset */

int cluster_decoder_feed(uint32_t stream, int config, const uint8_t *unit, uint32_t bytes, uint64_t time_ns) {
    (void)stream; (void)config; (void)bytes; (void)time_ns;
    if (g_fed < sizeof(g_first)) g_first[g_fed] = unit[4];
    return ++g_fed == g_drop_at ? 1 : 0;
}
void cluster_decoder_poll(void) { }
void cluster_decoder_close(void) { ++g_closes; }

static void reset_fed(void) { g_fed = 0; memset(g_first, 0, sizeof(g_first)); }

static void put(cvr_writer_t *w, uint32_t bytes, uint32_t flags) {
    uint32_t start, i;
    uint8_t *p = cvr_writer_begin(w, bytes, &start);
    if (!p) return;
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
    p[4] = (flags & CVR_CONFIG) ? 0x67 : (flags & CVR_KEY) ? 0x65 : 0x41;
    for (i = 5; i < bytes; ++i) p[i] = (uint8_t)i;
    cvr_writer_commit(w, start, bytes, flags, 0);
}

/* Waits up to `ms` for `done`. */
#define WAIT(done, ms) do { int waited_; for (waited_ = 0; waited_ < (ms) && !(done); waited_ += 10) { \
    struct timespec t_ = { 0, 10000000L }; nanosleep(&t_, NULL); } } while (0)

int main(void) {
    cvr_writer_t w;
    uint32_t asked = 0;
    unsigned closes;
    int i;
    shm_unlink(CVR_SHM_NAME);
    cluster_video_start();
    cluster_video_want(1);
    WAIT(0, 300);                                       /* the reader finds no ring yet */
    check(cvr_writer_open(&w, 1) == 0, "the hook sets the ring up after the reader started");
    cvr_writer_new_session(&w);
    put(&w, 40, CVR_CONFIG);
    put(&w, 3000, CVR_FRAME | CVR_KEY);
    for (i = 0; i < 4; ++i) put(&w, 500, CVR_FRAME);
    WAIT(g_fed == 6, 3000);
    check(g_fed == 6 && g_first[0] == 0x67 && g_first[1] == 0x65 && g_first[2] == 0x41,
          "the reader attaches and feeds the config, the key frame and the frames (%u)", g_fed);

    cvr_writer_requests(&w);
    reset_fed();
    cvr_writer_new_session(&w);
    put(&w, 40, CVR_CONFIG);
    for (i = 0; i < CVR_KEY_WAIT; ++i) put(&w, 300, CVR_FRAME);
    WAIT((asked += cvr_writer_requests(&w)) > 0, 2000);
    check(asked == 1, "a new stream without a key frame: the reader asks for one (%u)", asked);
    check(g_fed == 1 && g_first[0] == 0x67, "and feeds only its config meanwhile (%u)", g_fed);
    put(&w, 40, CVR_CONFIG);
    put(&w, 2000, CVR_FRAME | CVR_KEY);
    put(&w, 300, CVR_FRAME);
    WAIT(g_fed == 4, 2000);
    check(g_fed == 4 && g_first[1] == 0x67 && g_first[2] == 0x65 && g_first[3] == 0x41,
          "the requested config and key frame resume it (%u)", g_fed);

    reset_fed();
    g_drop_at = 1;
    put(&w, 300, CVR_FRAME);
    WAIT(g_fed >= 5, 2000);
    check(g_fed == 5 && g_first[1] == 0x67 && g_first[2] == 0x65,
          "a unit the decoder missed: it starts over at the config and key frame, then every frame since (%u)",
          g_fed);
    g_drop_at = 0;

    closes = g_closes;
    cluster_video_want(0);
    WAIT(g_closes > closes, 2000);
    reset_fed();
    put(&w, 300, CVR_FRAME);
    WAIT(0, 300);
    check(g_closes > closes && g_fed == 0, "the window no longer wants: the decoder is closed and fed nothing");
    cluster_video_want(1);
    WAIT(g_fed >= 4, 2000);
    check(g_first[0] == 0x67 && g_first[1] == 0x65, "wanted again: from the config and key frame (%u)", g_fed);
    cluster_video_stop();
    WAIT(0, 200);
    cvr_writer_close(&w);
    shm_unlink(CVR_SHM_NAME);
    printf("cluster_video_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
