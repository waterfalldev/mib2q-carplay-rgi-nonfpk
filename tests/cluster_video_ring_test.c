/* The frame ring from the hook to the renderer (common/cluster_video_ring.h), writer and
 * readers in one process on a small ring: records come out whole and in order; a reader
 * that joins late starts at the stream's config and then its newest key frame;
 * one overtaken by the writer starts over there, or asks for a key frame when the ring no
 * longer has one; the tail of the ring is skipped; a new stream or a new writer starts every
 * reader over; records too big are refused or passed over; and with the writer and a slow
 * reader racing on two threads, no torn record is ever delivered. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cluster_video_ring.h"
#include "most/check.h"

static uint8_t g_out[CVR_RECORD_MAX];

/* A record's payload: its session, its sequence and its length, recognizable byte by byte. */
static uint8_t fill(uint32_t session, uint32_t seq, uint32_t i) {
    return (uint8_t)(session * 31u + seq * 7u + i * 13u + (i >> 8));
}

static int intact(const cvr_record_t *r, const uint8_t *payload) {
    uint32_t i;
    for (i = 0; i < r->bytes; ++i) if (payload[i] != fill(r->session, r->seq, i)) return 0;
    return 1;
}

static void put(cvr_writer_t *w, uint32_t bytes, uint32_t flags) {
    uint32_t start, i;
    uint8_t *p = cvr_writer_begin(w, bytes, &start);
    if (!p) return;
    for (i = 0; i < bytes; ++i) p[i] = fill(w->session, w->seq, i);
    cvr_writer_commit(w, start, bytes, flags, 0);
}

/* The next record's flags and sequence, or -1 when there is none; payload checked. */
static int take(cvr_reader_t *r, uint32_t *seq) {
    cvr_record_t rec;
    if (!cvr_reader_next(r, &rec, g_out, sizeof(g_out))) return -1;
    check(intact(&rec, g_out), "record %u of stream %u arrives intact", rec.seq, rec.session);
    if (seq) *seq = rec.seq;
    return (int)rec.flags;
}

static void basics(void) {
    cvr_writer_t w;
    cvr_reader_t r, late;
    uint32_t seq = 99, i;
    int f;
    shm_unlink(CVR_SHM_NAME);
    check(cvr_reader_attach(&r) != 0, "no ring before the writer sets it up");
    check(cvr_writer_open(&w, 5) == 0, "the writer sets the ring up");
    check(w.h->magic == CVR_MAGIC && w.h->epoch == 5 && w.h->session == 0, "header: magic, the seeded epoch, no stream yet");
    check(cvr_reader_attach(&r) == 0, "a reader attaches to it");
    check(take(&r, NULL) == -1, "nothing to read before the first stream");

    cvr_writer_new_session(&w);
    put(&w, 40, CVR_CONFIG);
    put(&w, 3000, CVR_FRAME | CVR_KEY);
    for (i = 0; i < 3; ++i) put(&w, 900 + i, CVR_FRAME);
    f = take(&r, &seq);
    check(f == (int)CVR_CONFIG && seq == 0, "the stream's config comes first (%d, %u)", f, seq);
    f = take(&r, &seq);
    check(f == (int)(CVR_FRAME | CVR_KEY) && seq == 1, "then its key frame");
    for (i = 0; i < 3; ++i) {
        f = take(&r, &seq);
        check(f == (int)CVR_FRAME && seq == 2 + i, "then frame %u in order", 2 + i);
    }
    check(take(&r, NULL) == -1, "and nothing more");

    check(cvr_reader_attach(&late) == 0, "a second reader attaches late");
    put(&w, 500, CVR_FRAME);
    put(&w, 2500, CVR_FRAME | CVR_KEY);
    put(&w, 500, CVR_FRAME);
    f = take(&late, &seq);
    check(f == (int)CVR_CONFIG && seq == 0, "a late reader starts at the config");
    f = take(&late, &seq);
    check(f == (int)(CVR_FRAME | CVR_KEY) && seq == 6 && take(&late, &seq) == (int)CVR_FRAME && seq == 7,
          "then jumps to the newest key frame, no backlog (seq %u)", seq);
    f = take(&r, &seq);
    check(f == (int)CVR_FRAME && seq == 5, "the first reader just gets the new frames");
    take(&r, NULL);
    take(&r, NULL);

    /* A config followed by frames but no key frame: passed over, then asked for. */
    cvr_writer_new_session(&w);
    put(&w, 40, CVR_CONFIG);
    for (i = 0; i < CVR_KEY_WAIT - 1; ++i) put(&w, 100, CVR_FRAME);
    f = take(&r, &seq);
    check(f == (int)CVR_CONFIG && seq == 0, "a new stream starts the reader over at its config");
    check(take(&r, NULL) == -1 && !r.wants_key, "frames before a key frame are passed over, quietly at first");
    put(&w, 100, CVR_FRAME);
    check(take(&r, NULL) == -1 && r.wants_key && r.waited == CVR_KEY_WAIT,
          "after %d of them the reader wants a key frame (%u passed over)", CVR_KEY_WAIT, r.waited);
    cvr_reader_request_key(&r);
    check(!r.wants_key && cvr_writer_requests(&w) == 1 && cvr_writer_requests(&w) == 0,
          "the request reaches the writer once");
    put(&w, 40, CVR_CONFIG);
    put(&w, 2000, CVR_FRAME | CVR_KEY);
    check(take(&r, NULL) == (int)CVR_CONFIG && take(&r, NULL) == (int)(CVR_FRAME | CVR_KEY),
          "the phone's answer - config and key frame - is delivered");

    /* Overtaken: the config, key frame and everything after them overwritten unread. */
    for (i = 0; i < 3 * CVR_DATA_BYTES / 1000; ++i) put(&w, 1000 - 24, CVR_FRAME);
    check(take(&r, NULL) == -1 && r.lost >= 1 && r.wants_key,
          "a reader overtaken past the stream's config loses its place and wants a key frame");
    put(&w, 40, CVR_CONFIG);
    put(&w, 1500, CVR_FRAME | CVR_KEY);
    put(&w, 300, CVR_FRAME);
    check(take(&r, NULL) == (int)CVR_CONFIG && take(&r, NULL) == (int)(CVR_FRAME | CVR_KEY)
          && take(&r, NULL) == (int)CVR_FRAME, "and resumes at the next config and key frame");

    /* Records too big for the ring, and for a reader's buffer. */
    {
        uint32_t start;
        check(cvr_writer_begin(&w, CVR_RECORD_MAX, &start) == NULL, "a record bigger than CVR_RECORD_MAX is refused");
    }
    put(&w, 600, CVR_FRAME | CVR_KEY);
    put(&w, 20, CVR_FRAME);
    put(&w, 30, CVR_FRAME | CVR_KEY);
    {
        cvr_record_t rec;
        uint8_t small[100];
        memset(&rec, 0, sizeof(rec));
        check(cvr_reader_next(&r, &rec, small, sizeof(small)) == 1 && rec.bytes == 30 && rec.seq == w.seq - 1,
              "a payload bigger than the reader's buffer is passed over, and the frames that need it, "
              "up to the next key frame (%u bytes)", rec.bytes);
    }

    /* A new writer process: every reader starts over at its first stream's config. */
    check(cvr_writer_open(&w, 1) == 0 && w.h->epoch != 5 && w.h->epoch != 0, "a new writer takes the ring over");
    check(take(&r, NULL) == -1, "nothing until its first stream");
    cvr_writer_new_session(&w);
    put(&w, 40, CVR_CONFIG);
    put(&w, 800, CVR_FRAME | CVR_KEY);
    check(take(&r, NULL) == (int)CVR_CONFIG && take(&r, NULL) == (int)(CVR_FRAME | CVR_KEY),
          "and then reads it from its config");
    cvr_reader_detach(&late);
    cvr_reader_detach(&r);
    cvr_writer_close(&w);
}

