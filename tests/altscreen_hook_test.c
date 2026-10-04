/* The altscreen module (hook/altscreen/altscreen_hook.c) against recording fakes of the stock
 * CoreFoundation-lite (resolved by name as on the car: the test is linked -rdynamic), stock's
 * command sender, key derivation and AES-CTR (a keystream running across frames), and the
 * hook bus; its receiver on real sockets.  Checks: /info gains the cluster display; a mixed
 * SETUP reaches stock without the cluster stream; SETUP replies enable altScreen and add the
 * stream on a filter-allowed port, then showUI and forceKeyFrame; the stream decrypts into the
 * frame ring as Annex B access units (garbage never does); the bus says when it is live and
 * ended; the renderer's key-frame requests become forceKeyFrame, at most once per
 * ALT_KEYFRAME_MIN_MS; the session is held only while its stream can use it (released when
 * stock tears the whole session down, its stream ends, a new phone session starts, a newer
 * SETUP replaces it or no connection comes); Java's wheel steps become changeMapZoomLevel and
 * the MMI's night mode setNightMode for the cluster display; no CF object leaks. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "altscreen/altscreen_hook.h"
#include "framework/cflite.h"
#include "cluster_video_ring.h"
#include "most/check.h"

/* ---------------------------------------------------------------- fake CFLite */

enum { K_STRING, K_NUMBER, K_BOOL, K_DICT, K_ARRAY, K_DATA };
typedef struct obj {
    int kind, refs;
    char text[96];                        /* string / data */
    long length;                          /* data */
    int64_t number;
    struct obj *keys[32], *values[32];    /* dict: keys + values; array: values */
    int count;
} obj;

static int g_live;
const int kCFLDictionaryKeyCallBacksCFLTypes = 1;
const int kCFLDictionaryValueCallBacksCFLTypes = 2;
const int kCFLArrayCallBacksCFLTypes = 3;
static obj g_false = { K_BOOL, 1 << 30, "false", 0, 0, {0}, {0}, 0 };
const void *kCFLBooleanFalse = &g_false;   /* a CFBooleanRef variable, as in MU1329's libairplay */
static obj g_true = { K_BOOL, 1 << 30, "true", 0, 0, {0}, {0}, 0 };
const void *kCFLBooleanTrue = &g_true;

static obj *new_obj(int kind) {
    obj *o = (obj *)calloc(1, sizeof(obj));
    o->kind = kind;
    o->refs = 1;
    ++g_live;
    return o;
}
static obj *retain(obj *o) { if (o) ++o->refs; return o; }

void CFRelease(const void *p) {
    obj *o = (obj *)p;
    int i;
    if (!o || o == &g_false || o == &g_true || --o->refs > 0) return;
    for (i = 0; i < o->count; ++i) {
        if (o->kind == K_DICT) CFRelease(o->keys[i]);
        CFRelease(o->values[i]);
    }
    free(o);
    --g_live;
}

const void *CFStringCreateWithCString(const void *alloc, const char *s, uint32_t encoding) {
    obj *o = new_obj(K_STRING);
    (void)alloc; (void)encoding;
    snprintf(o->text, sizeof(o->text), "%s", s);
    return o;
}

static int slot(obj *d, obj *key) {
    int i;
    for (i = 0; i < d->count; ++i) if (strcmp(d->keys[i]->text, key->text) == 0) return i;
    return -1;
}

