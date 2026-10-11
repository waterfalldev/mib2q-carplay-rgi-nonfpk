/* The route-guidance module (hook/routeguidance/rgd_hook.c, included) against a recording fake
 * of the hook bus, fed the phone's start burst as map23 caught it: route state 3, 0, 3, 1 with
 * the maneuver list and the maneuvers, then a reset frame (1 -> 0) and 3, 1 again with nothing
 * resent.  Checks: the route survives that reset - the bus snapshot a late Java client replays
 * still carries the list and the maneuvers, under the route generation Java already has;
 * a repeated identical list keeps it too; a different list, new maneuver data, a reroute's
 * reset or a reset outside the start burst flush it as before. */
#include "routeguidance/rgd_hook.c"
#include "most/check.h"

#include <stdlib.h>

/* ---------------------------------------------------------------- fake bus: the last frame */

static char g_frame[16384];
static int g_frames;

void bus_text_begin_with(bus_text_builder_t *b, const char *object_name, uint8_t *buf, uint32_t cap) {
    (void)object_name;
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->own_buf = false;
    b->overflow = false;
    if (cap) buf[0] = '\0';
}
hook_result_t bus_text_begin_heap(bus_text_builder_t *b, const char *object_name, uint32_t cap) {
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return HOOK_ERR_MEMORY;
    bus_text_begin_with(b, object_name, buf, cap);
    b->own_buf = true;
    return HOOK_OK;
}
static void put(bus_text_builder_t *b, const char *key, const char *value) {
    int n = snprintf((char *)b->buf + b->len, b->cap - b->len, "%s=%s\n", key, value);
    if (n < 0 || (uint32_t)n >= b->cap - b->len) { b->overflow = true; return; }
    b->len += (uint32_t)n;
}
void bus_text_str(bus_text_builder_t *b, const char *key, const char *value) { put(b, key, value ? value : ""); }
void bus_text_int(bus_text_builder_t *b, const char *key, int64_t value) {
    char v[32];
    snprintf(v, sizeof(v), "%lld", (long long)value);
    put(b, key, v);
}
void bus_text_uint(bus_text_builder_t *b, const char *key, uint64_t value) {
    char v[32];
    snprintf(v, sizeof(v), "%llu", (unsigned long long)value);
    put(b, key, v);
}
hook_result_t bus_send_text(uint16_t type, uint8_t flags, bus_text_builder_t *b) {
    (void)flags;
    if (type == EVT_RGD_UPDATE) {
        snprintf(g_frame, sizeof(g_frame), "%.*s", (int)b->len, (const char *)b->buf);
        ++g_frames;
    }
    if (b->own_buf) free(b->buf);
    b->own_buf = false;
    return HOOK_OK;
}
void bus_set_periodic_tick(bus_tick_cb_t cb) { (void)cb; }

/* ---------------------------------------------------------------- fake framework */

hook_result_t hook_inject_message(uint16_t msgid, const uint8_t *payload, size_t payload_len) {
    (void)msgid; (void)payload; (void)payload_len;
    return HOOK_OK;
}
bool hook_is_ready(void) { return true; }
static hook_context_t g_ctx;
hook_context_t *hook_framework_get_context(void) { return &g_ctx; }

/* ---------------------------------------------------------------- the phone's messages */

static uint8_t g_msg[512];
static size_t g_len;

static void begin(uint16_t msgid) {
    g_msg[0] = g_msg[1] = 0x40;
    write_be16(g_msg + 4, msgid);
    g_len = 6;
}
static void tlv(uint16_t id, const uint8_t *value, size_t n) {
    write_be16(g_msg + g_len, (uint16_t)(4 + n));
    write_be16(g_msg + g_len + 2, id);
    memcpy(g_msg + g_len + 4, value, n);
    g_len += 4 + n;
}
static void tlv_u8(uint16_t id, uint8_t v) { tlv(id, &v, 1); }
static void tlv_u16(uint16_t id, uint16_t v) { uint8_t b[2]; write_be16(b, v); tlv(id, b, 2); }
static void send_msg(void) {
    iap2_frame_t frame;
    write_be16(g_msg + 2, (uint16_t)g_len);
    memset(&frame, 0, sizeof(frame));
    frame.msgid = read_be16(g_msg + 4);
    frame.frame_len = (uint16_t)g_len;
    g_ctx.raw_buf = g_msg;
    g_ctx.raw_len = g_len;
    rgd_module_def.on_message(&g_ctx, &frame);
}

