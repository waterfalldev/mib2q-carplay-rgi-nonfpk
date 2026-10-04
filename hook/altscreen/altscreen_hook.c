/* altScreen - the phone's second CarPlay display for the MOST MAP view (altscreen_hook.h). */
#include "altscreen_hook.h"
#include "../framework/cflite.h"
#include "cluster_video_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOG_MODULE "altscreen"

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- /info: the cluster display, one view area covering it, safe where the dials are not */

static void set_frame(const cflite_t *cf, cf_ref_t dict, int64_t x, int64_t y, int64_t w, int64_t h) {
    cf_set_int64(cf, dict, "widthPixels", w);
    cf_set_int64(cf, dict, "heightPixels", h);
    cf_set_int64(cf, dict, "originXPixels", x);
    cf_set_int64(cf, dict, "originYPixels", y);
}

static cf_ref_t cluster_display(const cflite_t *cf) {
    cf_ref_t display = cf_dictionary(cf), areas = cf_array(cf), area = cf_dictionary(cf), safe = cf_dictionary(cf);
    if (!display || !areas || !area || !safe) {
        cf_release(cf, display);
        cf_release(cf, areas);
        cf_release(cf, area);
        cf_release(cf, safe);
        return NULL;
    }
    set_frame(cf, area, 0, 0, ALT_WIDTH, ALT_HEIGHT);
    set_frame(cf, safe, ALT_SAFE_X, ALT_SAFE_Y, ALT_SAFE_WIDTH, ALT_SAFE_HEIGHT);
    cf_set(cf, safe, "drawUIOutsideSafeArea", cf->boolean_false);
    cf_set(cf, area, "viewAreaTransitionControl", cf->boolean_false);
    cf_set(cf, area, "safeArea", safe);
    cf->release(safe);
    cf->array_append_value(areas, area);
    cf->release(area);
    cf_set_cstring(cf, display, "uuid", ALT_DISPLAY_UUID);
    cf_set_int64(cf, display, "type", ALT_STREAM_TYPE);
    cf_set_int64(cf, display, "maxFPS", ALT_MAX_FPS);
    cf_set_int64(cf, display, "widthPixels", ALT_WIDTH);
    cf_set_int64(cf, display, "heightPixels", ALT_HEIGHT);
    cf_set_int64(cf, display, "widthPhysical", (ALT_WIDTH + ALT_PIXELS_PER_MM / 2) / ALT_PIXELS_PER_MM);
    cf_set_int64(cf, display, "heightPhysical", (ALT_HEIGHT + ALT_PIXELS_PER_MM / 2) / ALT_PIXELS_PER_MM);
    cf_set_int64(cf, display, "features", ALT_DISPLAY_FEATURES);
    cf_set_int64(cf, display, "primaryInputDevice", ALT_PRIMARY_INPUT);
    cf_set_cstring(cf, display, "initialURL", ALT_DISPLAY_URL);
    cf_set(cf, display, "viewAreas", areas);
    cf->release(areas);
    cf_set_int64(cf, display, "initialViewArea", 0);
    return display;
}

static void altscreen_on_server_info(void *session, void **info) {
    const cflite_t *cf = cflite();
    cf_ref_t copy, displays, extended = NULL, display = NULL, data;
    int64_t features;
    int ok;
    (void)session;
    if (!cf || !(copy = cf->plist_create_deep_copy(NULL, *info, CF_PLIST_MUTABLE_ALL))) return;
    if ((displays = cf_get(cf, copy, "displays"))) {
        extended = cf->array_create_mutable_copy(NULL, 0, displays);
        display = cluster_display(cf);
    }
    if (!extended || !display) {
        LOG_WARN(LOG_MODULE, "/info has no displays to extend; the cluster display is not advertised");
        cf_release(cf, extended);
        cf_release(cf, display);
        cf->release(copy);
        return;
    }
    cf->array_append_value(extended, display);
    cf->release(display);
    cf_set(cf, copy, "displays", extended);
    cf->release(extended);
    features = cf_get_int64(cf, copy, "features", &ok);
    if (ok) cf_set_int64(cf, copy, "features", features | ALT_FEATURES_BIT);
    /* Stock fails the whole connection on an /info it cannot serialize (map04: 500), so such a
     * copy is dropped and stock's goes out. */
    if (!(data = cf->plist_create_data(NULL, copy, CF_PLIST_BINARY_FORMAT, 0, NULL))) {
        LOG_WARN(LOG_MODULE, "extended /info cannot be serialized; stock's is sent unchanged");
        cf->release(copy);
        return;
    }
    cf->release(data);
    cf->release(*info);
    *info = (void *)copy;
    LOG_WARN(LOG_MODULE, "/info advertises the cluster display %dx%d (type %d, %s)",
             ALT_WIDTH, ALT_HEIGHT, ALT_STREAM_TYPE, ALT_DISPLAY_URL);
}

