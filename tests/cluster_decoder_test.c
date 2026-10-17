/* The renderer's hardware decoder client (maneuver_render/cluster_decoder.c) against a fake
 * OpenMAX core and AVC component in this program (CD_OMX_CORE NULL: dlopen of itself).  The
 * component completes Idle only once every buffer is allocated and Loaded once every buffer
 * is freed, returns its buffers on the way to Idle, and answers every frame with a TILE_4x2
 * picture written from the layout's literal tile order (tile_order), not the decoder's formula. */
#define CD_OMX_CORE NULL
#define CD_STATE_WAIT_MS 300
#include "../maneuver_render/cluster_decoder.c"
#include "../maneuver_render/h264_sps.c"
#include "most/check.h"

#include <stdlib.h>

#define FAKE_EXT    0x7F000010u
#define FAKE_TILED  0x7F000004u
#define FAKE_BUFFER (434 * 1024)        /* 800 x 298 in TILE_4x2 and some (map19: 434224) */

static uint8_t luma_at(size_t x, size_t y, unsigned frame) { return (uint8_t)(x * 3 + y * 7 + frame); }
static uint8_t chroma_at(size_t x, size_t y, unsigned frame) { return (uint8_t)(x * 5 + y * 11 + frame * 3); }

/* Video range to full, rounded to nearest: luma 16-235 and chroma 16-240 to 0-255. */
static uint8_t full(uint8_t v, int top) {
    double s = (v - 16) * 255.0 / (top - 16);
    return (uint8_t)(s <= 0 ? 0 : s >= 255 ? 255 : (int)(s + 0.5));
}
static uint8_t full_luma(uint8_t v) { return full(v, 235); }
static uint8_t full_chroma(uint8_t v) { return full(v, 240); }

/* Storage position of every tile of a TILE_4x2 plane, from the layout's literal order: each pair
 * of tile rows stores T0 T1, then B0-B3, T2-T5, B4-B7, T6-T9 ... (T its top row, B its bottom
 * row); an unpaired last row runs straight. */
static void tile_order(size_t cols, size_t rows, size_t *pos) {
    size_t n = 0, p, k, i;
    for (p = 0; p + 1 < rows; p += 2) {
        pos[p * cols] = n++;
        pos[p * cols + 1] = n++;
        for (k = 0; k < cols; k += 4) {
            for (i = k; i < k + 4 && i < cols; ++i) pos[(p + 1) * cols + i] = n++;
            for (i = k + 2; i < k + 6 && i < cols; ++i) pos[p * cols + i] = n++;
        }
    }
    if (rows & 1) for (i = 0; i < cols; ++i) pos[(rows - 1) * cols + i] = n++;
}

/* One plane of w bytes x h lines as TILE_4x2 at dst; returns its bytes. */
static size_t write_tiled(uint8_t *dst, size_t w, size_t h, int chroma, unsigned frame) {
    size_t cols = ((w + 63) / 64 + 1) & ~(size_t)1, rows = (h + 31) / 32, pos[64 * 16], tx, ty, r, c;
    tile_order(cols, rows, pos);
    for (ty = 0; ty < rows; ++ty)
        for (tx = 0; tx < cols; ++tx)
            for (r = 0; r < 32; ++r)
                for (c = 0; c < 64; ++c) {
                    size_t x = tx * 64 + c, y = ty * 32 + r;
                    dst[pos[ty * cols + tx] * 2048 + r * 64 + c] = x >= w || y >= h ? 0xee
                        : chroma ? chroma_at(x, y, frame) : luma_at(x, y, frame);
                }
    return cols * rows * 2048;
}

static struct {
    omx_component_t c;
    const omx_callbacks_t *cb;
    omx_port_t port[2];
    uint32_t order[2];
    int state, pending, handles, live_handles, refuse_handle, refuse_tiled, short_pictures;
    unsigned allocated, freed, frames, configs, flags_ok, held;
    omx_header_t *queue[CD_MAX_BUFFERS];
    unsigned queued;
    uint8_t config[16];
} f;