const void *CFDictionaryCreateMutable(const void *a, long c, const void *k, const void *v) {
    (void)a; (void)c;
    if (k != &kCFLDictionaryKeyCallBacksCFLTypes || v != &kCFLDictionaryValueCallBacksCFLTypes) return NULL;
    return new_obj(K_DICT);
}
const void *CFDictionaryGetValue(const void *d, const void *key) {
    int i = slot((obj *)d, (obj *)key);
    return i < 0 ? NULL : ((obj *)d)->values[i];
}
void CFDictionarySetValue(const void *pd, const void *pkey, const void *value) {
    obj *d = (obj *)pd, *key = (obj *)pkey;
    int i = slot(d, key);
    if (i >= 0) {
        CFRelease(d->values[i]);
        d->values[i] = retain((obj *)value);
        return;
    }
    d->keys[d->count] = (obj *)CFStringCreateWithCString(NULL, key->text, 0);
    d->values[d->count++] = retain((obj *)value);
}
int64_t CFDictionaryGetInt64(const void *d, const void *key, int32_t *err) {
    obj *v = (obj *)CFDictionaryGetValue(d, key);
    *err = v && v->kind == K_NUMBER ? 0 : -6727;
    return *err ? 0 : v->number;
}
void CFDictionarySetInt64(const void *d, const void *key, int64_t value) {
    obj *n = new_obj(K_NUMBER);
    n->number = value;
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}
int32_t CFDictionarySetCString(const void *d, const void *key, const char *s, size_t length) {
    const void *str = CFStringCreateWithCString(NULL, s, 0);
    (void)length;
    CFDictionarySetValue(d, key, str);
    CFRelease(str);
    return 0;
}
const void *CFArrayCreateMutable(const void *a, long c, const void *cb) {
    (void)a; (void)c;
    return cb == &kCFLArrayCallBacksCFLTypes ? new_obj(K_ARRAY) : NULL;
}
long CFArrayGetCount(const void *a) { return ((obj *)a)->count; }
const void *CFArrayGetValueAtIndex(const void *a, long i) { return ((obj *)a)->values[i]; }
void CFArrayAppendValue(const void *a, const void *v) { ((obj *)a)->values[((obj *)a)->count++] = retain((obj *)v); }
const void *CFArrayCreateMutableCopy(const void *alloc, long c, const void *src) {
    obj *a = (obj *)CFArrayCreateMutable(alloc, c, &kCFLArrayCallBacksCFLTypes);
    int i;
    for (i = 0; i < ((obj *)src)->count; ++i) CFArrayAppendValue(a, ((obj *)src)->values[i]);
    return a;
}
const void *CFPropertyListCreateDeepCopy(const void *alloc, const void *p, unsigned long options) {
    obj *o = (obj *)p, *c;
    int i;
    if (o == &g_false) return o;
    c = new_obj(o->kind);
    memcpy(c->text, o->text, sizeof(c->text));
    c->number = o->number;
    c->length = o->length;
    for (i = 0; i < o->count; ++i) {
        if (o->kind == K_DICT) c->keys[i] = (obj *)CFStringCreateWithCString(NULL, o->keys[i]->text, 0);
        c->values[i] = (obj *)CFPropertyListCreateDeepCopy(alloc, o->values[i], options);
    }
    c->count = o->count;
    return c;
}
/* Like stock's writer, anything that is not a CF object fails the whole plist - such as the
 * address of the kCFLBooleanFalse variable in place of its value (map04's 500 on /info). */
static int serializable(const obj *o) {
    int i;
    if (o == (const obj *)&kCFLBooleanFalse) return 0;
    if (o == &g_false || (o->kind != K_DICT && o->kind != K_ARRAY)) return 1;
    for (i = 0; i < o->count; ++i) if (!serializable(o->values[i])) return 0;
    return 1;
}
const void *CFPropertyListCreateData(const void *alloc, const void *p, long format, unsigned long options, void *error) {
    (void)alloc; (void)format; (void)options; (void)error;
    return serializable((const obj *)p) ? new_obj(K_DATA) : NULL;
}
const void *CFRetain(const void *p) { return retain((obj *)p); }
/* Stock's command sender: records the commands the module sends, in order. */
#define SENT_MAX 64
static char g_sent_type[SENT_MAX][32], g_sent_uuid[SENT_MAX][64], g_sent_url[SENT_MAX][64];
static int64_t g_sent_zoom[SENT_MAX];      /* zoomDirection, or -1 */
static int g_sent_night[SENT_MAX];         /* nightMode: 1 true, 0 false, -1 absent */
static int g_sent;
static const char *text_at(const void *dict, const char *key) {
    const void *k = CFStringCreateWithCString(NULL, key, 0);
    obj *v = dict ? (obj *)CFDictionaryGetValue(dict, k) : NULL;
    CFRelease(k);
    return v && v->kind == K_STRING ? v->text : "";
}
/* The phone answers at once: status 0 and an empty reply. */
int AirPlayReceiverSessionSendCommand(void *session, const void *command,
                                      void (*done)(int, const void *, void *), void *context) {
    const void *k = CFStringCreateWithCString(NULL, "params", 0);
    const void *params = CFDictionaryGetValue(command, k);
    (void)session;
    if (done) {
        const void *reply = CFDictionaryCreateMutable(NULL, 0, &kCFLDictionaryKeyCallBacksCFLTypes,
                                                      &kCFLDictionaryValueCallBacksCFLTypes);
        done(0, reply, context);
        CFRelease(reply);
    }
    CFRelease(k);
    if (g_sent < SENT_MAX) {
        const void *zk = CFStringCreateWithCString(NULL, "zoomDirection", 0);
        obj *zoom = params ? (obj *)CFDictionaryGetValue(params, zk) : NULL;
        CFRelease(zk);
        g_sent_zoom[g_sent] = zoom && zoom->kind == K_NUMBER ? zoom->number : -1;
        zk = CFStringCreateWithCString(NULL, "nightMode", 0);
        zoom = params ? (obj *)CFDictionaryGetValue(params, zk) : NULL;
        CFRelease(zk);
        g_sent_night[g_sent] = zoom == &g_true ? 1 : zoom == &g_false ? 0 : -1;
        snprintf(g_sent_uuid[g_sent], sizeof(g_sent_uuid[0]), "%s", text_at(params, "uuid"));
        snprintf(g_sent_url[g_sent], sizeof(g_sent_url[0]), "%s", text_at(params, "url"));
        snprintf(g_sent_type[g_sent], sizeof(g_sent_type[0]), "%s", text_at(command, "type"));
        ++g_sent;
    }
    return 0;
}

/* Stock's screen-key derivation and AES-CTR, faked: the derivation records its inputs, and the
 * "cipher" is a keystream of key, IV and position that, like stock's, runs on across calls.
 * Stock's derivation is void and leaves free()'s return in r0 - nonzero here, as on the car
 * (map12), where reading it as a status stopped the decryption. */