/* ---- The stream's keys: stock derives a screen stream's AES-128-CTR key and IV from the
 * session's master key (only ever handed over through SetSecurityInfo, hence the seam) and
 * the stream's connection id.  One CarPlay session per process. */

static pthread_mutex_t g_key_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_master_key[16];
static uint64_t g_connection_id;
static int g_connection_id_set;

static void altscreen_on_security_info(void *session, const uint8_t *key, const uint8_t *iv) {
    (void)session;
    (void)iv;
    pthread_mutex_lock(&g_key_lock);
    memcpy(g_master_key, key, sizeof(g_master_key));
    pthread_mutex_unlock(&g_key_lock);
    LOG_WARN(LOG_MODULE, "session key set: the cluster stream can be decrypted");
}

static int is_cluster_stream(const cflite_t *cf, cf_ref_t stream) {
    int ok;
    return cf_get_int64(cf, stream, "type", &ok) == ALT_STREAM_TYPE && ok;
}

/* A SETUP mixing the cluster stream with others reaches stock as a copy without it (the seam
 * releases the copy); stock ignores a cluster-only SETUP and succeeds. */
static void altscreen_on_setup_request(hook_setup_ctx_t *setup) {
    const cflite_t *cf;
    cf_ref_t streams, kept, copy;
    long i, count, cluster = 0;
    if (!setup->request || !(cf = cflite())) return;
    streams = cf_get(cf, setup->request, "streams");
    if (!streams || (count = cf->array_get_count(streams)) <= 0 || !(kept = cf_array(cf))) return;
    for (i = 0; i < count; ++i) {
        cf_ref_t stream = cf->array_get_value_at_index(streams, i);
        int ok;
        int64_t id;
        if (!is_cluster_stream(cf, stream)) {
            cf->array_append_value(kept, stream);
        } else if (!cluster++) {
            id = cf_get_int64(cf, stream, "streamConnectionID", &ok);
            pthread_mutex_lock(&g_key_lock);
            g_connection_id = (uint64_t)id;
            g_connection_id_set = ok;
            pthread_mutex_unlock(&g_key_lock);
        }
    }
    if (cluster && cluster < count && (copy = cf->plist_create_deep_copy(NULL, setup->request, CF_PLIST_MUTABLE_ALL))) {
        cf_set(cf, copy, "streams", kept);
        setup->request = (void *)copy;
    }
    cf->release(kept);
}

/* ---- The session the stream belongs to, retained from the SETUP reply that gave the stream
 * its port, so the phone can be asked for key frames.  Stock is never called under a lock of
 * ours nor from the receiver thread, and the last release never happens on stock's thread
 * (docs/deploy/session-lifecycle.md).  The reference goes when stock tears the session down
 * (a pulled cable never closes the connection: map14), its connection ends, a new phone
 * session starts (Identify) or no connection comes within ALT_HOLD_CONNECT_MS. */

static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_session;
static void *g_session_replaced;    /* replaced by a newer SETUP: the receiver lets it go */
static uint32_t g_session_hold;     /* one per held session */
static uint64_t g_session_since;    /* when it was held, until its connection came (then 0) */
static uint32_t g_end_hold;         /* a session to end, recorded on stock's thread */
static const char *g_end_why;

static void hold_session(const cflite_t *cf, void *session) {
    void *release = NULL;
    cf->retain(session);
    pthread_mutex_lock(&g_session_lock);
    if (g_session == session || g_session_replaced) release = g_session;   /* ours still held, or two in a row */
    else g_session_replaced = g_session;
    g_session = session;
    ++g_session_hold;
    g_session_since = now_ns();
    pthread_mutex_unlock(&g_session_lock);
    if (release) cf->release(release);
}

static void release_session(void *session, const char *why) {
    const cflite_t *cf = cflite();
    if (!session || !cf) return;
    LOG_WARN(LOG_MODULE, "cluster stream's session released (%s)", why);
    (void)why;
    cf->release(session);
}

/* Receiver, outside the lock: a replaced session goes, and the held one if it is still
 * `hold`'s (0: the current one) - with `expired`, only one held that long with no connection. */
static void drop_session(uint32_t hold, int expired, const char *why) {
    void *session = NULL, *replaced;
    pthread_mutex_lock(&g_session_lock);
    if (!hold) hold = g_session_hold;
    if (g_session && g_session_hold == hold
            && (!expired || (g_session_since && now_ns() - g_session_since >= ALT_HOLD_CONNECT_MS * 1000000ull))) {
        session = g_session;
        g_session = NULL;
    }
    replaced = g_session_replaced;
    g_session_replaced = NULL;
    pthread_mutex_unlock(&g_session_lock);
    release_session(replaced, "a newer SETUP replaced it");
    release_session(session, why);
}