static void complete(int state) {
    f.state = state;
    f.pending = 0;
    f.cb->event(&f.c, NULL, OMX_EVENT_CMD_COMPLETE, OMX_COMMAND_STATE_SET, (uint32_t)state, NULL);
}

static unsigned total_buffers(void) { return f.port[0].count_actual + f.port[1].count_actual; }

static int fake_send(omx_component_t *c, int cmd, uint32_t state, void *data) {
    unsigned i;
    (void)c; (void)data;
    if (cmd != OMX_COMMAND_STATE_SET) return (int)0x80001000u;
    if (state == OMX_STATE_IDLE && f.state == OMX_STATE_EXECUTING) {
        for (i = 0; i < f.queued; ++i) {             /* outputs come back empty */
            f.queue[i]->filled_len = 0;
            f.cb->fill_done(&f.c, NULL, f.queue[i]);
        }
        f.queued = 0;
        complete(OMX_STATE_IDLE);
    } else if (state == OMX_STATE_IDLE || state == OMX_STATE_LOADED) {
        f.pending = (int)state;                      /* once every buffer is there / gone */
    } else if (state == OMX_STATE_EXECUTING) {
        complete(OMX_STATE_EXECUTING);
    }
    return 0;
}

static int fake_get(omx_component_t *c, uint32_t index, void *p) {
    (void)c;
    if (index == OMX_INDEX_PORT_DEFINITION) {
        omx_port_t *d = p;
        memcpy(d, &f.port[d->port], sizeof(*d));
        return 0;
    }
    if (index == FAKE_EXT) return 0;
    return (int)0x8000101Au;
}

static int fake_set(omx_component_t *c, uint32_t index, void *p) {
    (void)c;
    if (index == FAKE_EXT) {
        memcpy(f.order, (uint32_t *)p + 3, sizeof(f.order));
        return 0;
    }
    if (index == OMX_INDEX_PORT_DEFINITION) {
        omx_port_t *d = p, *s = &f.port[d->port];
        if (d->port == OMX_PORT_OUT) {
            if (f.refuse_tiled || d->video.color != FAKE_TILED) return (int)0x80001019u;  /* UnsupportedSetting */
            s->video.color = d->video.color;
        }
        s->count_actual = d->count_actual;
        s->video.width = d->video.width;
        s->video.height = d->video.height;
        s->video.compression = d->video.compression;
        return 0;
    }
    return (int)0x8000101Au;
}

static int fake_ext(omx_component_t *c, const char *name, uint32_t *index) {
    (void)c;
    if (strcmp(name, CD_PICTURE_ORDER_EXTENSION) != 0) return (int)0x8000101Au;
    *index = FAKE_EXT;
    return 0;
}

static int fake_allocate(omx_component_t *c, omx_header_t **h, uint32_t port, void *app, uint32_t bytes) {
    (void)c; (void)app;
    *h = calloc(1, sizeof(**h));
    (*h)->buffer = calloc(1, bytes);
    (*h)->alloc_len = bytes;
    (*h)->input_port = port;
    if (++f.allocated - f.freed == total_buffers() && f.pending == OMX_STATE_IDLE) complete(OMX_STATE_IDLE);
    return 0;
}

static int fake_free(omx_component_t *c, uint32_t port, omx_header_t *h) {
    (void)c; (void)port;
    free(h->buffer);
    free(h);
    if (++f.freed == f.allocated && f.pending == OMX_STATE_LOADED) complete(OMX_STATE_LOADED);
    return 0;
}

