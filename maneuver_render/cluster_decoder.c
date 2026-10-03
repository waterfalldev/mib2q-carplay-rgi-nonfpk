/* The cluster stream's hardware decoder (cluster_decoder.h). */
#include "cluster_decoder.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* OpenMAX IL 1.1 as the MU1329 core speaks it (stock libairplay: nVersion 0x101, a 96-byte
 * port definition on ARM).  Only what this file uses. */
#define OMX_VERSION                 0x101u
#define OMX_INDEX_PORT_DEFINITION   0x02000001u
#define OMX_COMMAND_STATE_SET       0
#define OMX_STATE_LOADED            1
#define OMX_STATE_IDLE              2
#define OMX_STATE_EXECUTING         3
#define OMX_EVENT_CMD_COMPLETE      0
#define OMX_EVENT_ERROR             1
#define OMX_EVENT_PORT_SETTINGS     3
#define OMX_FLAG_ENDOFFRAME         0x10u
#define OMX_FLAG_CODECCONFIG        0x80u
#define OMX_CODING_AVC              7u
#define OMX_PORT_IN                 0u
#define OMX_PORT_OUT                1u
#define CD_MAX_BUFFERS              32u
#define CD_INPUT_WAIT_MS            100
#define CD_PICTURE_ORDER_EXTENSION  "OMX.QCOM.index.config.video.DisplayPictureBuffer"
#define CD_TILE_4X2                 0x7F000004u  /* Qualcomm's 64 x 32 tiles (VCD TILE_4x2) */
#define CD_TILE_W                   64
#define CD_TILE_H                   32
#define CD_TILE_BYTES               (CD_TILE_W * CD_TILE_H)
#define CD_TILE_GROUP               (4 * CD_TILE_BYTES)

typedef struct {
    uint32_t size, version;
    uint8_t *buffer;
    uint32_t alloc_len, filled_len, offset;
    void *app_private, *platform_private, *input_port_private, *output_port_private;
    void *mark_target, *mark_data;
    uint32_t tick_count;
    int64_t timestamp;
    uint32_t flags, output_port, input_port;
} omx_header_t;

typedef struct {
    uint32_t size, version, port, dir, count_actual, count_min, buffer_size, enabled, populated, domain;
    struct {
        char *mime;
        void *native_render;
        uint32_t width, height;
        int32_t stride;
        uint32_t slice_height, bitrate, framerate_q16, error_concealment, compression, color;
        void *native_window;
    } video;
    uint32_t contiguous, alignment;
} omx_port_t;

#ifdef __arm__                  /* the layouts stock libairplay uses (its offsets and nSize) */
typedef char cd_port_definition_is_96_bytes[sizeof(omx_port_t) == 96 ? 1 : -1];
typedef char cd_header_flags_at_64[offsetof(omx_header_t, flags) == 64 ? 1 : -1];
typedef char cd_header_timestamp_at_56[offsetof(omx_header_t, timestamp) == 56 ? 1 : -1];
#endif

typedef struct omx_component omx_component_t;
typedef int (*omx_fn_t)();
struct omx_component {
    uint32_t size, version;
    void *component_private, *application_private;
    omx_fn_t get_component_version;
    int (*send_command)(omx_component_t *, int, uint32_t, void *);
    int (*get_parameter)(omx_component_t *, uint32_t, void *);
    int (*set_parameter)(omx_component_t *, uint32_t, void *);
    omx_fn_t get_config, set_config;
    int (*get_extension_index)(omx_component_t *, const char *, uint32_t *);
    omx_fn_t get_state, component_tunnel_request, use_buffer;
    int (*allocate_buffer)(omx_component_t *, omx_header_t **, uint32_t, void *, uint32_t);
    int (*free_buffer)(omx_component_t *, uint32_t, omx_header_t *);
    int (*empty_this_buffer)(omx_component_t *, omx_header_t *);
    int (*fill_this_buffer)(omx_component_t *, omx_header_t *);
};

typedef struct {
    int (*event)(omx_component_t *, void *, int, uint32_t, uint32_t, void *);
    int (*empty_done)(omx_component_t *, void *, omx_header_t *);
    int (*fill_done)(omx_component_t *, void *, omx_header_t *);
} omx_callbacks_t;

static struct {
    int (*init)(void);
    int (*get_handle)(omx_component_t **, const char *, void *, const omx_callbacks_t *);
    int (*free_handle)(omx_component_t *);
    int loaded, failed;
} g_core;