static void end_session_soon(uint32_t hold, const char *why) {
    g_end_hold = hold;
    g_end_why = why;
}

static void altscreen_on_state(hook_context_t *ctx, int event, void *data) {
    (void)ctx;
    (void)data;
    if (event != HOOK_EVENT_IDENTIFY_START) return;
    pthread_mutex_lock(&g_session_lock);
    end_session_soon(g_session_hold, "a new phone session started");
    pthread_mutex_unlock(&g_session_lock);
}

/* Stock's teardown of the whole session (the phone also tears single streams down). */
static void altscreen_on_session_teardown(void *session, int whole) {
    if (!whole || !session) return;
    pthread_mutex_lock(&g_session_lock);
    if (session == g_session) end_session_soon(g_session_hold, "stock tore its session down");
    pthread_mutex_unlock(&g_session_lock);
}

static uint32_t take_end(const char **why) {
    uint32_t hold;
    pthread_mutex_lock(&g_session_lock);
    hold = g_end_hold;
    *why = g_end_why;
    g_end_hold = 0;
    pthread_mutex_unlock(&g_session_lock);
    return hold;
}

/* ---- Commands to the phone (stock AirPlayReceiverSessionSendCommand, as the reference
 * receivers send them): {type, params: {uuid[, url | zoomDirection | nightMode]}}.  The phone
 * starts the cluster UI and stream only after showUI (map07); forceKeyFrame brings a new avcC
 * record and an IDR frame (map11); changeMapZoomLevel zooms the cluster map one step,
 * zoomDirection 0 in and 1 out (the reference sends one per wheel step, map23 confirmed);
 * setNightMode is stock's own command (AirPlayReceiverSessionSetNightMode, nightMode a
 * boolean), which stock sends without a uuid and the cluster map does not follow (map23) -
 * with the cluster's uuid it changes that display's theme (yuedizhibo/MHI2Q-CarPlay-AltScreen
 * issue 25).  Each batch runs on its own thread with its own session reference. */

typedef void (*command_done_fn)(int status, cf_ref_t response, void *context);
typedef int (*send_command_fn)(void *session, cf_ref_t command, command_done_fn done, void *context);
/* `zoom`: signed wheel steps, positive out.  `night`: setNightMode's value, -1 none; a showUI
 * batch sends the MMI's at the time instead. */
typedef struct { void *session; int show_ui, keyframe, zoom, night; } alt_commands_t;

/* The MMI's night mode as Java last reported it (CMD_ALT_APPEARANCE), -1 until then. */
static int g_night = -1;

static int mmi_night(void) { return __sync_fetch_and_add(&g_night, 0); }

static void command_done(int status, cf_ref_t response, void *context) {
    (void)response;
    LOG_WARN(LOG_MODULE, "%s answered: status %d", (const char *)context, status);
    (void)status;
    (void)context;
}

/* `zoom_direction`, `night` < 0: none. */
static void send_command(const cflite_t *cf, send_command_fn send, void *session, const char *type,
                         const char *url, int zoom_direction, int night) {
    cf_ref_t command = cf_dictionary(cf), params = cf_dictionary(cf);
    int status = -1;
    if (command && params) {
        cf_set_cstring(cf, params, "uuid", ALT_DISPLAY_UUID);
        if (url) cf_set_cstring(cf, params, "url", url);
        if (zoom_direction >= 0) cf_set_int64(cf, params, "zoomDirection", zoom_direction);
        if (night >= 0) cf_set(cf, params, "nightMode", night ? cf->boolean_true : cf->boolean_false);
        cf_set_cstring(cf, command, "type", type);
        cf_set(cf, command, "params", params);
        status = send(session, command, command_done, (void *)type);
    }
    LOG_WARN(LOG_MODULE, "%s for the cluster display sent (status %d)", type, status);
    (void)status;
    cf_release(cf, params);
    cf_release(cf, command);
}