/* 0x5201: a route state, with a maneuver list of `n` indices when `list` is given. */
static void update(uint8_t state, const uint16_t *list, int n) {
    int i;
    begin(IAP2_MSG_ROUTE_GUIDANCE_UPDATE);
    tlv_u8(RGD_TLV_ROUTE_GUIDANCE_STATE, state);
    if (list) {
        uint8_t v[2 * MAX_MANEUVER_LIST];
        for (i = 0; i < n; ++i) write_be16(v + 2 * i, list[i]);
        tlv(RGD_TLV_MANEUVER_LIST, v, (size_t)(2 * n));
        tlv_u16(RGD_TLV_MANEUVER_COUNT, 29);
    }
    send_msg();
}
static void maneuver(uint16_t index, uint8_t type) {
    begin(IAP2_MSG_ROUTE_GUIDANCE_MANEUVER);
    tlv_u16(MAN_TLV_INDEX, index);
    tlv_u8(MAN_TLV_TYPE, type);
    send_msg();
}

/* ---------------------------------------------------------------- reading the last frame */

static const char *value(const char *key, char *out, size_t n) {
    char pattern[64];
    const char *at;
    size_t len = 0;
    snprintf(pattern, sizeof(pattern), "%s=", key);
    for (at = g_frame; (at = strstr(at, pattern)) != NULL; ++at)
        if (at == g_frame || at[-1] == '\n') break;
    if (!at) return NULL;
    at += strlen(pattern);
    while (at[len] && at[len] != '\n' && len + 1 < n) ++len;
    memcpy(out, at, len);
    out[len] = '\0';
    return out;
}
/* The type the last frame gives the maneuver in its list's position `pos`, or -1. */
static int listed_type(int pos) {
    char list[128], key[32], v[32];
    int slot = -1, i;
    const char *p;
    if (!value("maneuver_list", list, sizeof(list))) return -1;
    for (i = 0, p = list; i < pos && p; ++i) p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL;
    if (!p || !*p) return -1;
    slot = atoi(p);
    snprintf(key, sizeof(key), "m%d_type", slot);
    return value(key, v, sizeof(v)) ? atoi(v) : -1;
}
static unsigned long long generation(void) {
    char v[32];
    return value("route_generation", v, sizeof(v)) ? strtoull(v, NULL, 10) : 0ULL;
}

/* A new phone session: a fresh subscription, its first message now. */
static void new_session(void) {
    rgd_clear_state("test");
    g_frame[0] = '\0';
}

static const uint16_t k_list[] = { 0, 1 }, k_other[] = { 5, 6 };

/* map23: 3, 0, 3, 1 + list + maneuvers, then 1 -> 0, 3, 1 with nothing resent. */
static unsigned long long burst_to_maneuvers(void) {
    new_session();
    update(3, NULL, 0);
    update(0, NULL, 0);
    update(3, NULL, 0);
    update(1, k_list, 2);
    maneuver(0, 11);
    maneuver(1, 2);
    update(1, NULL, 0);
    return generation();
}

int main(void) {
    unsigned long long before;

    before = burst_to_maneuvers();
    check(listed_type(0) == 11 && listed_type(1) == 2 && before, "the burst's list and maneuvers reach the bus");
    update(0, NULL, 0);
    update(3, NULL, 0);
    update(1, NULL, 0);
    check(listed_type(0) == 11 && listed_type(1) == 2,
          "the start burst's reset frame: the snapshot still carries the list and both maneuvers");
    check(generation() == before, "under the route generation Java already has (%llu, was %llu)", generation(), before);

    burst_to_maneuvers();
    update(0, NULL, 0);
    update(0, NULL, 0);
    update(3, NULL, 0);
    update(1, NULL, 0);
    check(listed_type(0) == 11 && listed_type(1) == 2, "a repeated reset frame keeps the route too (map24)");

    burst_to_maneuvers();
    update(0, NULL, 0);
    update(3, k_list, 2);
    update(1, NULL, 0);
    check(listed_type(0) == 11 && listed_type(1) == 2, "the same list repeated after the reset keeps the route");

    before = burst_to_maneuvers();
    update(0, NULL, 0);
    update(3, k_other, 2);
    check(listed_type(0) == -1 && generation() != before, "a different list after the reset: a new route, flushed");

    burst_to_maneuvers();
    update(0, NULL, 0);
    maneuver(5, 7);
    update(3, NULL, 0);
    check(listed_type(0) == -1, "new maneuver data after the reset: flushed as before");

    burst_to_maneuvers();
    update(5, NULL, 0);
    update(0, NULL, 0);
    update(3, NULL, 0);
    check(listed_type(0) == -1, "a reroute's reset frame flushes as before");

    burst_to_maneuvers();
    g_rgd.first_520x_ms -= RGD_START_BURST_MS + 1;
    update(0, NULL, 0);
    update(3, NULL, 0);
    check(listed_type(0) == -1, "a reset frame after the start burst flushes as before");

    printf("rgd_start_burst_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