static struct {
    omx_component_t *omx;
    uint32_t stream, given_up;
    omx_port_t in, out;
    omx_header_t *inputs[CD_MAX_BUFFERS], *outputs[CD_MAX_BUFFERS];
    unsigned n_in, n_out, fed, dropped, logged_errors;
    unsigned warned_short;
    /* Shared with the decoder's callbacks and the render thread, under `lock`. */
    omx_header_t *free_in[CD_MAX_BUFFERS], *returns[CD_MAX_BUFFERS], *newest;
    unsigned n_free_in, n_returns, serial, pictures, errors;
    int state, port_changed;
    uint32_t error;
} g;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_changed = PTHREAD_COND_INITIALIZER;

static void deadline_in(struct timespec *t, long ms) {
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) { ++t->tv_sec; t->tv_nsec -= 1000000000L; }
}

static int on_event(omx_component_t *c, void *app, int event, uint32_t d1, uint32_t d2, void *data) {
    (void)c; (void)app; (void)data;
    pthread_mutex_lock(&g_lock);
    if (event == OMX_EVENT_CMD_COMPLETE && d1 == OMX_COMMAND_STATE_SET) g.state = (int)d2;
    else if (event == OMX_EVENT_ERROR) { g.error = d1; ++g.errors; }
    else if (event == OMX_EVENT_PORT_SETTINGS) g.port_changed = 1;
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int on_empty_done(omx_component_t *c, void *app, omx_header_t *h) {
    (void)c; (void)app;
    pthread_mutex_lock(&g_lock);
    if (g.n_free_in < CD_MAX_BUFFERS) g.free_in[g.n_free_in++] = h;
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* The newest picture stays out until a newer one replaces it; the rest go back (poll). */
static int on_fill_done(omx_component_t *c, void *app, omx_header_t *h) {
    (void)c; (void)app;
    pthread_mutex_lock(&g_lock);
    if (h->filled_len && g.state == OMX_STATE_EXECUTING) {
        if (g.newest && g.n_returns < CD_MAX_BUFFERS) g.returns[g.n_returns++] = g.newest;
        g.newest = h;
        ++g.serial;
        ++g.pictures;
    } else if (g.n_returns < CD_MAX_BUFFERS) {
        g.returns[g.n_returns++] = h;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static const omx_callbacks_t g_callbacks = { on_event, on_empty_done, on_fill_done };

static int load_core(void) {
    void *lib, *symbol;
    if (g_core.loaded || g_core.failed) return g_core.loaded;
    g_core.failed = 1;
    lib = dlopen(CD_OMX_CORE, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        const char *why = dlerror();
        fprintf(stderr, "cluster_decoder: %s not loaded (%s)\n", CD_OMX_CORE ? CD_OMX_CORE : "self",
                why ? why : "?");
        return 0;
    }
    symbol = dlsym(lib, "OMX_Init");
    memcpy(&g_core.init, &symbol, sizeof(symbol));
    symbol = dlsym(lib, "OMX_GetHandle");
    memcpy(&g_core.get_handle, &symbol, sizeof(symbol));
    symbol = dlsym(lib, "OMX_FreeHandle");
    memcpy(&g_core.free_handle, &symbol, sizeof(symbol));
    if (!g_core.init || !g_core.get_handle || !g_core.free_handle || g_core.init() != 0) {
        fprintf(stderr, "cluster_decoder: the OpenMAX core did not start\n");
        return 0;
    }
    g_core.failed = 0;
    g_core.loaded = 1;
    return 1;
}

/* Waits for the state, or an error; 0 once reached. */
static int wait_state(int state) {
    struct timespec t;
    int reached;
    deadline_in(&t, CD_STATE_WAIT_MS);
    pthread_mutex_lock(&g_lock);
    while (g.state != state && pthread_cond_timedwait(&g_changed, &g_lock, &t) != ETIMEDOUT) { }
    reached = g.state == state;
    pthread_mutex_unlock(&g_lock);
    if (!reached) fprintf(stderr, "cluster_decoder: state %d not reached (still %d)\n", state, g.state);
    return reached ? 0 : -1;
}

static int port_get(omx_port_t *p, uint32_t port) {
    memset(p, 0, sizeof(*p));
    p->size = sizeof(*p);
    p->version = OMX_VERSION;
    p->port = port;
    return g.omx->get_parameter(g.omx, OMX_INDEX_PORT_DEFINITION, p);
}

/* Sets the port to the stream's size and rate - AVC in, TILE_4x2 out - with at least `count`
 * buffers and `extra` beyond the decoder's minimum, then reads back what the decoder made of
 * it into *p. */
static int port_set(omx_port_t *p, uint32_t port, uint32_t count, uint32_t extra) {
    int e;
    if ((e = port_get(p, port)) != 0) return e;
    p->video.width = CD_WIDTH;
    p->video.height = CD_HEIGHT;
    p->video.framerate_q16 = CD_FPS << 16;
    p->video.compression = port == OMX_PORT_IN ? OMX_CODING_AVC : 0;
    if (port == OMX_PORT_OUT) p->video.color = CD_TILE_4X2;
    if (count < p->count_min + extra) count = p->count_min + extra;
    if (p->count_actual < count) p->count_actual = count;
    if ((e = g.omx->set_parameter(g.omx, OMX_INDEX_PORT_DEFINITION, p)) != 0) return e;
    if ((e = port_get(p, port)) != 0) return e;
    return p->count_actual > CD_MAX_BUFFERS ? -1 : 0;
}

/* Where tile (x, y) of a TILE_4x2 plane `w` tiles wide (even) and `h` high starts, in tiles:
 * Qualcomm's 64 x 32 tiles in 2 x 2 groups, each pair of tile rows zigzagging through them;
 * an odd last row runs straight. */
static size_t tile_index(size_t x, size_t y, size_t w, size_t h) {
    size_t i = x + (y & ~(size_t)1) * w;
    if (y & 1) i += (x & ~(size_t)3) + 2;
    else if ((h & 1) == 0 || y != h - 1) i += (x + 2) & ~(size_t)3;
    return i;
}

/* A w x h TILE_4x2 picture: tile columns (even), luma and chroma tile rows, and where chroma
 * starts (luma rounded up to a whole 2 x 2 group).  1024 x 480 gives 753664 bytes; stock's
 * decoder buffer for it is 753712 (map17's system log). */
static void tile_layout(uint32_t w, uint32_t h, size_t *cols, size_t *rows, size_t *chroma_rows,
                        size_t *chroma_at) {
    *cols = (((size_t)w + CD_TILE_W - 1) / CD_TILE_W + 1) & ~(size_t)1;
    *rows = ((size_t)h + CD_TILE_H - 1) / CD_TILE_H;
    *chroma_rows = ((size_t)h / 2 + CD_TILE_H - 1) / CD_TILE_H;
    *chroma_at = (*cols * *rows * CD_TILE_BYTES + CD_TILE_GROUP - 1) / CD_TILE_GROUP * CD_TILE_GROUP;
}

static size_t tiled_bytes(uint32_t w, uint32_t h) {
    size_t cols, rows, chroma_rows, chroma_at;
    tile_layout(w, h, &cols, &rows, &chroma_rows, &chroma_at);
    return chroma_at + cols * chroma_rows * CD_TILE_BYTES;
}

/* One TILE_4x2 plane, `rows` tile rows of it, into a linear plane of cw x ch. */
static void untile_plane(const uint8_t *src, size_t cols, size_t rows, uint8_t *dst, int stride, int cw, int ch) {
    size_t tx, ty, r;
    for (ty = 0; ty < rows; ++ty)
        for (tx = 0; tx < cols && (int)(tx * CD_TILE_W) < cw; ++tx) {
            const uint8_t *tile = src + tile_index(tx, ty, cols, rows) * CD_TILE_BYTES;
            size_t x0 = tx * CD_TILE_W, y0 = ty * CD_TILE_H, n = (size_t)cw - x0 < CD_TILE_W ? (size_t)cw - x0 : CD_TILE_W;
            for (r = 0; r < CD_TILE_H && (int)(y0 + r) < ch; ++r)
                memcpy(dst + (y0 + r) * (size_t)stride + x0, tile + r * CD_TILE_W, n);
        }
}

/* Output in decode order, as stock asks: no reordering delay. */
static void decode_order(void) {
    uint32_t index = 0, p[5];
    memset(p, 0, sizeof(p));
    p[0] = sizeof(p);
    p[1] = OMX_VERSION;
    if (g.omx->get_extension_index(g.omx, CD_PICTURE_ORDER_EXTENSION, &index) != 0
            || g.omx->get_parameter(g.omx, index, p) != 0) {
        fprintf(stderr, "cluster_decoder: no decode-order mode; pictures may come later\n");
        return;
    }
    p[3] = 1;
    p[4] = 1;
    if (g.omx->set_parameter(g.omx, index, p) != 0) fprintf(stderr, "cluster_decoder: decode-order mode refused\n");
}

static void free_buffers(void) {
    unsigned i;
    for (i = 0; i < g.n_in; ++i) g.omx->free_buffer(g.omx, OMX_PORT_IN, g.inputs[i]);
    for (i = 0; i < g.n_out; ++i) g.omx->free_buffer(g.omx, OMX_PORT_OUT, g.outputs[i]);
    g.n_in = g.n_out = 0;
}

static void close_decoder(const char *why) {
    int loaded = 0;
    if (!g.omx) return;
    if (g.state == OMX_STATE_EXECUTING) {
        g.omx->send_command(g.omx, OMX_COMMAND_STATE_SET, OMX_STATE_IDLE, NULL);
        wait_state(OMX_STATE_IDLE);
    }
    pthread_mutex_lock(&g_lock);                /* the render thread copies no more */
    g.newest = NULL;
    g.n_returns = g.n_free_in = 0;
    pthread_mutex_unlock(&g_lock);
    if (g.state == OMX_STATE_IDLE)
        loaded = g.omx->send_command(g.omx, OMX_COMMAND_STATE_SET, OMX_STATE_LOADED, NULL) == 0;
    free_buffers();
    if (loaded) wait_state(OMX_STATE_LOADED);
    g_core.free_handle(g.omx);
    g.omx = NULL;
    fprintf(stderr, "cluster_decoder: stream %u decoder closed (%s): %u units in, %u pictures, %u dropped, "
            "%u errors\n", g.stream, why, g.fed, g.pictures, g.dropped, g.errors);
}

static int open_decoder(uint32_t stream) {
    uint32_t i;
    int e;
    if (!load_core()) return -1;
    pthread_mutex_lock(&g_lock);
    g.state = OMX_STATE_LOADED;
    g.port_changed = 0;
    g.error = 0;
    g.errors = g.pictures = 0;
    pthread_mutex_unlock(&g_lock);
    g.stream = stream;
    g.fed = g.dropped = g.logged_errors = 0;
    g.warned_short = 0;
    if ((e = g_core.get_handle(&g.omx, CD_COMPONENT, NULL, &g_callbacks)) != 0 || !g.omx) {
        fprintf(stderr, "cluster_decoder: %s not available (0x%x)\n", CD_COMPONENT, (unsigned)e);
        g.omx = NULL;
        return -1;
    }
    decode_order();
    if ((e = port_set(&g.in, OMX_PORT_IN, CD_INPUT_BUFFERS, 0)) != 0) {
        fprintf(stderr, "cluster_decoder: input port refused (0x%x)\n", (unsigned)e);
        goto fail;
    }
    if ((e = port_set(&g.out, OMX_PORT_OUT, 0, CD_OUTPUT_EXTRA)) != 0 || g.out.video.color != CD_TILE_4X2
            || g.out.buffer_size < tiled_bytes(g.out.video.width, g.out.video.height)) {
        fprintf(stderr, "cluster_decoder: output port refused (0x%x): colour 0x%x, %ux%u in %u B\n", (unsigned)e,
                g.out.video.color, g.out.video.width, g.out.video.height, g.out.buffer_size);
        goto fail;
    }
    if (g.omx->send_command(g.omx, OMX_COMMAND_STATE_SET, OMX_STATE_IDLE, NULL) != 0) goto fail;
    for (i = 0; i < g.in.count_actual; ++i, ++g.n_in)
        if (g.omx->allocate_buffer(g.omx, &g.inputs[i], OMX_PORT_IN, NULL, g.in.buffer_size) != 0) goto fail;
    for (i = 0; i < g.out.count_actual; ++i, ++g.n_out)
        if (g.omx->allocate_buffer(g.omx, &g.outputs[i], OMX_PORT_OUT, NULL, g.out.buffer_size) != 0) goto fail;
    if (wait_state(OMX_STATE_IDLE) != 0) goto fail;
    if (g.omx->send_command(g.omx, OMX_COMMAND_STATE_SET, OMX_STATE_EXECUTING, NULL) != 0
            || wait_state(OMX_STATE_EXECUTING) != 0) goto fail;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g.n_in; ++i) g.free_in[i] = g.inputs[i];
    g.n_free_in = g.n_in;
    pthread_mutex_unlock(&g_lock);
    for (i = 0; i < g.n_out; ++i) g.omx->fill_this_buffer(g.omx, g.outputs[i]);
    fprintf(stderr, "cluster_decoder: stream %u decoding %ux%u: in %u x %u B, out %u x %u B\n", stream,
            g.out.video.width, g.out.video.height, g.in.count_actual, g.in.buffer_size, g.out.count_actual,
            g.out.buffer_size);
    return 0;
fail:
    close_decoder("set-up failed");
    return -1;
}

void cluster_decoder_poll(void) {
    omx_header_t *back[CD_MAX_BUFFERS];
    unsigned n, i, errors;
    uint32_t error;
    int changed;
    if (!g.omx) return;
    pthread_mutex_lock(&g_lock);
    n = g.n_returns;
    memcpy(back, g.returns, n * sizeof(back[0]));
    g.n_returns = 0;
    errors = g.errors;
    error = g.error;
    changed = g.port_changed;
    pthread_mutex_unlock(&g_lock);
    for (i = 0; i < n; ++i) g.omx->fill_this_buffer(g.omx, back[i]);
    if (errors != g.logged_errors && (g.logged_errors < 5 || errors - g.logged_errors >= 100)) {
        fprintf(stderr, "cluster_decoder: stream %u decoder error 0x%x (%u so far)\n", g.stream, error, errors);
        g.logged_errors = errors;
    }
    if (changed) {
        omx_port_t now;
        port_get(&now, OMX_PORT_OUT);
        fprintf(stderr, "cluster_decoder: output changed to %ux%u colour 0x%x\n", now.video.width, now.video.height,
                now.video.color);
        g.given_up = g.stream;
        close_decoder("output changed");
    }
}

int cluster_decoder_feed(uint32_t stream, int config, const uint8_t *unit, uint32_t bytes, uint64_t time_ns) {
    omx_header_t *in = NULL;
    struct timespec t;
    if (g.omx && stream != g.stream) close_decoder("new stream");
    if (!g.omx) {
        if (!config || stream == g.given_up) return -1;
        if (open_decoder(stream) != 0) {
            g.given_up = stream;
            return -1;
        }
    }
    cluster_decoder_poll();
    if (!g.omx) return -1;
    deadline_in(&t, CD_INPUT_WAIT_MS);
    pthread_mutex_lock(&g_lock);
    while (!g.n_free_in && pthread_cond_timedwait(&g_changed, &g_lock, &t) != ETIMEDOUT) { }
    if (g.n_free_in) in = g.free_in[--g.n_free_in];
    pthread_mutex_unlock(&g_lock);
    if (!in || bytes > in->alloc_len) {
        if (g.dropped++ < 5) fprintf(stderr, "cluster_decoder: stream %u unit of %u B dropped (%s)\n", stream, bytes,
                                     in ? "larger than the input buffers" : "no input buffer free");
        if (in) on_empty_done(g.omx, NULL, in);
        return 1;
    }
    memcpy(in->buffer, unit, bytes);
    in->filled_len = bytes;
    in->offset = 0;
    in->flags = config ? OMX_FLAG_CODECCONFIG : OMX_FLAG_ENDOFFRAME;
    in->timestamp = (int64_t)(time_ns / 1000u);
    if (g.omx->empty_this_buffer(g.omx, in) != 0) {
        on_empty_done(g.omx, NULL, in);
        ++g.dropped;
        return 1;
    }
    ++g.fed;
    return 0;
}

void cluster_decoder_close(void) {
    close_decoder("stream idle");
    g.given_up = 0;
}

int cluster_decoder_copy(unsigned *serial, uint8_t *y, int y_stride, uint8_t *uv, int uv_stride, int w, int h) {
    int copied = 0;
    pthread_mutex_lock(&g_lock);
    if (g.newest && g.serial != *serial) {
        const uint32_t fw = g.out.video.width, fh = g.out.video.height;
        const int cw = w < (int)fw ? w : (int)fw, ch = h < (int)fh ? h : (int)fh;
        size_t cols, rows, chroma_rows, chroma_at;
        tile_layout(fw, fh, &cols, &rows, &chroma_rows, &chroma_at);
        if (g.newest->filled_len >= tiled_bytes(fw, fh)) {
            const uint8_t *src = g.newest->buffer + g.newest->offset;
            untile_plane(src, cols, rows, y, y_stride, cw, ch);
            untile_plane(src + chroma_at, cols, chroma_rows, uv, uv_stride, cw, ch / 2);
            copied = 1;
        } else if (!g.warned_short++) {
            fprintf(stderr, "cluster_decoder: a %u-byte picture, short of TILE_4x2 %ux%u; not shown\n",
                    g.newest->filled_len, fw, fh);
        }
        *serial = g.serial;
    }
    pthread_mutex_unlock(&g_lock);
    return copied;
}

int cluster_decoder_fresh(unsigned serial) {
    int fresh;
    pthread_mutex_lock(&g_lock);
    fresh = g.newest && g.serial != serial;
    pthread_mutex_unlock(&g_lock);
    return fresh;
}