/* Every frame becomes a picture in the oldest queued output. */
static int fake_empty(omx_component_t *c, omx_header_t *h) {
    size_t luma;
    omx_header_t *out;
    (void)c;
    if (f.state != OMX_STATE_EXECUTING) return (int)0x80001017u;
    if (h->flags == OMX_FLAG_CODECCONFIG) {
        ++f.configs;
        f.flags_ok += h->filled_len == sizeof(f.config) && memcmp(h->buffer, f.config, sizeof(f.config)) == 0;
    } else {
        ++f.frames;
        f.flags_ok += h->flags == OMX_FLAG_ENDOFFRAME;
    }
    f.cb->empty_done(&f.c, NULL, h);
    if (h->flags != OMX_FLAG_ENDOFFRAME || !f.queued) return 0;
    out = f.queue[0];
    memmove(f.queue, f.queue + 1, --f.queued * sizeof(f.queue[0]));
    memset(out->buffer, 0xee, out->alloc_len);
    out->offset = 0;
    luma = (write_tiled(out->buffer, CD_WIDTH, CD_HEIGHT, 0, f.frames) + 8191) / 8192 * 8192;
    out->filled_len = (uint32_t)(luma + write_tiled(out->buffer + luma, CD_WIDTH, CD_HEIGHT / 2, 1, f.frames));
    if (f.short_pictures) out->filled_len /= 2;
    f.cb->fill_done(&f.c, NULL, out);
    return 0;
}

static int fake_fill(omx_component_t *c, omx_header_t *h) {
    (void)c;
    f.queue[f.queued++] = h;
    return 0;
}

int OMX_Init(void) { return 0; }

int OMX_GetHandle(omx_component_t **handle, const char *name, void *app, const omx_callbacks_t *cb) {
    (void)app;
    ++f.handles;
    if (f.refuse_handle || strcmp(name, CD_COMPONENT) != 0) return (int)0x80001003u;  /* ComponentNotFound */
    memset(&f.c, 0, sizeof(f.c));
    f.c.send_command = fake_send;
    f.c.get_parameter = fake_get;
    f.c.set_parameter = fake_set;
    f.c.get_extension_index = fake_ext;
    f.c.allocate_buffer = fake_allocate;
    f.c.free_buffer = fake_free;
    f.c.empty_this_buffer = fake_empty;
    f.c.fill_this_buffer = fake_fill;
    f.cb = cb;
    f.state = OMX_STATE_LOADED;
    f.queued = 0;
    memset(f.port, 0, sizeof(f.port));
    f.port[0].port = OMX_PORT_IN;
    f.port[0].count_min = f.port[0].count_actual = 4;
    f.port[0].buffer_size = 64 * 1024;
    f.port[1].port = OMX_PORT_OUT;
    f.port[1].count_min = f.port[1].count_actual = 3;
    f.port[1].buffer_size = FAKE_BUFFER;
    f.port[1].video.width = CD_WIDTH;
    f.port[1].video.height = CD_HEIGHT;
    f.port[1].video.color = FAKE_TILED;
    ++f.live_handles;
    *handle = &f.c;
    return 0;
}

int OMX_FreeHandle(omx_component_t *handle) {
    (void)handle;
    --f.live_handles;
    return 0;
}

#define TW (CD_WIDTH - 10)                /* a target smaller than the picture, padded rows */
#define TH (CD_HEIGHT - 2)
#define TPAD 20
static uint8_t g_y[TH][TW + TPAD], g_uv[TH / 2][TW + TPAD];

/* The target holds picture `frame`, expanded to full range or (`as_is`) unchanged. */
static int target_is(unsigned frame, int as_is) {
    int x, y, ok = 1;
    for (y = 0; y < TH; ++y) {
        for (x = 0; x < TW; ++x) {
            uint8_t v = luma_at((size_t)x, (size_t)y, frame);
            ok &= g_y[y][x] == (as_is ? v : full_luma(v));
        }
        for (x = TW; x < TW + TPAD; ++x) ok &= g_y[y][x] == 0x11;
    }
    for (y = 0; y < TH / 2; ++y) {
        for (x = 0; x < TW; ++x) {
            uint8_t v = chroma_at((size_t)x, (size_t)y, frame);
            ok &= g_uv[y][x] == (as_is ? v : full_chroma(v));
        }
        for (x = TW; x < TW + TPAD; ++x) ok &= g_uv[y][x] == 0x11;
    }
    return ok;
}