static uint8_t g_derived_master[16];
static uint64_t g_derived_id;
static int g_derived;
int AirPlay_DeriveAESKeySHA512ForScreen(const void *master, size_t length, uint64_t id,
                                        uint8_t key[16], uint8_t iv[16]) {
    int i;
    if (length != 16) return -6705;
    memcpy(g_derived_master, master, 16);
    g_derived_id = id;
    ++g_derived;
    for (i = 0; i < 16; ++i) {
        key[i] = (uint8_t)(((const uint8_t *)master)[i] ^ (uint8_t)(id >> (8 * (i % 8))));
        iv[i] = (uint8_t)(i * 7 + 1);
    }
    return 0x7f3a10;
}
typedef struct { uint8_t key[16], iv[16]; uint64_t at; } fake_ctr_t;
static uint8_t keystream(const fake_ctr_t *c, uint64_t at) {
    return (uint8_t)(c->key[at % 16] ^ c->iv[(at / 16) % 16] ^ (uint8_t)(at / 16) ^ 0x5a);
}
int AES_CTR_Init(void *context, const uint8_t key[16], const uint8_t iv[16]) {
    fake_ctr_t *c = (fake_ctr_t *)context;
    memcpy(c->key, key, 16);
    memcpy(c->iv, iv, 16);
    c->at = 0;
    return 0;
}
int AES_CTR_Update(void *context, const void *in, size_t length, void *out) {
    fake_ctr_t *c = (fake_ctr_t *)context;
    size_t i;
    for (i = 0; i < length; ++i) ((uint8_t *)out)[i] = (uint8_t)(((const uint8_t *)in)[i] ^ keystream(c, c->at++));
    return 0;
}
void AES_CTR_Final(void *context) { memset(context, 0, sizeof(fake_ctr_t)); }

/* The hook bus, faked: what the module tells Java about the stream (EVT_CLUSTER_VIDEO). */
static pthread_mutex_t g_bus_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_bus_events, g_bus_live = -1, g_bus_sticky, g_bus_wrong_type;
static uint32_t g_bus_stream;
static int g_text_live;
static uint32_t g_text_stream;
void bus_text_begin_with(bus_text_builder_t *b, const char *object_name, uint8_t *buf, uint32_t cap) {
    (void)object_name;
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->own_buf = false;
    b->overflow = false;
}
void bus_text_bool(bus_text_builder_t *b, const char *key, bool value) {
    (void)b;
    if (strcmp(key, "live") == 0) g_text_live = value;
}
void bus_text_uint(bus_text_builder_t *b, const char *key, uint64_t value) {
    (void)b;
    if (strcmp(key, "stream") == 0) g_text_stream = (uint32_t)value;
}
hook_result_t bus_send_text(uint16_t type, uint8_t flags, bus_text_builder_t *b) {
    (void)b;
    pthread_mutex_lock(&g_bus_lock);
    ++g_bus_events;
    if (type != EVT_CLUSTER_VIDEO) ++g_bus_wrong_type;
    g_bus_sticky = (flags & BUS_FLAG_STICKY) != 0;
    g_bus_live = g_text_live;
    g_bus_stream = g_text_stream;
    pthread_mutex_unlock(&g_bus_lock);
    return HOOK_OK;
}
/* Java's commands: the handlers the module registers for CMD_ALT_ZOOM and CMD_ALT_APPEARANCE. */
static bus_handler_t g_zoom_handler, g_appearance_handler;
static int g_bus_other_handlers;
hook_result_t bus_on(uint16_t type, bus_handler_t handler, void *ctx) {
    (void)ctx;
    if (type == CMD_ALT_ZOOM) g_zoom_handler = handler;
    else if (type == CMD_ALT_APPEARANCE) g_appearance_handler = handler;
    else ++g_bus_other_handlers;
    return HOOK_OK;
}
void bus_off(uint16_t type) {
    if (type == CMD_ALT_ZOOM) g_zoom_handler = NULL;
    if (type == CMD_ALT_APPEARANCE) g_appearance_handler = NULL;
}
static void wheel(int8_t steps) {
    uint8_t payload = (uint8_t)steps;
    if (g_zoom_handler) g_zoom_handler(CMD_ALT_ZOOM, BUS_FLAG_BINARY, &payload, 1, NULL);
}
static void mmi_night(int night) {
    uint8_t payload = (uint8_t)night;
    if (g_appearance_handler) g_appearance_handler(CMD_ALT_APPEARANCE, BUS_FLAG_BINARY, &payload, 1, NULL);
}

static int bus_events(int *live, uint32_t *stream) {
    int n;
    pthread_mutex_lock(&g_bus_lock);
    n = g_bus_events;
    if (live) *live = g_bus_live;
    if (stream) *stream = g_bus_stream;
    pthread_mutex_unlock(&g_bus_lock);
    return n;
}
/* Waits up to a second for the n-th bus event. */
static int bus_event(int n, int *live, uint32_t *stream) {
    int i;
    for (i = 0; i < 100 && bus_events(NULL, NULL) < n; ++i) usleep(10000);
    return bus_events(live, stream) == n;
}
static int refs_become(obj *o, int refs) {
    int i;
    for (i = 0; i < 150 && o->refs != refs; ++i) usleep(10000);
    return o->refs == refs;
}