/* Records of every size wrap around the ring many times; a reader keeping up gets each one. */
static void wrapping(void) {
    cvr_writer_t w;
    cvr_reader_t r;
    uint32_t i, expected = 0, got = 0, seq, bad = 0;
    check(cvr_writer_open(&w, 1) == 0 && cvr_reader_attach(&r) == 0, "ring for the wrap test");
    cvr_writer_new_session(&w);
    put(&w, 40, CVR_CONFIG);
    put(&w, 100, CVR_FRAME | CVR_KEY);
    got = (take(&r, &seq) == (int)CVR_CONFIG) + (take(&r, &seq) == (int)(CVR_FRAME | CVR_KEY));
    expected = 2;
    for (i = 0; i < 20000; ++i) {
        uint32_t bytes = (i * 7919u) % (CVR_RECORD_MAX - 64) + 1;
        put(&w, bytes, CVR_FRAME | ((i % 50) == 0 ? CVR_KEY : 0));
        while (take(&r, &seq) >= 0) {
            if (seq != expected) ++bad;
            expected = seq + 1;
            ++got;
        }
    }
    check(got == 20002 && bad == 0 && r.lost == 0,
          "%u of 20002 records through %u wraps, in order (%u out of order, %u lost)",
          got, w.pos / CVR_DATA_BYTES, bad, r.lost);
    cvr_reader_detach(&r);
    cvr_writer_close(&w);
}

/* The writer runs flat out on its own thread; a reader that sometimes sleeps is overtaken
 * again and again.  Whatever it is given must be whole. */
static volatile int g_stop;
static void *writer_main(void *arg) {
    cvr_writer_t *w = (cvr_writer_t *)arg;
    uint32_t i = 0;
    while (!g_stop) {
        if (i % 4000 == 0) {
            cvr_writer_new_session(w);
            put(w, 40, CVR_CONFIG);
        }
        put(w, (i * 2654435761u) % 9000 + 1, CVR_FRAME | ((i % 30) == 0 ? CVR_KEY : 0));
        if (cvr_writer_requests(w)) put(w, 40, CVR_CONFIG);
        ++i;
    }
    return NULL;
}

static void racing(void) {
    cvr_writer_t w;
    cvr_reader_t r;
    pthread_t thread;
    struct timespec until, now;
    uint32_t delivered = 0, torn = 0;
    cvr_record_t rec;
    check(cvr_writer_open(&w, 2) == 0 && cvr_reader_attach(&r) == 0, "ring for the race");
    g_stop = 0;
    pthread_create(&thread, NULL, writer_main, &w);
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += 2;
    do {
        int k;
        for (k = 0; k < 50; ++k) {
            if (!cvr_reader_next(&r, &rec, g_out, sizeof(g_out))) break;
            ++delivered;
            if (!intact(&rec, g_out)) ++torn;
        }
        if (r.wants_key) cvr_reader_request_key(&r);
        if (delivered % 7 == 0) usleep(200);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec < until.tv_sec || (now.tv_sec == until.tv_sec && now.tv_nsec < until.tv_nsec));
    g_stop = 1;
    pthread_join(thread, NULL);
    check(delivered > 100 && torn == 0 && r.lost > 0,
          "racing a writer: %u records delivered, %u torn, overtaken %u times", delivered, torn, r.lost);
    cvr_reader_detach(&r);
    cvr_writer_close(&w);
}

int main(void) {
    basics();
    wrapping();
    racing();
    shm_unlink(CVR_SHM_NAME);
    printf("cluster_video_ring_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