static void *commands_thread(void *arg) {
    alt_commands_t *c = (alt_commands_t *)arg;
    const cflite_t *cf = cflite();
    send_command_fn send;
    int i, night = c->night;
    if (c->show_ui) {
        struct timespec delay = { ALT_SHOW_UI_DELAY_MS / 1000, (ALT_SHOW_UI_DELAY_MS % 1000) * 1000000L };
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    }
    *(void **)&send = dlsym(RTLD_DEFAULT, "AirPlayReceiverSessionSendCommand");
    if (!send) {
        LOG_WARN(LOG_MODULE, "cluster display commands not sent: AirPlayReceiverSessionSendCommand unresolved");
    } else {
        if (c->show_ui) {
            send_command(cf, send, c->session, "showUI", ALT_DISPLAY_URL, -1, -1);
            night = mmi_night();
            if (night >= 0) {
                /* The phone answered day mode with status 0 and kept the cluster map dark
                 * (map24): a value it already holds is no change.  The opposite first makes
                 * the MMI's one a real switch. */
                struct timespec gap = { ALT_NIGHT_TOGGLE_MS / 1000, (ALT_NIGHT_TOGGLE_MS % 1000) * 1000000L };
                send_command(cf, send, c->session, "setNightMode", NULL, -1, !night);
                while (nanosleep(&gap, &gap) != 0 && errno == EINTR) { }
            }
        }
        if (night >= 0) send_command(cf, send, c->session, "setNightMode", NULL, -1, night);
        if (c->keyframe) send_command(cf, send, c->session, "forceKeyFrame", NULL, -1, -1);
        for (i = 0; i < (c->zoom < 0 ? -c->zoom : c->zoom); ++i)
            send_command(cf, send, c->session, "changeMapZoomLevel", NULL, c->zoom > 0, -1);
    }
    cf->release(c->session);
    free(c);
    return NULL;
}

/* Takes over a session reference the caller holds. */
static void send_commands_soon(const cflite_t *cf, void *session, int show_ui, int keyframe, int zoom, int night) {
    alt_commands_t *c = (alt_commands_t *)malloc(sizeof(*c));
    pthread_t thread;
    if (c) {
        c->session = session;
        c->show_ui = show_ui;
        c->keyframe = keyframe;
        c->zoom = zoom;
        c->night = night;
        if (pthread_create(&thread, NULL, commands_thread, c) == 0) {
            pthread_detach(thread);
            return;
        }
        free(c);
    }
    LOG_WARN(LOG_MODULE, "cluster display command thread failed errno=%d", errno);
    cf->release(session);
}

/* The held session, retained, or NULL. */
static void *held_session(const cflite_t *cf) {
    void *session = NULL;
    pthread_mutex_lock(&g_session_lock);
    if (g_session) session = (void *)cf->retain(g_session);
    pthread_mutex_unlock(&g_session_lock);
    return session;
}

static void request_keyframe(void) {
    const cflite_t *cf = cflite();
    void *session = cf ? held_session(cf) : NULL;
    if (session) send_commands_soon(cf, session, 0, 1, 0, -1);
}

/* CMD_ALT_ZOOM (bus thread): [int8 MapScale steps, positive out], from Java while the MAP view
 * shows the phone's map. */
static void on_zoom(uint16_t type, uint8_t flags, const uint8_t *payload, uint32_t len, void *ctx) {
    const cflite_t *cf = cflite();
    void *session;
    int steps = len >= 1 ? (int8_t)payload[0] : 0;
    (void)type;
    (void)flags;
    (void)ctx;
    if (!steps || !cf) return;
    if (steps > ALT_ZOOM_MAX_STEPS) steps = ALT_ZOOM_MAX_STEPS;
    if (steps < -ALT_ZOOM_MAX_STEPS) steps = -ALT_ZOOM_MAX_STEPS;
    if (!(session = held_session(cf))) {
        LOG_WARN(LOG_MODULE, "wheel zoom %d not sent: no cluster stream session", steps);
        return;
    }
    LOG_WARN(LOG_MODULE, "wheel zoom %d: changeMapZoomLevel %s", steps, steps > 0 ? "out" : "in");
    send_commands_soon(cf, session, 0, 0, steps, -1);
}

/* CMD_ALT_APPEARANCE (bus thread): [u8 night], the MMI's night mode - at CarPlay start and on
 * every change.  Kept for the next showUI; sent at once while a session is held. */
static void on_appearance(uint16_t type, uint8_t flags, const uint8_t *payload, uint32_t len, void *ctx) {
    const cflite_t *cf = cflite();
    void *session;
    int night;
    (void)type;
    (void)flags;
    (void)ctx;
    if (len < 1) return;
    night = payload[0] != 0;
    __sync_lock_test_and_set(&g_night, night);
    if (!cf || !(session = held_session(cf))) {
        LOG_WARN(LOG_MODULE, "MMI night mode %d: kept for the cluster display's showUI", night);
        return;
    }
    LOG_WARN(LOG_MODULE, "MMI night mode %d: setNightMode for the cluster display", night);
    send_commands_soon(cf, session, 0, 0, 0, night);
}

static void altscreen_init(void) {
    bus_on(CMD_ALT_ZOOM, on_zoom, NULL);
    bus_on(CMD_ALT_APPEARANCE, on_appearance, NULL);
}

static void altscreen_shutdown(void) {
    bus_off(CMD_ALT_APPEARANCE);
    bus_off(CMD_ALT_ZOOM);
}

/* ---- One connection of the stream, in stock's screen framing (map11): a 128-byte header
 * (payload size little-endian, type in byte 4), then the payload.  Type 0 is a video frame,
 * AES-CTR encrypted with one keystream across the connection, its NAL units each behind a
 * big-endian length; type 1 the avcC record; the rest is plain and ignored. */