/* ---------------------------------------------------------------- helpers */

static obj *get(obj *d, const char *key) { return (obj *)cf_get(cflite(), d, key); }
static int64_t num(obj *d, const char *key) { obj *v = get(d, key); return v && v->kind == K_NUMBER ? v->number : -1; }
static const char *str(obj *d, const char *key) { obj *v = get(d, key); return v && v->kind == K_STRING ? v->text : ""; }
static void put_num(obj *d, const char *key, int64_t v) { cf_set_int64(cflite(), d, key, v); }
static void put(obj *d, const char *key, obj *v) { cf_set(cflite(), d, key, v); }
static obj *dict(void) { return (obj *)cf_dictionary(cflite()); }
static obj *array(void) { return (obj *)cf_array(cflite()); }

static obj *setup_request(const int *types, int n) {
    obj *request = dict(), *streams = array();
    int i;
    for (i = 0; i < n; ++i) {
        obj *s = dict();
        put_num(s, "type", types[i]);
        put_num(s, "streamConnectionID", 1000 + i);
        CFArrayAppendValue(streams, s);
        CFRelease(s);
    }
    put(request, "streams", streams);
    CFRelease(streams);
    return request;
}

static int stream_types(obj *request, int *out) {
    obj *streams = get(request, "streams");
    int i;
    for (i = 0; i < streams->count; ++i) out[i] = (int)num(streams->values[i], "type");
    return streams->count;
}

static int filter_allowed(int port) {
    static const int ports[] = ALT_STREAM_PORTS;
    size_t i;
    for (i = 0; i < sizeof(ports) / sizeof(ports[0]); ++i) if (ports[i] == port) return 1;
    return 0;
}

static int g_port;

static int connect_receiver(void) {
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
    if (fd >= 0) close(fd);
    return -1;
}

/* The phone's end of a connection the receiver closed: EOF (or a reset) within a second. */
static int closed_by_receiver(int fd) {
    struct pollfd p;
    char c;
    int i;
    for (i = 0; fd >= 0 && i < 100; ++i) {
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 10) > 0) {
            ssize_t n = read(fd, &c, 1);
            return n == 0 || (n < 0 && errno == ECONNRESET);
        }
    }
    return 0;
}

/* ---- The cluster stream as the phone sends it (stock's screen framing): a 128-byte header
 * (payload size little-endian, type in byte 4), then the payload. */

static uint8_t g_wire[4096];
static size_t g_wire_size;

static void packet(int type, const uint8_t *payload, size_t size) {
    uint8_t *h = g_wire + g_wire_size;
    memset(h, 0, 128);
    h[0] = (uint8_t)size;
    h[1] = (uint8_t)(size >> 8);
    h[4] = (uint8_t)type;
    memcpy(h + 128, payload, size);
    g_wire_size += 128 + size;
}

/* One connection sending the wire in uneven pieces, then closing. */
static int send_wire(void) {
    static const size_t pieces[] = { 1, 7, 130, 3 };
    size_t at = 0, i = 0;
    int fd = connect_receiver();
    if (fd < 0) return 0;
    while (at < g_wire_size) {
        size_t take = pieces[i++ % 4];
        if (take > g_wire_size - at) take = g_wire_size - at;
        if (write(fd, g_wire + at, take) != (ssize_t)take) break;
        at += take;
        usleep(1000);
    }
    close(fd);
    return at == g_wire_size;
}

static void encrypted_frame(fake_ctr_t *cipher, const uint8_t *frame, size_t size) {
    uint8_t sealed[256];
    AES_CTR_Update(cipher, frame, size, sealed);
    packet(0, sealed, size);
}

/* The session key, the stream's parameter sets, an IDR frame (behind an access unit delimiter)
 * and a P frame as length-prefixed NAL units; and as Annex B, as the renderer reads them:
 * config 0..17, key frame 17..33, P frame 33..41. */