static int tiled_target_is(unsigned frame) { return target_is(frame, 0); }

/* An Annex B SPS written bit by bit, emulation prevention inserted as an encoder does. */
static struct { uint8_t rbsp[128]; size_t bits; } w;

static void put(unsigned v, int bits) {
    while (bits-- > 0) {
        if (v >> bits & 1) w.rbsp[w.bits / 8] |= (uint8_t)(0x80 >> w.bits % 8);
        ++w.bits;
    }
}

static void put_ue(unsigned v) {
    int len = 0;
    while ((v + 1) >> (len + 1)) ++len;
    put(0, len);
    put(v + 1, len + 1);
}

static void put_se(int v) { put_ue(v > 0 ? (unsigned)(2 * v - 1) : (unsigned)(-2 * v)); }

static size_t sps_out(uint8_t *out) {
    size_t n = 0, i, zeros = 0;
    put(1, 1);                                                  /* rbsp stop bit */
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1; out[n++] = 0x67;
    for (i = 0; i < (w.bits + 7) / 8; ++i) {
        if (zeros >= 2 && w.rbsp[i] <= 3) { out[n++] = 3; zeros = 0; }
        out[n++] = w.rbsp[i];
        zeros = w.rbsp[i] ? 0 : zeros + 1;
    }
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1; out[n++] = 0x68; out[n++] = 0xce;   /* its PPS */
    return n;
}

/* Profile 66, no VUI. */
static size_t sps_plain(uint8_t *out) {
    memset(&w, 0, sizeof(w));
    put(66, 8); put(0xc0, 8); put(31, 8); put_ue(0);
    put_ue(0); put_ue(0); put_ue(4);                            /* frame num, poc type 0 and its lsb */
    put_ue(1); put(0, 1); put_ue(49); put_ue(18);               /* 800 x 304 */
    put(1, 1); put(1, 1); put(1, 1);                            /* frame_mbs_only, direct, cropping: */
    put_ue(0); put_ue(0); put_ue(0); put_ue(3);                 /* to 298 */
    put(0, 1);                                                  /* no VUI */
    return sps_out(out);
}

/* High profile through every branch the reader skips: a scaling list ending early, poc type 1,
 * no frame_mbs_only, an extended aspect ratio of zeros (emulation prevention), the given range
 * and, when `matrix` >= 0, a colour description. */
static size_t sps_high(uint8_t *out, int full, int matrix) {
    int j;
    memset(&w, 0, sizeof(w));
    put(100, 8); put(0, 8); put(0, 8); put_ue(0);
    put_ue(1); put_ue(0); put_ue(0); put(0, 1);                 /* 4:2:0, 8 bits */
    put(1, 1);                                                  /* scaling matrix */
    put(1, 1); put_se(-8);                                      /* list 0: next 0 at once */
    put(1, 1); for (j = 0; j < 16; ++j) put_se(j & 1 ? -3 : 5); /* list 1: all 16 deltas */
    for (j = 2; j < 8; ++j) put(0, 1);
    put_ue(0); put_ue(1); put(0, 1); put_se(-2); put_se(1);     /* poc type 1 */
    put_ue(2); put_se(1); put_se(-1);
    put_ue(1); put(0, 1); put_ue(49); put_ue(9);
    put(0, 1); put(1, 1); put(1, 1);                            /* field coding, mbaff, direct */
    put(0, 1);                                                  /* no cropping */
    put(1, 1);                                                  /* VUI */
    put(1, 1); put(255, 8); put(0, 16); put(0, 16);             /* aspect ratio 0:0 */
    put(1, 1); put(0, 1);                                       /* overscan */
    put(1, 1); put(5, 3); put((unsigned)full, 1);               /* video signal type */
    put(matrix >= 0, 1);
    if (matrix >= 0) { put(1, 8); put(1, 8); put((unsigned)matrix, 8); }
    put(0, 1);                                                  /* no timing, and so on */
    return sps_out(out);
}