#define ALT_HEADER_SIZE 128
#define ALT_MAX_UNITS 64

typedef void (*derive_screen_key_fn)(const void *master, size_t length, uint64_t connection_id,
                                     uint8_t key[16], uint8_t iv[16]);   /* void: map12 */
typedef int (*ctr_init_fn)(void *context, const uint8_t key[16], const uint8_t iv[16]);
typedef int (*ctr_update_fn)(void *context, const void *in, size_t length, void *out);
typedef void (*ctr_final_fn)(void *context);

typedef struct {
    int fd;
    uint32_t hold;                  /* the held session it belongs to */
    uint32_t stream;                /* its stream in the frame ring; 0 without the ring */
    int live;                       /* its frames decrypt to H.264: published to Java */
    long long total;
    long frames;
    int dropped;
    uint8_t header[ALT_HEADER_SIZE];
    size_t header_fill;
    uint8_t *payload;
    size_t payload_size, payload_fill, payload_capacity;
    int framed;                     /* 0 once the framing is lost: the rest is only drained */
    int crypto;                     /* 1 ready, -1 unavailable, 0 not yet set up */
    uint64_t context[64];           /* stock's AES_CTR_Context (284 bytes in this libairplay) */
    ctr_update_fn ctr_update;
    ctr_final_fn ctr_final;
    int length_size;                /* bytes before each NAL unit, from the avcC record */
} alt_rx_t;

/* The frame ring, written only by the receiver thread; set up at the first connection. */
static cvr_writer_t g_ring;
static int g_ring_ready;

static int ring_ready(void) {
    if (!g_ring_ready && cvr_writer_open(&g_ring, (uint32_t)now_ns()) == 0) {
        g_ring_ready = 1;
        LOG_WARN(LOG_MODULE, "cluster video ring %s ready for the renderer", CVR_SHM_NAME);
    } else if (!g_ring_ready) {
        LOG_WARN(LOG_MODULE, "cluster video ring %s unavailable errno=%d", CVR_SHM_NAME, errno);
    }
    return g_ring_ready;
}

/* One access unit into the ring, each NAL unit behind a 4-byte start code. */
static void ring_write(alt_rx_t *rx, const uint8_t *const *units, const size_t *lengths, int count, uint32_t flags) {
    static const uint8_t start_code[4] = { 0, 0, 0, 1 };
    uint32_t total = 0, start;
    uint8_t *p;
    int i;
    if (!rx->stream) return;
    for (i = 0; i < count; ++i) total += (uint32_t)(sizeof(start_code) + lengths[i]);
    if (!(p = cvr_writer_begin(&g_ring, total, &start))) {
        if (!rx->dropped++) LOG_WARN(LOG_MODULE, "cluster stream: a %u-byte access unit does not fit the frame ring", total);
        return;
    }
    for (i = 0; i < count; ++i) {
        memcpy(p, start_code, sizeof(start_code));
        memcpy(p + sizeof(start_code), units[i], lengths[i]);
        p += sizeof(start_code) + lengths[i];
    }
    cvr_writer_commit(&g_ring, start, total, flags, now_ns());
}

/* EVT_CLUSTER_VIDEO (sticky): Java takes the MAP view during guidance while it is live. */
static void publish_live(alt_rx_t *rx, int live) {
    uint8_t buf[128];
    bus_text_builder_t b;
    rx->live = live;
    bus_text_begin_with(&b, "clustervideo", buf, sizeof(buf));
    bus_text_bool(&b, "live", live != 0);
    bus_text_uint(&b, "stream", rx->stream);
    bus_send_text(EVT_CLUSTER_VIDEO, BUS_FLAG_STICKY, &b);
    LOG_WARN(LOG_MODULE, "cluster video %s (ring stream %u)", live ? "live" : "ended", rx->stream);
}

static void rx_crypto_start(alt_rx_t *rx) {
    derive_screen_key_fn derive;
    ctr_init_fn init;
    uint8_t master[16], key[16] = { 0 }, iv[16] = { 0 };
    uint64_t id;
    int have_id;
    rx->crypto = -1;
    *(void **)&derive = dlsym(RTLD_DEFAULT, "AirPlay_DeriveAESKeySHA512ForScreen");
    *(void **)&init = dlsym(RTLD_DEFAULT, "AES_CTR_Init");
    *(void **)&rx->ctr_update = dlsym(RTLD_DEFAULT, "AES_CTR_Update");
    *(void **)&rx->ctr_final = dlsym(RTLD_DEFAULT, "AES_CTR_Final");
    pthread_mutex_lock(&g_key_lock);
    memcpy(master, g_master_key, sizeof(master));
    id = g_connection_id;
    have_id = g_connection_id_set;
    pthread_mutex_unlock(&g_key_lock);
    if (!derive || !init || !rx->ctr_update || !rx->ctr_final || !have_id) {
        LOG_WARN(LOG_MODULE, "cluster stream not decrypted: %s", have_id ? "stock's functions unresolved"
                                                                         : "no streamConnectionID");
        return;
    }
    derive(master, sizeof(master), id, key, iv);
    if (init(rx->context, key, iv) == 0) rx->crypto = 1;
    memset(master, 0, sizeof(master));
    memset(key, 0, sizeof(key));
    memset(iv, 0, sizeof(iv));
    LOG_WARN(LOG_MODULE, "cluster stream decryption %s", rx->crypto > 0 ? "set up" : "failed: AES_CTR_Init");
}