static const uint8_t k_master[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, k_iv[16] = { 0 };
static const uint8_t k_avcc[] = { 1, 0x64, 0, 0x1f, 0xff, 0xe1, 0, 5, 0x67, 0x64, 0, 0x1f, 0xac,
                                  1, 0, 4, 0x68, 0xee, 0x3c, 0xb0 };
static const uint8_t k_idr[] = { 0, 0, 0, 2, 0x09, 0x10, 0, 0, 0, 6, 0x65, 0x88, 0x84, 0, 0x33, 0x7f };
static const uint8_t k_p_frame[] = { 0, 0, 0, 4, 0x41, 0x9a, 2, 3 };
static const uint8_t k_annexb[] = {
    0, 0, 0, 1, 0x67, 0x64, 0, 0x1f, 0xac,  0, 0, 0, 1, 0x68, 0xee, 0x3c, 0xb0,
    0, 0, 0, 1, 0x09, 0x10,  0, 0, 0, 1, 0x65, 0x88, 0x84, 0, 0x33, 0x7f,
    0, 0, 0, 1, 0x41, 0x9a, 2, 3 };

/* The phone's cipher for a new connection (id 1000, the last cluster SETUP's). */
static void phone_cipher(fake_ctr_t *phone) {
    uint8_t key[16], stream_iv[16];
    AirPlay_DeriveAESKeySHA512ForScreen(k_master, 16, 1000, key, stream_iv);
    AES_CTR_Init(phone, key, stream_iv);
}

/* An avcC record and an encrypted IDR frame on the wire. */
static void live_wire(void) {
    fake_ctr_t phone;
    phone_cipher(&phone);
    g_wire_size = 0;
    packet(1, k_avcc, sizeof(k_avcc));
    encrypted_frame(&phone, k_idr, sizeof(k_idr));
}

/* The next record the renderer reads from the frame ring, within a second: 1 when it is
 * exactly `flags` and `bytes`. */
static int ring_record(cvr_reader_t *ring, uint32_t flags, const uint8_t *bytes, size_t size) {
    static uint8_t out[1024];
    cvr_record_t rec;
    int i;
    for (i = 0; i < 100; ++i) {
        if (cvr_reader_next(ring, &rec, out, sizeof(out)))
            return rec.flags == flags && rec.bytes == size && memcmp(out, bytes, size) == 0;
        usleep(10000);
    }
    return 0;
}

/* A cluster-stream SETUP reply; its showUI and forceKeyFrame sent. */
static obj *cluster_setup(void) {
    static const int cluster_only[] = { 111 };
    hook_setup_ctx_t setup;
    obj *request = setup_request(cluster_only, 1), *reply = dict(), *session = dict();
    int sent = g_sent, i;
    memset(&setup, 0, sizeof(setup));
    setup.request = request;
    setup.response = reply;
    setup.session = session;
    altscreen_module_def.airplay.on_setup_response(&setup);
    CFRelease(reply);
    CFRelease(request);
    for (i = 0; i < 100 && (g_sent < sent + 2 || session->refs != 2); ++i) usleep(10000);
    return session;
}

/* ---------------------------------------------------------------- scenarios */

static void server_info(void) {
    int live = g_live;
    obj *before = dict(), *displays = array(), *primary = dict(), *cluster, *area, *safe;
    void *info = before;
    put_num(primary, "widthPixels", 1280);
    CFArrayAppendValue(displays, primary);
    CFRelease(primary);
    put(before, "displays", displays);
    CFRelease(displays);
    put_num(before, "features", 0x1234);

    altscreen_module_def.airplay.on_server_info(NULL, &info);
    displays = get((obj *)info, "displays");
    check(info != before && displays && displays->count == 2 && num(displays->values[0], "widthPixels") == 1280,
          "/info is replaced by a copy with one display added to stock's");
    if (!displays || displays->count != 2) { CFRelease(info); return; }
    cluster = displays->values[1];
    check(num(cluster, "type") == 111 && strcmp(str(cluster, "uuid"), ALT_DISPLAY_UUID) == 0
          && strcmp(str(cluster, "initialURL"), "maps:/car/instrumentcluster/map") == 0
          && num(cluster, "widthPixels") == 800 && num(cluster, "heightPixels") == 298
          && num(cluster, "widthPhysical") == 160 && num(cluster, "heightPhysical") == 60
          && num(cluster, "maxFPS") == 60 && num(cluster, "features") == 2
          && num(cluster, "primaryInputDevice") == 3 && num(cluster, "initialViewArea") == 0,
          "the cluster display: its UUID, type 111, the cluster-map URL, the map's size, knob-driven");
    area = get(cluster, "viewAreas") ? get(cluster, "viewAreas")->values[0] : NULL;
    safe = area ? get(area, "safeArea") : NULL;
    check(area && safe && num(area, "widthPixels") == 800 && num(area, "heightPixels") == 298
          && num(area, "originXPixels") == 0 && num(area, "originYPixels") == 0
          && num(safe, "originXPixels") == 150 && num(safe, "originYPixels") == 0
          && num(safe, "widthPixels") == 500 && num(safe, "heightPixels") == 278
          && get(area, "viewAreaTransitionControl") == &g_false && get(safe, "drawUIOutsideSafeArea") == &g_false,
          "one view area covering the display, safe between the dials and above the street label");
    check(num((obj *)info, "features") == (0x1234 | (1LL << 26)), "features keeps stock's bits and adds bit 26");
    CFRelease(info);
    check(g_live == live, "stock's /info was released when replaced: %d live", g_live - live);
}

static void setup_requests(void) {
    static const int mixed[] = { 110, 111, 101 }, cluster_only[] = { 111 };
    hook_setup_ctx_t setup;
    int types[8], original[8], count;
    obj *request = setup_request(mixed, 3);
    memset(&setup, 0, sizeof(setup));
    setup.request = request;
    altscreen_module_def.airplay.on_setup_request(&setup);
    count = setup.request != request ? stream_types((obj *)setup.request, types) : 0;
    check(count == 2 && types[0] == 110 && types[1] == 101 && stream_types(request, original) == 3,
          "a mixed SETUP reaches stock as a copy without the cluster stream; the phone's is untouched");
    if (setup.request != request) CFRelease(setup.request);
    CFRelease(request);
    setup.request = request = setup_request(cluster_only, 1);
    altscreen_module_def.airplay.on_setup_request(&setup);
    check(setup.request == request, "a cluster-only SETUP passes unchanged (stock ignores type 111)");
    CFRelease(request);
}

/* Stock's SETUP replies: a successful one enables viewAreas and altScreen; one to a request
 * for the cluster stream keeps stock's streams and gains it with the receiver's port. */
static void setup_replies(void) {
    static const int ordinary[] = { 110, 101 }, cluster_only[] = { 111 };
    hook_setup_ctx_t setup;
    obj *request = setup_request(ordinary, 2), *reply = dict(), *enabled, *streams, *screen, *session = dict();
    int fd, i;
    memset(&setup, 0, sizeof(setup));
    setup.request = request;
    setup.response = reply;
    setup.result = -6700;
    altscreen_module_def.airplay.on_setup_response(&setup);
    check(get(reply, "enabledFeatures") == NULL, "a failed SETUP reply is stock's");
    setup.result = 0;
    altscreen_module_def.airplay.on_setup_response(&setup);
    enabled = get(reply, "enabledFeatures");
    check(enabled && enabled->count == 2 && strcmp(enabled->values[0]->text, "viewAreas") == 0
          && strcmp(enabled->values[1]->text, "altScreen") == 0 && get(reply, "streams") == NULL && g_sent == 0,
          "a successful one enables viewAreas and altScreen; no stream or command unless asked");
    CFRelease(reply);
    CFRelease(request);

    setup.request = request = setup_request(cluster_only, 1);
    setup.response = reply = dict();
    setup.session = session;
    streams = array();
    screen = dict();
    put_num(screen, "dataPort", 6030);
    CFArrayAppendValue(streams, screen);
    CFRelease(screen);
    put(reply, "streams", streams);
    CFRelease(streams);
    altscreen_module_def.airplay.on_setup_response(&setup);
    streams = get(reply, "streams");
    g_port = streams && streams->count == 2 && num(streams->values[1], "type") == 111
             ? (int)num(streams->values[1], "dataPort") : 0;
    check(num(streams->values[0], "dataPort") == 6030 && filter_allowed(g_port),
          "a cluster-stream reply keeps stock's streams and adds 111 on port %d, one lsm-pf lets in", g_port);
    CFRelease(reply);
    CFRelease(request);
    for (i = 0; i < 100 && (g_sent < 2 || session->refs != 2); ++i) usleep(10000);
    check(g_sent == 2 && strcmp(g_sent_type[0], "showUI") == 0 && strcmp(g_sent_uuid[0], ALT_DISPLAY_UUID) == 0
          && strcmp(g_sent_url[0], "maps:/car/instrumentcluster/map") == 0
          && strcmp(g_sent_type[1], "forceKeyFrame") == 0 && g_sent_url[1][0] == '\0' && session->refs == 2,
          "then showUI and forceKeyFrame; the session held for its stream (%d refs)", session->refs);
    fd = connect_receiver();
    if (fd >= 0) close(fd);
    check(fd >= 0 && refs_become(session, 1), "the session is released when its stream connection ends: %d refs",
          session->refs);
    CFRelease(session);
    check(bus_events(NULL, NULL) == 0, "a stream that never decrypts to H.264 is never live");
}

static void stream_decryption(void) {
    static const uint8_t stats[] = "bplist00 encoder statistics";
    fake_ctr_t phone;
    cvr_reader_t ring;
    uint32_t stream = 0;
    int live, events = bus_events(NULL, NULL);
    altscreen_module_def.airplay.on_security_info(NULL, k_master, k_iv);
    phone_cipher(&phone);
    g_derived = 0;
    g_wire_size = 0;
    packet(1, k_avcc, sizeof(k_avcc));
    encrypted_frame(&phone, k_idr, sizeof(k_idr));
    packet(5, stats, sizeof(stats));
    encrypted_frame(&phone, k_p_frame, sizeof(k_p_frame));
    check(send_wire() && cvr_reader_attach(&ring) == 0, "a stream connection sets the frame ring up");
    check(ring_record(&ring, CVR_CONFIG, k_annexb, 17) && ring_record(&ring, CVR_FRAME | CVR_KEY, k_annexb + 17, 16)
          && ring_record(&ring, CVR_FRAME, k_annexb + 33, 8),
          "its frames, decrypted across uneven reads, reach the ring as Annex B: config, key frame, frame");
    check(g_derived == 1 && memcmp(g_derived_master, k_master, 16) == 0 && g_derived_id == 1000,
          "with stock's keys from the session key and the stream's connection id (%llu)", (unsigned long long)g_derived_id);
    check(bus_event(events + 2, &live, &stream) && live == 0 && stream == ring.session && g_bus_sticky && !g_bus_wrong_type,
          "the bus said the stream was live (sticky EVT_CLUSTER_VIDEO), then that it ended (stream %u)", stream);

    g_wire_size = 0;
    packet(1, k_avcc, sizeof(k_avcc));
    packet(0, k_idr, sizeof(k_idr));            /* not encrypted: garbage once decrypted */
    check(send_wire() && ring_record(&ring, CVR_CONFIG, k_annexb, 17) && !ring_record(&ring, CVR_FRAME, k_annexb + 33, 8)
          && bus_events(NULL, NULL) == events + 2,
          "a frame that is not H.264 after decryption never reaches the ring, nor is that stream live");
    cvr_reader_detach(&ring);
}

/* A connected stream: live; key-frame requests reach the phone, at most one per
 * ALT_KEYFRAME_MIN_MS; a newer connection replaces the older one; a new phone session ends
 * the stream; a session never connected expires; a newer SETUP replaces a held session. */
static void live_stream(void) {
    cvr_reader_t ring;
    obj *session = cluster_setup();
    uint32_t stream = 0;
    int fd, fd2, live, events = bus_events(NULL, NULL), sent = g_sent, i;
    live_wire();
    fd = connect_receiver();
    check(fd >= 0 && write(fd, g_wire, g_wire_size) == (ssize_t)g_wire_size && bus_event(events + 1, &live, &stream)
          && live == 1, "a connected stream is live");
    check(cvr_reader_attach(&ring) == 0 && ring_record(&ring, CVR_CONFIG, k_annexb, 17)
          && ring_record(&ring, CVR_FRAME | CVR_KEY, k_annexb + 17, 16) && ring.session == stream,
          "a renderer joining reads its config and key frame (stream %u)", stream);

    cvr_reader_request_key(&ring);
    for (i = 0; i < 100 && g_sent < sent + 1; ++i) usleep(10000);
    check(g_sent == sent + 1 && strcmp(g_sent_type[sent], "forceKeyFrame") == 0,
          "the renderer's key-frame request reaches the phone as forceKeyFrame");
    cvr_reader_request_key(&ring);
    cvr_reader_request_key(&ring);
    usleep(ALT_KEYFRAME_MIN_MS * 1000 / 2);
    check(g_sent == sent + 1, "more requests within ALT_KEYFRAME_MIN_MS wait");
    for (i = 0; i < 150 && g_sent < sent + 2; ++i) usleep(10000);
    usleep(2 * ALT_POLL_MS * 1000);
    check(g_sent == sent + 2, "then go out together, once: %d commands", g_sent - sent);

    live_wire();
    fd2 = connect_receiver();
    check(fd2 >= 0 && bus_event(events + 2, &live, NULL) && live == 0, "a newer connection ends the older one's stream");
    check(write(fd2, g_wire, g_wire_size) == (ssize_t)g_wire_size && bus_event(events + 3, &live, &stream)
          && live == 1 && stream != ring.session && session->refs == 2,
          "and is live itself, a new stream (%u) of the same held session", stream);
    altscreen_module_def.on_state(NULL, HOOK_EVENT_IDENTIFY_START, NULL);
    check(bus_event(events + 4, &live, NULL) && live == 0 && refs_become(session, 1),
          "a new phone session ends the stream and lets its session go: %d refs", session->refs);
    if (fd >= 0) close(fd);
    if (fd2 >= 0) close(fd2);
    CFRelease(session);
    cvr_reader_detach(&ring);

    session = cluster_setup();
    check(session->refs == 2 && refs_become(session, 1), "a session whose stream never connects expires");
    CFRelease(session);

    session = cluster_setup();
    {
        obj *newer = cluster_setup();
        check(newer->refs == 2 && refs_become(session, 1),
              "a newer SETUP replaces the held session, released by the receiver: %d refs", session->refs);
        CFRelease(session);
        check(refs_become(newer, 1), "and the newer one is let go in turn: %d refs", newer->refs);
        CFRelease(newer);
    }
}

/* A pulled cable never closes the stream's connection (map14): stock's teardown of the whole
 * session must end it and let the session go.  A teardown of single streams (the phone ends
 * its audio mid-session), or of another session, changes nothing. */
static void torn_down_session(void) {
    obj *session = cluster_setup(), *other = dict();
    int fd, live, events = bus_events(NULL, NULL);
    live_wire();
    fd = connect_receiver();
    check(fd >= 0 && write(fd, g_wire, g_wire_size) == (ssize_t)g_wire_size
          && bus_event(events + 1, &live, NULL) && live == 1, "a held session's stream is live");
    altscreen_module_def.airplay.on_session_teardown(session, 0);
    altscreen_module_def.airplay.on_session_teardown(other, 1);
    usleep(3 * ALT_POLL_MS * 1000);
    check(bus_events(NULL, NULL) == events + 1 && session->refs == 2,
          "a teardown of single streams, or of another session, leaves it: %d refs", session->refs);
    altscreen_module_def.airplay.on_session_teardown(session, 1);
    check(bus_event(events + 2, &live, NULL) && live == 0 && refs_become(session, 1) && closed_by_receiver(fd),
          "stock's whole-session teardown ends the stream, closes its connection, lets the session go");
    if (fd >= 0) close(fd);
    CFRelease(other);
    CFRelease(session);
}

/* `n` commands from `from` on: all changeMapZoomLevel for the cluster display, `direction`. */
static int zooms(int from, int n, int64_t direction) {
    int i, ok = g_sent == from + n;
    for (i = from; ok && i < from + n; ++i)
        ok = strcmp(g_sent_type[i], "changeMapZoomLevel") == 0 && strcmp(g_sent_uuid[i], ALT_DISPLAY_UUID) == 0
             && g_sent_zoom[i] == direction;
    return ok;
}

/* The steering-wheel roller over the MAP view: Java's signed MapScale steps (CMD_ALT_ZOOM)
 * become one changeMapZoomLevel each - in for negative, out for positive, at most
 * ALT_ZOOM_MAX_STEPS a report - only while a session is held; the handler goes at shutdown. */
static void wheel_zoom(void) {
    obj *session;
    int sent, i;
    altscreen_module_def.on_init();
    check(g_zoom_handler && g_appearance_handler && !g_bus_other_handlers,
          "the module takes CMD_ALT_ZOOM and CMD_ALT_APPEARANCE from the bus at init");
    sent = g_sent;
    wheel(1);
    usleep(50000);
    check(g_sent == sent, "no session held: the wheel sends nothing");

    session = cluster_setup();
    sent = g_sent;
    wheel(-2);
    for (i = 0; i < 100 && (g_sent < sent + 2 || session->refs != 2); ++i) usleep(10000);
    check(zooms(sent, 2, 0) && session->refs == 2, "two steps in: two changeMapZoomLevel, zoomDirection 0");
    sent = g_sent;
    wheel(1);
    for (i = 0; i < 100 && (g_sent < sent + 1 || session->refs != 2); ++i) usleep(10000);
    check(zooms(sent, 1, 1), "a step out: zoomDirection 1");
    sent = g_sent;
    wheel(100);
    for (i = 0; i < 100 && (g_sent < sent + ALT_ZOOM_MAX_STEPS || session->refs != 2); ++i) usleep(10000);
    usleep(50000);
    check(zooms(sent, ALT_ZOOM_MAX_STEPS, 1) && session->refs == 2,
          "a burst is capped at %d steps; every command thread let its session reference go", ALT_ZOOM_MAX_STEPS);
    sent = g_sent;
    wheel(0);
    usleep(50000);
    check(g_sent == sent, "no step, no command");

    altscreen_module_def.airplay.on_session_teardown(session, 1);
    check(refs_become(session, 1), "the session goes as before: %d refs", session->refs);
    CFRelease(session);
    altscreen_module_def.on_shutdown();
    check(!g_zoom_handler && !g_appearance_handler, "and the handlers at shutdown");
}

/* The MMI's night mode (CMD_ALT_APPEARANCE): kept while no session is held and sent after the
 * next showUI; sent at once while one is; always as setNightMode for the cluster display. */
static void night_mode(void) {
    obj *session;
    int sent, i;
    altscreen_module_def.on_init();
    sent = g_sent;
    mmi_night(1);
    usleep(50000);
    check(g_sent == sent, "no session held: the night mode is only kept");
    session = cluster_setup();
    for (i = 0; i < 100 && g_sent < sent + 4; ++i) usleep(10000);
    check(g_sent == sent + 4 && strcmp(g_sent_type[sent], "showUI") == 0
          && strcmp(g_sent_type[sent + 1], "setNightMode") == 0 && g_sent_night[sent + 1] == 0
          && strcmp(g_sent_type[sent + 2], "setNightMode") == 0 && g_sent_night[sent + 2] == 1
          && strcmp(g_sent_uuid[sent + 2], ALT_DISPLAY_UUID) == 0 && strcmp(g_sent_type[sent + 3], "forceKeyFrame") == 0,
          "the next showUI is followed by setNightMode false then true (a real switch) for the cluster "
          "display, then forceKeyFrame");
    sent = g_sent;
    mmi_night(0);
    for (i = 0; i < 100 && (g_sent < sent + 1 || session->refs != 2); ++i) usleep(10000);
    check(g_sent == sent + 1 && strcmp(g_sent_type[sent], "setNightMode") == 0 && g_sent_night[sent] == 0
          && strcmp(g_sent_uuid[sent], ALT_DISPLAY_UUID) == 0 && session->refs == 2,
          "a change while the session is held: setNightMode false at once");
    altscreen_module_def.airplay.on_session_teardown(session, 1);
    check(refs_become(session, 1), "the session goes as before: %d refs", session->refs);
    CFRelease(session);
    altscreen_module_def.on_shutdown();
}

int main(void) {
    int live;
    server_info();
    live = g_live;
    setup_requests();
    setup_replies();
    stream_decryption();
    live_stream();
    torn_down_session();
    wheel_zoom();
    night_mode();
    check(g_live == live, "no CF object leaks: %d live", g_live - live);
    shm_unlink(CVR_SHM_NAME);
    printf("altscreen_hook_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