static int copy(unsigned *serial) {
    return cluster_decoder_copy(serial, &g_y[0][0], TW + TPAD, &g_uv[0][0], TW + TPAD, TW, TH);
}

static int64_t ms_since(uint64_t start) { return (int64_t)(monotonic_ns() - start) / 1000000; }

static int is_queued(const omx_header_t *h) {
    unsigned i;
    for (i = 0; i < f.queued; ++i) if (f.queue[i] == h) return 1;
    return 0;
}

/* The reader thread's side: a frame 50 ms from now, or the decoder closed. */
static uint8_t g_late_frame[100];
static void *feed_late(void *unused) {
    struct timespec t = { 0, 50 * 1000000L };
    (void)unused;
    nanosleep(&t, NULL);
    cluster_decoder_feed(1, 0, g_late_frame, sizeof(g_late_frame), 1000);
    return NULL;
}
static void *close_now(void *unused) {
    (void)unused;
    cluster_decoder_close();
    return NULL;
}

int main(void) {
    uint8_t frame[100];
    unsigned serial = 0, i;
    int fed = 0, handles;
    memset(f.config, 0x67, sizeof(f.config));
    memset(frame, 0x41, sizeof(frame));
    memset(g_y, 0x11, sizeof(g_y));
    memset(g_uv, 0x11, sizeof(g_uv));

    check(tiled_bytes(1024, 480) == 753664 && tiled_bytes(1024, 480) <= 753712,
          "TILE_4x2 of 1024 x 480 takes %u bytes, within stock's 753712-byte decoder buffer (map17)",
          (unsigned)tiled_bytes(1024, 480));
    check(cluster_decoder_feed(1, 0, frame, sizeof(frame), 0) == -1 && f.handles == 0,
          "a frame before any config opens nothing");
    check(cluster_decoder_feed(1, 1, f.config, sizeof(f.config), 0) == 0 && f.live_handles == 1,
          "the stream's config opens the decoder");
    check(f.state == OMX_STATE_EXECUTING && f.port[1].video.color == FAKE_TILED, "executing, output TILE_4x2");
    check(f.port[0].count_actual == CD_INPUT_BUFFERS && f.port[1].count_actual == 3 + CD_OUTPUT_EXTRA
          && f.port[0].video.compression == OMX_CODING_AVC && f.port[0].video.width == CD_WIDTH,
          "input: AVC at %ux%u, %u buffers; output: the minimum plus %u", f.port[0].video.width,
          f.port[0].video.height, f.port[0].count_actual, CD_OUTPUT_EXTRA);
    check(f.order[0] == 1 && f.order[1] == 1, "decode-order mode set, as stock does");
    check(f.configs == 1 && f.queued == f.port[1].count_actual, "the config went in, every output buffer waits");
    check(!cluster_decoder_fresh(serial) && copy(&serial) == 0, "no picture before a frame");

    for (i = 0; i < 12; ++i) fed += cluster_decoder_feed(1, 0, frame, sizeof(frame), 1000) == 0;
    check(fed == 12 && f.frames == 12 && f.flags_ok == 13, "12 frames in, as ENDOFFRAME (config as CODECCONFIG)");
    check(cluster_decoder_fresh(serial) && copy(&serial) == 1 && tiled_target_is(12),
          "the newest picture untiled into the target, both planes, its padding untouched");
    {
        uint64_t low = 0, high = 0;
        int x, y;
        for (y = 0; y < TH; ++y)
            for (x = 0; x < TW; ++x) {
                uint8_t v = luma_at((size_t)x, (size_t)y, 12);
                low += v < 16;
                high += v > 235;
            }
        check(g.luma[0] == (uint64_t)TW * TH && g.luma[1] == low && g.luma[2] == high,
              "its luma counted: %llu bytes, %llu below 16, %llu above 235", (unsigned long long)g.luma[0],
              (unsigned long long)g.luma[1], (unsigned long long)g.luma[2]);
    }
    check(g_full_luma[16] == 0 && g_full_luma[235] == 255 && g_full_luma[126] == 128 && g_full_chroma[16] == 0
          && g_full_chroma[128] == 128 && g_full_chroma[240] == 255 && g_full_luma[0] == 0 && g_full_chroma[255] == 255,
          "video range expanded to full: black 0, white 255, grey chroma 128, out of range clamped");
    check(!cluster_decoder_fresh(serial) && copy(&serial) == 0, "the same picture is not copied twice");
    cluster_decoder_poll();
    check(f.queued + 1 == f.port[1].count_actual, "every output but the newest went back to the decoder");
    for (i = 0; i < 3; ++i) cluster_decoder_feed(1, 0, frame, sizeof(frame), 1000);
    check(copy(&serial) == 1 && tiled_target_is(15), "later pictures follow");
    f.short_pictures = 1;
    cluster_decoder_feed(1, 0, frame, sizeof(frame), 1000);
    check(copy(&serial) == 0 && tiled_target_is(15), "a picture short of the layout is not shown");
    f.short_pictures = 0;

    {
        pthread_t thread;
        uint64_t start = monotonic_ns();
        unsigned seen = serial;
        omx_header_t *held;
        uint32_t w, h;
        unsigned picture;
        uint8_t before[64];

        check(cluster_decoder_wait(serial, 20 * 1000000LL) == 0 && ms_since(start) >= 15,
              "no new picture: the wait sleeps its time (%d ms) and says so", (int)ms_since(start));
        cluster_decoder_feed(1, 0, frame, sizeof(frame), 1000);
        start = monotonic_ns();
        check(cluster_decoder_wait(serial, 2000 * 1000000LL) == 1 && ms_since(start) < 5,
              "a picture already waiting: no wait");
        copy(&serial);
        memset(g_late_frame, 0x41, sizeof(g_late_frame));
        pthread_create(&thread, NULL, feed_late, NULL);
        start = monotonic_ns();
        check(cluster_decoder_wait(serial, 2000 * 1000000LL) == 1 && ms_since(start) >= 40 && ms_since(start) < 500,
              "a picture arriving during the wait ends it at once (%d ms of 2000)", (int)ms_since(start));
        pthread_join(thread, NULL);
        check(copy(&serial) == 1 && serial != seen && g.shown > 0, "that picture is copied and counted (%u shown)",
              g.shown);

        cluster_decoder_poll();
        cluster_decoder_feed(1, 0, frame, sizeof(frame), 1000);
        held = copy_begin(serial, &picture, &w, &h);
        memcpy(before, held->buffer, sizeof(before));
        for (i = 0; i < 3; ++i) cluster_decoder_feed(1, 0, frame, sizeof(frame), 1000);
        cluster_decoder_poll();
        check(held && g.newest != held && !is_queued(held) && memcmp(before, held->buffer, sizeof(before)) == 0,
              "a picture being copied stays out of the decoder while newer ones arrive, untouched");
        copy_end(held, 1);
        cluster_decoder_poll();
        check(is_queued(held) && f.queued + 1 == f.port[1].count_actual,
              "its copy done, it goes back; only the newest stays out");

        held = copy_begin(serial, &picture, &w, &h);
        pthread_create(&thread, NULL, close_now, NULL);
        {
            struct timespec t = { 0, 50 * 1000000L };
            nanosleep(&t, NULL);
        }
        check(held && f.allocated - f.freed == total_buffers(), "close waits for a copy in progress, freeing nothing");
        copy_end(held, 1);
        pthread_join(thread, NULL);
        check(f.live_handles == 0 && f.allocated == f.freed, "and frees everything once it ends");
        check(cluster_decoder_feed(1, 1, f.config, sizeof(f.config), 0) == 0 && f.live_handles == 1,
              "the stream's next config opens it again");
    }

    cluster_decoder_close();
    check(f.live_handles == 0 && f.state == OMX_STATE_LOADED && f.allocated == f.freed,
          "close: back to Loaded, %u of %u buffers freed, the handle freed", f.freed, f.allocated);
    check(copy(&serial) == 0 && !cluster_decoder_fresh(serial), "no picture after close");

    check(cluster_decoder_feed(2, 1, f.config, sizeof(f.config), 0) == 0 && f.live_handles == 1,
          "a new stream's config opens it again");
    check(cluster_decoder_feed(3, 1, f.config, sizeof(f.config), 0) == 0 && f.live_handles == 1
          && f.allocated == f.freed + total_buffers(), "a newer stream closes the old decoder first");

    f.refuse_tiled = 1;
    handles = f.handles;
    check(cluster_decoder_feed(7, 1, f.config, sizeof(f.config), 0) == -1 && f.live_handles == 0
          && f.allocated == f.freed, "TILE_4x2 refused: closed, nothing left allocated");
    check(cluster_decoder_feed(7, 1, f.config, sizeof(f.config), 0) == -1 && f.handles == handles + 1,
          "and not tried again for that stream");
    f.refuse_tiled = 0;
    f.refuse_handle = 1;
    check(cluster_decoder_feed(5, 1, f.config, sizeof(f.config), 0) == -1 && f.live_handles == 0,
          "no component: nothing decodes");
    f.refuse_handle = 0;
    check(cluster_decoder_feed(6, 1, f.config, sizeof(f.config), 0) == 0, "a later stream tries again");
    f.cb->event(&f.c, NULL, OMX_EVENT_ERROR, 0x8000100Bu, 0, NULL);
    check(cluster_decoder_feed(6, 0, frame, sizeof(frame), 0) == 0 && g.errors == 1,
          "a decode error is counted and decoding goes on");
    f.cb->event(&f.c, NULL, OMX_EVENT_PORT_SETTINGS, OMX_PORT_OUT, 0, NULL);
    check(cluster_decoder_feed(6, 0, frame, sizeof(frame), 0) == -1 && f.live_handles == 0 && f.allocated == f.freed,
          "the output changing under it closes the decoder for that stream");

    {
        uint8_t sps[160];
        size_t n;
        int matrix, range;
        n = sps_plain(sps);
        range = h264_sps_range(sps, (uint32_t)n, &matrix);
        check(range == -1 && matrix == -1, "an SPS without VUI signals no range (%d)", range);
        n = sps_high(sps, 1, 1);
        check(memmem(sps, n, "\x00\x00\x03", 3) != NULL, "the high-profile SPS needs emulation prevention");
        range = h264_sps_range(sps, (uint32_t)n, &matrix);
        check(range == 1 && matrix == 1, "high profile past scaling lists, poc type 1, fields and an extended "
              "aspect ratio: full range, matrix 1 (%d, %d)", range, matrix);
        n = sps_high(sps, 0, -1);
        range = h264_sps_range(sps, (uint32_t)n, &matrix);
        check(range == 0 && matrix == -1, "video range, no colour description (%d, %d)", range, matrix);
        check(h264_sps_range(sps, 12, &matrix) == -1, "a cut SPS signals nothing");
        check(h264_sps_range(frame, sizeof(frame), &matrix) == -1, "no SPS: nothing");

        n = sps_high(sps, 1, 1);
        check(cluster_decoder_feed(8, 1, sps, (uint32_t)n, 0) == 0 && g_range == 1, "a full-range stream's config");
        cluster_decoder_feed(8, 0, frame, sizeof(frame), 1000);
        check(copy(&serial) == 1 && target_is(f.frames, 1), "its pictures are copied as they are");
        n = sps_high(sps, 0, 1);
        cluster_decoder_feed(9, 1, sps, (uint32_t)n, 0);
        cluster_decoder_feed(9, 0, frame, sizeof(frame), 1000);
        check(g_range == 0 && copy(&serial) == 1 && target_is(f.frames, 0), "a video-range stream's are expanded");
    }
    printf("cluster_decoder_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