/* The NAL units of a frame, each behind a `length_size`-byte big-endian length: their count,
 * or -1 unless they fill it exactly with valid headers (wrongly decrypted data practically
 * never does) and number at most ALT_MAX_UNITS. */
static int split_units(const uint8_t *p, size_t n, int length_size, const uint8_t **units, size_t *lengths) {
    size_t at = 0;
    int count = 0;
    while (at < n) {
        size_t length = 0;
        int i;
        if (n - at <= (size_t)length_size || count == ALT_MAX_UNITS) return -1;
        for (i = 0; i < length_size; ++i) length = (length << 8) | p[at + i];
        at += (size_t)length_size;
        if (length == 0 || length > n - at || (p[at] & 0x80) || (p[at] & 0x1f) == 0 || (p[at] & 0x1f) > 23)
            return -1;
        units[count] = p + at;
        lengths[count++] = length;
        at += length;
    }
    return count;
}

/* avcC: version 1, profile, compatibility, level, length size, then the SPS and PPS sets. */
static void on_codec(alt_rx_t *rx, const uint8_t *p, size_t n) {
    const uint8_t *units[32];
    size_t lengths[32], at = 6;
    int count = 0, set, i;
    if (n < 7 || p[0] != 1) return;
    rx->length_size = (p[4] & 3) + 1;
    for (set = 0; set < 2 && at < n; ++set) {
        int sets = set ? p[at++] : (p[5] & 0x1f);
        for (i = 0; i < sets; ++i) {
            size_t length;
            if (at + 2 > n || (length = ((size_t)p[at] << 8) | p[at + 1]) == 0 || at + 2 + length > n || count == 32) {
                LOG_WARN(LOG_MODULE, "cluster stream avcC record is truncated");
                return;
            }
            units[count] = p + at + 2;
            lengths[count++] = length;
            at += 2 + length;
        }
    }
    ring_write(rx, units, lengths, count, CVR_CONFIG);
}

static void on_frame(alt_rx_t *rx, uint8_t *p, size_t n) {
    const uint8_t *units[ALT_MAX_UNITS];
    size_t lengths[ALT_MAX_UNITS];
    uint32_t flags = CVR_FRAME;
    int count, i;
    if (!rx->crypto) rx_crypto_start(rx);
    if (rx->crypto < 0 || rx->ctr_update(rx->context, p, n, p) != 0) return;
    count = split_units(p, n, rx->length_size, units, lengths);
    if (!rx->frames++)
        LOG_WARN(LOG_MODULE, "cluster stream decrypted: its first frame is %sH.264 (%d NAL units)",
                 count > 0 ? "" : "not ", count);
    if (count <= 0) return;
    for (i = 0; i < count; ++i)
        if ((units[i][0] & 0x1f) == 5) flags |= CVR_KEY;     /* IDR slice */
    ring_write(rx, units, lengths, count, flags);
    if (!rx->live && rx->stream) publish_live(rx, 1);
}

/* Feeds the next bytes of the connection, as they arrive. */
static void rx_feed(alt_rx_t *rx, const uint8_t *p, size_t n) {
    while (n > 0 && rx->framed) {
        size_t take;
        if (rx->header_fill < ALT_HEADER_SIZE) {
            take = ALT_HEADER_SIZE - rx->header_fill < n ? ALT_HEADER_SIZE - rx->header_fill : n;
            memcpy(rx->header + rx->header_fill, p, take);
            rx->header_fill += take;
            p += take;
            n -= take;
            if (rx->header_fill < ALT_HEADER_SIZE) break;
            rx->payload_size = (size_t)rx->header[0] | ((size_t)rx->header[1] << 8)
                             | ((size_t)rx->header[2] << 16) | ((size_t)rx->header[3] << 24);
            rx->payload_fill = 0;
            if (rx->payload_size > rx->payload_capacity) {
                uint8_t *grown = rx->payload_size <= ALT_PACKET_MAX ? (uint8_t *)realloc(rx->payload, rx->payload_size) : NULL;
                if (!grown) {
                    LOG_WARN(LOG_MODULE, "cluster stream: a %lu-byte packet; the rest is only drained",
                             (unsigned long)rx->payload_size);
                    rx->framed = 0;
                    break;
                }
                rx->payload = grown;
                rx->payload_capacity = rx->payload_size;
            }
        } else {
            take = rx->payload_size - rx->payload_fill < n ? rx->payload_size - rx->payload_fill : n;
            memcpy(rx->payload + rx->payload_fill, p, take);
            rx->payload_fill += take;
            p += take;
            n -= take;
        }
        if (rx->payload_fill == rx->payload_size) {
            if (rx->header[4] == 0) on_frame(rx, rx->payload, rx->payload_size);
            else if (rx->header[4] == 1) on_codec(rx, rx->payload, rx->payload_size);
            rx->header_fill = 0;
        }
    }
}

/* ---- The receiver: one thread and listener per process, at most one connection - the newest
 * (a newer one means the older is dead).  Every ALT_POLL_MS it also forwards the renderer's
 * key-frame requests and ends a session stock or a new phone session ended. */

static alt_rx_t *start_connection(int fd) {
    alt_rx_t *rx = (alt_rx_t *)calloc(1, sizeof(*rx));
    if (!rx) {
        close(fd);
        return NULL;
    }
    rx->fd = fd;
    rx->framed = 1;
    rx->length_size = 4;
    pthread_mutex_lock(&g_session_lock);
    g_session_since = 0;
    rx->hold = g_session_hold;
    pthread_mutex_unlock(&g_session_lock);
    if (ring_ready()) rx->stream = cvr_writer_new_session(&g_ring);
    LOG_WARN(LOG_MODULE, "cluster stream connected (ring stream %u)", rx->stream);
    return rx;
}

/* `keep_session`: the session outlives this connection (a newer one, or its end is handled). */
static void end_connection(alt_rx_t *rx, const char *why, int keep_session) {
    LOG_WARN(LOG_MODULE, "cluster stream %s after %lld bytes, %ld frames", why, rx->total, rx->frames);
    (void)why;
    if (rx->live) publish_live(rx, 0);
    if (rx->crypto > 0) rx->ctr_final(rx->context);
    if (!keep_session) drop_session(rx->hold, 0, "its stream ended");
    close(rx->fd);
    free(rx->payload);
    free(rx);
}

static void *sink_thread(void *arg) {
    static uint8_t buf[16384];
    int listener = (int)(intptr_t)arg, keyframe_pending = 0;
    uint64_t keyframe_last = 0;
    alt_rx_t *rx = NULL;
    for (;;) {
        struct pollfd fds[2];
        const char *why;
        uint32_t end;
        int n;
        fds[0].fd = listener;
        fds[1].fd = rx ? rx->fd : -1;
        fds[0].events = fds[1].events = POLLIN;
        fds[0].revents = fds[1].revents = 0;
        n = poll(fds, rx ? 2 : 1, ALT_POLL_MS);
        if (n < 0 && errno != EINTR) usleep(ALT_POLL_MS * 1000);
        if ((end = take_end(&why))) {
            if (rx && rx->hold == end) {
                end_connection(rx, "ended with its session", 1);
                rx = NULL;
            }
            drop_session(end, 0, why);
        }
        if (n > 0 && rx && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t got = read(rx->fd, buf, sizeof(buf));
            if (got > 0) {
                rx->total += got;
                rx_feed(rx, buf, (size_t)got);
            } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
                end_connection(rx, got == 0 ? "closed" : "failed", 0);
                rx = NULL;
            }
        }
        if (n > 0 && (fds[0].revents & POLLIN)) {
            int conn = accept(listener, NULL, NULL);
            if (conn >= 0) {
                fcntl(conn, F_SETFD, FD_CLOEXEC);
                if (rx) end_connection(rx, "replaced by a new connection", 1);
                rx = start_connection(conn);
            } else if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN) {
                LOG_WARN(LOG_MODULE, "cluster stream accept failed errno=%d; receiver stops", errno);
                if (rx) end_connection(rx, "closed: the receiver stops", 0);
                return NULL;
            }
        }
        /* The phone is asked at most once per ALT_KEYFRAME_MIN_MS, while a live stream can answer. */
        if (g_ring_ready && cvr_writer_requests(&g_ring)) keyframe_pending = 1;
        if (!rx || !rx->live) {
            keyframe_pending = 0;
        } else if (keyframe_pending && now_ns() - keyframe_last >= ALT_KEYFRAME_MIN_MS * 1000000ull) {
            keyframe_pending = 0;
            keyframe_last = now_ns();
            request_keyframe();
        }
        drop_session(0, 1, "no stream connection came");
    }
}

/* lsm-pf passes TCP into carplay0 only on the ALT_STREAM_PORTS (and stock's own): the first
 * free one.  IPv6, as the phone uses; IPv4 only where there is no IPv6 (host tests). */
static int start_sink(void) {
    static const int ports[] = ALT_STREAM_PORTS;
    struct sockaddr_in6 a6;
    struct sockaddr_in a4;
    struct sockaddr *addr = (struct sockaddr *)&a6;
    socklen_t length = sizeof(a6);
    pthread_t thread;
    int fd = socket(AF_INET6, SOCK_STREAM, 0), reuse = 1;
    size_t i;
    memset(&a6, 0, sizeof(a6));
    memset(&a4, 0, sizeof(a4));
    a6.sin6_family = AF_INET6;
    a6.sin6_addr = in6addr_any;
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 && errno == EAFNOSUPPORT) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        addr = (struct sockaddr *)&a4;
        length = sizeof(a4);
    }
    if (fd < 0) return 0;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    for (i = 0; i < sizeof(ports) / sizeof(ports[0]); ++i) {
        a6.sin6_port = a4.sin_port = htons((uint16_t)ports[i]);
        if (bind(fd, addr, length) == 0) break;
        if (errno != EADDRINUSE) i = sizeof(ports) / sizeof(ports[0]);
    }
    if (i == sizeof(ports) / sizeof(ports[0]) || listen(fd, 2) != 0
            || pthread_create(&thread, NULL, sink_thread, (void *)(intptr_t)fd) != 0) {
        LOG_WARN(LOG_MODULE, "cluster stream receiver unavailable errno=%d", errno);
        close(fd);
        return 0;
    }
    pthread_detach(thread);
    LOG_WARN(LOG_MODULE, "cluster stream receiver listening on port %d", ports[i]);
    return ports[i];
}

/* The receiver's port, started on first use; 0 when it cannot listen (retried next time). */
static int sink_port(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int port;
    pthread_mutex_lock(&lock);
    if (!port) port = start_sink();
    pthread_mutex_unlock(&lock);
    return port;
}

/* ---- SETUP replies */

/* Stock's reply `key` (an array, possibly absent) with `value` appended. */
static void append_to_reply(const cflite_t *cf, cf_ref_t reply, const char *key, cf_ref_t value) {
    cf_ref_t current = cf_get(cf, reply, key);
    cf_ref_t extended = current ? cf->array_create_mutable_copy(NULL, 0, current) : cf_array(cf);
    if (extended && value) {
        cf->array_append_value(extended, value);
        cf_set(cf, reply, key, extended);
    }
    cf_release(cf, extended);
    cf_release(cf, value);
}

static cf_ref_t text_ref(const cflite_t *cf, const char *text) {
    return cf->string_create_with_cstring(NULL, text, CF_STRING_ENCODING_UTF8);
}

/* A successful reply enables the cluster display's features (stock never sends
 * `enabledFeatures`) and answers a request for the stream with the receiver's port (stock
 * omits type 111, and the phone then tears the session down: map06). */
static void altscreen_on_setup_response(hook_setup_ctx_t *setup) {
    const cflite_t *cf = cflite();
    cf_ref_t streams, stream;
    long i, count;
    int port, asked = 0;
    if (!cf || setup->result != 0 || !setup->response) return;
    append_to_reply(cf, setup->response, "enabledFeatures", text_ref(cf, "viewAreas"));
    append_to_reply(cf, setup->response, "enabledFeatures", text_ref(cf, "altScreen"));
    streams = setup->request ? cf_get(cf, setup->request, "streams") : NULL;
    count = streams ? cf->array_get_count(streams) : 0;
    for (i = 0; i < count; ++i) asked |= is_cluster_stream(cf, cf->array_get_value_at_index(streams, i));
    if (!asked || !setup->session || !(port = sink_port()) || !(stream = cf_dictionary(cf))) return;
    cf_set_int64(cf, stream, "type", ALT_STREAM_TYPE);
    cf_set_int64(cf, stream, "dataPort", port);
    append_to_reply(cf, setup->response, "streams", stream);
    LOG_WARN(LOG_MODULE, "SETUP %u reply: the cluster stream's data port is %d", setup->sequence, port);
    hold_session(cf, setup->session);
    send_commands_soon(cf, (void *)cf->retain(setup->session), 1, 1, 0, -1);
}

const hook_module_def_t altscreen_module_def = {
    .name = "altscreen",
    .priority = HOOK_PRIORITY_NORMAL,
    .on_state = altscreen_on_state,
    .on_init = altscreen_init,
    .on_shutdown = altscreen_shutdown,
    .airplay = {
        .on_setup_request = altscreen_on_setup_request,
        .on_setup_response = altscreen_on_setup_response,
        .on_server_info = altscreen_on_server_info,
        .on_security_info = altscreen_on_security_info,
        .on_session_teardown = altscreen_on_session_teardown,
    },
};
