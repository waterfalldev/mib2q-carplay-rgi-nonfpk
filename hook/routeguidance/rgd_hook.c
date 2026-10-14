/*
 * Route Guidance Hook Module Implementation
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#include "rgd_hook.h"
#include <time.h>

DEFINE_LOG_MODULE(RGD);

/*
 * CarPlay route-guidance state=0 handling.
 *
 * iOS Maps uses route_state=0 both for real route end and as a reset
 * frame during reroute:
 *
 *   REROUTING(5) -> NO_ROUTE_SET(0) -> new route/maneuver data -> active
 *
 * Native iAP2 can absorb that reset cheaply.  Our Java side owns BAP and
 * renderer lifecycle, so forwarding the intermediate zero tears down the
 * HUD/VC renderer and causes visible flicker.  Treat state=0 as pending
 * until either:
 *   - hard-clear evidence arrives (disconnect/source_supports_rg=0), or
 *   - active-route evidence arrives (state>0, 0x5202, 0x5204, non-empty
 *     maneuver data), or
 *   - non-reroute iOS silence reaches the fallback deadline.
 *
 * Reroute resets intentionally have no timeout.  With poor connectivity,
 * Maps can sit in REROUTING for a long time; timing that out as "route
 * ended" would be worse than keeping the last stable cluster state.
 */
#define ROUTE_ZERO_NORMAL_GRACE_MS    800

/* A 0x5200 sent while the phone is still restoring a pre-existing Maps route
 * can be accepted without producing the initial 0x5201/2/4 snapshot.  The old
 * one-shot trigger then stayed latched forever.  Retry quickly through the HU /
 * phone start race, then very slowly until the first real RGI message proves
 * that the subscription is alive. */
#define RGD_START_FAST_RETRY_MS       2000
#define RGD_START_SLOW_RETRY_MS      30000
#define RGD_START_FAST_ATTEMPTS          5

/* The phone opens every route subscription with a burst - route state 3, 0, 3, 1, 0, 3, 1
 * within ~60 ms (map23) - whose reset frames came after the maneuvers it never sends again:
 * flushing them left both the replay and a live Java client without maneuvers until the
 * next one.  A reset frame this soon after the first message keeps the route aside, and the
 * route comes back if the state returns before anything new arrives. */
#define RGD_START_BURST_MS            2000

static uint64_t now_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Protects the debounce fields in g_rgd against the heartbeat tick
 * (rgd_periodic_tick, runs on bus heartbeat thread) racing with the
 * iAP2 callback thread inside rgd_message_handler.  Critical sections
 * are tiny (a few stores + a read of monotonic time). */
static pthread_mutex_t g_rgd_debounce_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rgd_start_lock = PTHREAD_MUTEX_INITIALIZER;

/* Forward decl — implemented after rgd_update_t is in scope. */
void rgd_periodic_tick(void);

/* The route content a reset frame flushes: the maneuver map, its caches and the merged
 * 0x5201 snapshot.  Kept aside by a start-burst reset frame (RGD_START_BURST_MS); iAP2
 * handler thread only, apart from rgd_drop_kept_route.
 *
 * MHI3-like maneuver storage mapping:
 * iOS uses monotonically increasing maneuver indexes (can exceed small caches).
 * The MHI3 stack stores a bounded vector and drops oldest when capacity is exceeded.
 *
 * We can't expose arbitrary-key maps through the text bus easily, so we remap iOS maneuver indexes
 * to fixed slots [0..MANEUVER_CACHE_SIZE-1] and publish maneuver_list as slot order. */
typedef struct {
    uint16_t current_list[MAX_MANEUVER_LIST];
    bool current_list_present; /* true if 0x5201 included ManeuverList TLV (can be empty) */
    uint16_t current_list_count;
    uint16_t slot_to_iap_idx[MANEUVER_CACHE_SIZE]; /* 0xFFFF = empty */
    uint32_t slot_seq[MANEUVER_CACHE_SIZE];        /* LRU order for eviction */
    uint32_t slot_ver[MANEUVER_CACHE_SIZE];        /* assignment version (bumps only on reassignment) */
    uint32_t seq_counter;
    uint32_t ver_counter;
    uint64_t route_generation; /* survives slot-version reuse at native route reset */
    /*
     * Lane guidance is keyed by iOS composedGuidanceEventIndex, not by
     * maneuver index.  Keep it in a separate bounded cache so a future
     * maneuver occupying slot 0 cannot mask lane-guidance index 0.
     */
    uint16_t lane_slot_to_iap_idx[MANEUVER_CACHE_SIZE]; /* 0xFFFF = empty */
    uint32_t lane_slot_seq[MANEUVER_CACHE_SIZE];
    uint32_t lane_seq_counter;
    rgd_lane_guidance_t lane_cache[MANEUVER_CACHE_SIZE];
    /*
     * Per-slot maneuver data cache.
     * Cache the last 0x5202 data per slot and re-publish it inside the
     * 0x5201 bus snapshot when maneuver_list is present.  This ensures Java
     * gets slot data atomically with the maneuver_list.
     */
    rgd_maneuver_t slot_cache[MANEUVER_CACHE_SIZE];
    /* Last merged 0x5201 snapshot (used to make bus writes full-state). */
    rgd_update_t update_cache;
} rgd_route_t;

/* Module state */
static struct {
    bool active;
    bool got_520x;
    uint16_t component_id;
    uint64_t last_5200_ms;
    unsigned int start_5200_attempts;
    uint64_t first_520x_ms;   /* this subscription's first message (RGD_START_BURST_MS) */
    bool have_update;
    uint8_t last_route_state;
    /* Monotonic timestamp/deadline for a pending route_state=0.
     * 0 = no pending zero. */
    uint64_t state_zero_started_ms;
    uint64_t state_zero_deadline_ms;
    bool state_zero_from_reroute;
    /* Last route_state actually emitted on the bus (after debounce).
     * Distinct from last_route_state which tracks raw iOS state. */
    uint8_t emitted_route_state;
    rgd_route_t route;
} g_rgd = { .component_id = 0x0010 };

/* Forward declarations */
static bool rgd_message_handler(hook_context_t* ctx, const iap2_frame_t* frame);
static size_t rgd_identify_patcher(hook_context_t* ctx, uint8_t* buf, size_t len, size_t max_len);
static void rgd_state_handler(hook_context_t* ctx, int event);
static void rgd_transport_handler(hook_context_t* ctx, uint16_t msgid);
static void write_bus_update_partial(const rgd_update_t* upd);
static void write_bus_maneuver_partial(const rgd_maneuver_t* man);
static void write_bus_lane_guidance_partial(const rgd_lane_guidance_t* lane);
static void write_bus_snapshot_from_cache(int extra_slot, const rgd_maneuver_t* extra_man,
                                          const rgd_update_t* current_upd);
static void write_slot_data_keys(bus_text_builder_t* b, unsigned slot, const rgd_maneuver_t* man);
static void write_lane_data_keys(bus_text_builder_t* b, unsigned slot, const rgd_lane_guidance_t* lane);
static void write_lane_clear_keys(bus_text_builder_t* b, unsigned slot);
static void rgd_lazy_init(void);

static void rgd_update_cache_reset(void) {
    memset(&g_rgd.route.update_cache, 0, sizeof(g_rgd.route.update_cache));
}

static void rgd_cancel_pending_zero_locked(const char* why, uint8_t active_state) {
    if (g_rgd.state_zero_started_ms == 0) return;
    LOG_INFO(LOG_MODULE, "Route reset canceled by %s (state=%u, reroute=%d)",
             why ? why : "active evidence", active_state,
             g_rgd.state_zero_from_reroute ? 1 : 0);
    g_rgd.state_zero_started_ms = 0;
    g_rgd.state_zero_deadline_ms = 0;
    g_rgd.state_zero_from_reroute = false;
}

static void rgd_cancel_pending_zero(const char* why) {
    pthread_mutex_lock(&g_rgd_debounce_lock);
    rgd_cancel_pending_zero_locked(why, g_rgd.last_route_state);
    pthread_mutex_unlock(&g_rgd_debounce_lock);
}

static bool rgd_update_has_active_evidence(const rgd_update_t* upd) {
    if (!upd) return false;
    if ((upd->present & RGD_UPD_ROUTE_STATE) && upd->route_state > 0) return true;
    if ((upd->present & RGD_UPD_MANEUVER_COUNT) && upd->maneuver_count > 0) return true;
    if ((upd->present & RGD_UPD_MANEUVER_LIST) && upd->maneuver_list_count > 0) return true;
    if ((upd->present & RGD_UPD_DIST_TO_MANEUVER) && upd->dist_to_maneuver > 0) return true;
    if ((upd->present & RGD_UPD_DISTANCE_REMAINING) && upd->distance_remaining > 0) return true;
    if ((upd->present & RGD_UPD_DESTINATION) && upd->destination[0] != '\0') return true;
    if ((upd->present & RGD_UPD_CURRENT_ROAD) && upd->current_road[0] != '\0') return true;
    return false;
}

static void rgd_update_cache_merge(const rgd_update_t* upd) {
    rgd_update_t* c = &g_rgd.route.update_cache;
    if (!upd) return;

    if (upd->present & RGD_UPD_COMPONENT_IDS) {
        c->component_count = upd->component_count;
        if (c->component_count > MAX_COMPONENT_LIST) c->component_count = MAX_COMPONENT_LIST;
        memcpy(c->component_ids, upd->component_ids, sizeof(c->component_ids));
    }
    if (upd->present & RGD_UPD_ROUTE_STATE) c->route_state = upd->route_state;
    if (upd->present & RGD_UPD_MANEUVER_STATE) c->maneuver_state = upd->maneuver_state;
    if (upd->present & RGD_UPD_CURRENT_ROAD) memcpy(c->current_road, upd->current_road, sizeof(c->current_road));
    if (upd->present & RGD_UPD_DESTINATION) memcpy(c->destination, upd->destination, sizeof(c->destination));
    if (upd->present & RGD_UPD_ETA) c->eta = upd->eta;
    if (upd->present & RGD_UPD_TIME_REMAINING) c->time_remaining = upd->time_remaining;
    if (upd->present & RGD_UPD_DISTANCE_REMAINING) c->distance_remaining = upd->distance_remaining;
    if (upd->present & RGD_UPD_DIST_TO_MANEUVER) c->dist_to_maneuver = upd->dist_to_maneuver;
    if (upd->present & RGD_UPD_MANEUVER_COUNT) c->maneuver_count = upd->maneuver_count;
    if (upd->present & RGD_UPD_MANEUVER_LIST) {
        c->maneuver_list_count = upd->maneuver_list_count;
        if (c->maneuver_list_count > MAX_MANEUVER_LIST) c->maneuver_list_count = MAX_MANEUVER_LIST;
        memcpy(c->maneuver_list, upd->maneuver_list, sizeof(c->maneuver_list));
    }
    if (upd->present & RGD_UPD_VISIBLE_IN_APP) c->visible_in_app = upd->visible_in_app;
    if (upd->present & RGD_UPD_LANE_INDEX) c->lane_guidance_index = upd->lane_guidance_index;
    if (upd->present & RGD_UPD_LANE_SLOT) c->lane_guidance_slot = upd->lane_guidance_slot;
    if (upd->present & RGD_UPD_LANE_SHOWING) c->lane_guidance_showing = upd->lane_guidance_showing;
    if (upd->present & RGD_UPD_SOURCE_SUPPORTS_RG) c->source_supports_route_guidance = upd->source_supports_route_guidance;

    c->present |= upd->present;
}

		static void rgd_maneuver_map_reset(void) {
            /* Java may never see a debounced route_state=0. A monotonic-clock
             * generation distinguishes reused slot versions, including a new
             * hook process while the Java session is still alive. */
            uint64_t generation = now_monotonic_ms();
            if (generation <= g_rgd.route.route_generation)
                generation = g_rgd.route.route_generation + 1;
            g_rgd.route.route_generation = generation;
		    g_rgd.route.current_list_present = false;
		    g_rgd.route.current_list_count = 0;
		    g_rgd.route.seq_counter = 0;
		    g_rgd.route.ver_counter = 0;
		    g_rgd.route.lane_seq_counter = 0;
	    for (int i = 0; i < MANEUVER_CACHE_SIZE; i++) {
	        g_rgd.route.slot_to_iap_idx[i] = 0xFFFF;
	        g_rgd.route.slot_seq[i] = 0;
	        g_rgd.route.slot_ver[i] = 0;
	        g_rgd.route.slot_cache[i].present = 0;
	        g_rgd.route.lane_slot_to_iap_idx[i] = 0xFFFF;
	        g_rgd.route.lane_slot_seq[i] = 0;
	        g_rgd.route.lane_cache[i].present = 0;
		    }
		}

static const uint64_t RGD_UPD_WRITE_MASK = RGD_UPD_ROUTE_STATE |
                                            RGD_UPD_MANEUVER_STATE |
                                            RGD_UPD_DISTANCE_REMAINING |
                                            RGD_UPD_DIST_TO_MANEUVER |
                                            RGD_UPD_ETA |
                                            RGD_UPD_TIME_REMAINING |
                                            RGD_UPD_CURRENT_ROAD |
                                            RGD_UPD_DESTINATION |
                                            RGD_UPD_LANE_INDEX |
                                            RGD_UPD_LANE_SLOT |
                                            RGD_UPD_LANE_SHOWING |
                                            RGD_UPD_SOURCE_SUPPORTS_RG |
                                            RGD_UPD_VISIBLE_IN_APP |
                                            RGD_UPD_COMPONENT_IDS |
                                            RGD_UPD_MANEUVER_COUNT |
                                            RGD_UPD_MANEUVER_LIST;

static rgd_route_t g_kept_route;
static volatile bool g_kept_route_valid;

static unsigned rgd_mapped_slots(const uint16_t* map) {
    unsigned n = 0;
    for (int i = 0; i < MANEUVER_CACHE_SIZE; i++) n += map[i] != 0xFFFF;
    return n;
}

/* The update's maneuver list is the kept route's (the phone repeating it). */
static bool rgd_kept_list_is(const rgd_update_t* upd) {
    return g_kept_route_valid && g_kept_route.current_list_present
        && upd->maneuver_list_count == g_kept_route.current_list_count
        && memcmp(upd->maneuver_list, g_kept_route.current_list,
                  upd->maneuver_list_count * sizeof(upd->maneuver_list[0])) == 0;
}

/* Before a start-burst reset frame flushes the route. */
static void rgd_keep_route(void) {
    g_kept_route = g_rgd.route;
    g_kept_route_valid = true;
    LOG_WARN(LOG_MODULE, "Start-burst reset frame: route kept aside (%u maneuvers, list %s%u)",
             rgd_mapped_slots(g_kept_route.slot_to_iap_idx),
             g_kept_route.current_list_present ? "" : "absent/", g_kept_route.current_list_count);
}

/* New route content, a route end or a session end: the kept route is no longer this one. */
static void rgd_drop_kept_route(const char* why) {
    if (!g_kept_route_valid) return;
    g_kept_route_valid = false;
    LOG_WARN(LOG_MODULE, "Kept route dropped: %s", why);
}

/* The route state returned with nothing new since the reset frame: the same route. */
static void rgd_restore_kept_route(void) {
    if (!g_kept_route_valid) return;
    g_rgd.route = g_kept_route;
    g_kept_route_valid = false;
    LOG_WARN(LOG_MODULE, "Route restored after the start-burst reset (%u maneuvers, generation kept)",
             rgd_mapped_slots(g_rgd.route.slot_to_iap_idx));
}

/* The lowest index of the current maneuver list; the caller checked it is not empty. */
static int rgd_min_current_index(void) {
    uint16_t min = g_rgd.route.current_list[0];
    for (uint16_t i = 1; i < g_rgd.route.current_list_count; i++) {
        if (g_rgd.route.current_list[i] < min) min = g_rgd.route.current_list[i];
    }
    return (int)min;
}

static bool rgd_can_process_maneuver_index(uint16_t idx) {
    /* Mirror MHI3 isRouteGuidanceManeuverIndexGreater semantics (simplified):
     * - if no route update exists => reject
     * - if current list is null (ManeuverList TLV not present) => accept
     * - if current list is empty => accept: a transient state=0 clears the list before
     *   0x5202 data arrives for the new route, and iOS never resends 0x5202 for
     *   already-sent indices
     * - if route_state == REROUTING => accept all
     * - else accept if idx >= min(current_list)
     */
    if (!g_rgd.have_update) return false;
    if (!g_rgd.route.current_list_present) return true;
    if (g_rgd.route.current_list_count == 0) return true;
    if (g_rgd.last_route_state == RGD_STATE_REROUTING) return true;
    return (int)idx >= rgd_min_current_index();
}

/*
 * Helper: check if an iAP2 index is in the active ManeuverList.
 * Slots for active maneuvers must never be evicted.
 */
static bool rgd_is_active_index(uint16_t iap_idx) {
    if (!g_rgd.route.current_list_present) return false;
    for (uint16_t i = 0; i < g_rgd.route.current_list_count; i++) {
        if (g_rgd.route.current_list[i] == iap_idx) return true;
    }
    return false;
}

static int rgd_find_slot_for_iap_index_no_touch(uint16_t idx) {
    for (int s = 0; s < MANEUVER_CACHE_SIZE; s++) {
        if (g_rgd.route.slot_to_iap_idx[s] == idx) return s;
    }
    return -1;
}

static bool rgd_is_active_lane_index(uint16_t lane_idx) {
    if ((g_rgd.route.update_cache.present & RGD_UPD_LANE_INDEX) &&
        g_rgd.route.update_cache.lane_guidance_index == lane_idx) {
        return true;
    }

    if (!g_rgd.route.current_list_present) return false;
    for (uint16_t i = 0; i < g_rgd.route.current_list_count; i++) {
        int slot;
        if (g_rgd.route.current_list[i] == lane_idx) return true;
        slot = rgd_find_slot_for_iap_index_no_touch(g_rgd.route.current_list[i]);
        if (slot < 0) continue;
        if ((g_rgd.route.slot_cache[slot].present & RGD_MAN_LINKED_LANE_INDEX) &&
            g_rgd.route.slot_cache[slot].linked_lane_guidance_index == lane_idx) {
            return true;
        }
    }
    return false;
}

/* Find idx in a slot map, refreshing its LRU seq so live slots are not eviction victims.
 * With create, a missing idx takes a free slot, else the oldest slot whose index is not
 * active - the ones on the cluster must survive incoming data for future maneuvers.  If
 * every slot is active, plain LRU applies.  *assigned reports a new assignment. */
static int rgd_assign_slot(uint16_t* map, uint32_t* seq, uint32_t* counter, bool (*active)(uint16_t),
                           uint16_t idx, bool create, bool* assigned) {
    int victim = -1;
    uint32_t best = UINT32_MAX;
    *assigned = false;
    for (int s = 0; s < MANEUVER_CACHE_SIZE; s++) {
        if (map[s] == idx) {
            seq[s] = ++*counter;
            return s;
        }
    }
    if (!create) return -1;
    for (int s = 0; s < MANEUVER_CACHE_SIZE && victim < 0; s++) {
        if (map[s] == 0xFFFF) victim = s;
    }
    for (int pass = 0; pass < 2 && victim < 0; pass++) {
        for (int s = 0; s < MANEUVER_CACHE_SIZE; s++) {
            if (pass == 0 && active(map[s])) continue;
            if (seq[s] < best) {
                best = seq[s];
                victim = s;
            }
        }
    }
    if (victim < 0) victim = 0;
    map[victim] = idx;
    seq[victim] = ++*counter;
    *assigned = true;
    return victim;
}

/* A new assignment bumps the slot version; the caller overwrites the slot's cache. */
static int rgd_slot_for_iap_index(uint16_t idx, bool create) {
    bool assigned;
    int slot = rgd_assign_slot(g_rgd.route.slot_to_iap_idx, g_rgd.route.slot_seq, &g_rgd.route.seq_counter,
                               rgd_is_active_index, idx, create, &assigned);
    if (assigned) g_rgd.route.slot_ver[slot] = ++g_rgd.route.ver_counter;
    return slot;
}

static int rgd_lane_slot_for_iap_index(uint16_t idx, bool create) {
    bool assigned;
    return rgd_assign_slot(g_rgd.route.lane_slot_to_iap_idx, g_rgd.route.lane_slot_seq,
                           &g_rgd.route.lane_seq_counter, rgd_is_active_lane_index, idx, create, &assigned);
}

static void rgd_update_session_active(void) {
    hook_context_t* ctx = hook_framework_get_context();
    if (ctx) {
        ctx->session_active = (g_rgd.active || g_rgd.got_520x);
    }
}

/* Message filter */
static uint16_t rgd_msg_filter[] = {
    IAP2_MSG_ROUTE_GUIDANCE_UPDATE,
    IAP2_MSG_ROUTE_GUIDANCE_MANEUVER,
    IAP2_MSG_ROUTE_GUIDANCE_LANE
};

/* Module definition.  Route guidance is pure iAP2: it wants Identify, three
 * 0x52xx messages, the session state edges and outgoing transport frames, and
 * nothing from the AirPlay side at all. */
const hook_module_def_t rgd_module_def = {
    .name = "routeguidance",
    .msg_filter = rgd_msg_filter,
    .msg_filter_count = sizeof(rgd_msg_filter) / sizeof(rgd_msg_filter[0]),
    .on_message = rgd_message_handler,
    .on_identify = rgd_identify_patcher,
    .on_state = rgd_state_handler,
    .on_transport_send = rgd_transport_handler,
    .on_shutdown = rgd_shutdown,
};

/* Bus snapshot writer.  Accumulates all state into a single EVT_RGD_UPDATE
 * text frame so Java sees it atomically. */
static void write_bus_update_partial(const rgd_update_t* upd) {
    if (!upd) return;
    if (upd->present == 0) return;
    if ((upd->present & RGD_UPD_WRITE_MASK) == 0) return;
    rgd_update_t enriched = *upd;
    if (enriched.present & RGD_UPD_LANE_INDEX) {
        int slot = rgd_lane_slot_for_iap_index(enriched.lane_guidance_index, false);
        enriched.present |= RGD_UPD_LANE_SLOT;
        enriched.lane_guidance_slot = (slot >= 0) ? (int16_t)slot : (int16_t)-1;
    }
    rgd_update_cache_merge(&enriched);
    write_bus_snapshot_from_cache(-1, NULL, &enriched);
}

/* ---- Slot keys: "<slot><idx>_<name>" for the maneuver (m) and lane-guidance (lg) caches.
 * write_slot_data_keys is called from inside write_bus_snapshot_from_cache, so maneuver_list
 * and slot data ship in a single EVT_RGD_UPDATE frame, atomic to Java. */
static void key_int(bus_text_builder_t* b, const char* slot, unsigned idx, const char* name, int64_t value) {
    char key[64];
    snprintf(key, sizeof(key), "%s%u_%s", slot, idx, name);
    bus_text_int(b, key, value);
}

static void key_str(bus_text_builder_t* b, const char* slot, unsigned idx, const char* name, const char* value) {
    char key[64];
    snprintf(key, sizeof(key), "%s%u_%s", slot, idx, name);
    bus_text_str(b, key, value);
}

/* Appends at *off, never past size: a list too long is cut, not overrun. */
static void appendf(char* buf, size_t size, size_t* off, const char* fmt, ...) {
    va_list ap;
    int n;
    if (*off + 1 >= size) return;
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, size - *off, fmt, ap);
    va_end(ap);
    if (n > 0) *off = (size_t)n < size - *off ? *off + (size_t)n : size - 1;
}

enum lane_field { LANE_POSITION, LANE_DIRECTION, LANE_STATUS };

static void key_lane_field(bus_text_builder_t* b, const char* slot, unsigned idx, const char* name,
                           const rgd_lane_t* lanes, int count, enum lane_field field) {
    char buf[128];
    size_t off = 0;
    buf[0] = '\0';
    for (int j = 0; j < count && j < MAX_LANE_GUIDANCE; j++) {
        int v = field == LANE_POSITION ? (int)lanes[j].position
              : field == LANE_DIRECTION ? (int)lanes[j].direction : (int)lanes[j].status;
        appendf(buf, sizeof(buf), &off, "%s%d", j > 0 ? "," : "", v);
    }
    key_str(b, slot, idx, name, buf);
}

/* A slot's lanes: count, positions, directions, status and angles - each lane's angles
 * comma-separated, lanes separated by '|' (e.g. "1000|40,20|").  A count of 0 or -1 sends
 * empty lists. */
static void key_lanes(bus_text_builder_t* b, const char* slot, unsigned idx, const rgd_lane_t* lanes, int count) {
    char buf[1200];
    size_t off = 0;
    key_int(b, slot, idx, "lane_count", count);
    key_lane_field(b, slot, idx, "lane_positions", lanes, count, LANE_POSITION);
    key_lane_field(b, slot, idx, "lane_directions", lanes, count, LANE_DIRECTION);
    key_lane_field(b, slot, idx, "lane_status", lanes, count, LANE_STATUS);
    buf[0] = '\0';
    for (int j = 0; j < count && j < MAX_LANE_GUIDANCE; j++) {
        if (j > 0) appendf(buf, sizeof(buf), &off, "|");
        for (int k = 0; k < lanes[j].angle_count && k < MAX_LANE_ANGLES; k++)
            appendf(buf, sizeof(buf), &off, "%s%d", k > 0 ? "," : "", (int)lanes[j].angles[k]);
    }
    key_str(b, slot, idx, "lane_angles", buf);
}

static void write_slot_data_keys(bus_text_builder_t* b, unsigned idx, const rgd_maneuver_t* man) {
    if (man->present & RGD_MAN_TYPE) key_int(b, "m", idx, "type", man->maneuver_type);
    if (man->present & RGD_MAN_EXIT_ANGLE) key_int(b, "m", idx, "exit_angle", man->exit_angle);
    if (man->present & RGD_MAN_JUNCTION_TYPE) key_int(b, "m", idx, "junction_type", man->junction_type);
    if (man->present & RGD_MAN_DRIVING_SIDE) key_int(b, "m", idx, "driving_side", man->driving_side);
    if (man->present & RGD_MAN_DISTANCE_BETWEEN) key_int(b, "m", idx, "distance", man->distance_between);
    if (man->present & RGD_MAN_DESCRIPTION) key_str(b, "m", idx, "name", man->description);
    if (man->present & RGD_MAN_AFTER_ROAD) key_str(b, "m", idx, "after_road", man->after_road_name);
    if (man->present & RGD_MAN_JUNCTION_ANGLES) {
        char buf[128];
        size_t off = 0;
        buf[0] = '\0';
        for (int j = 0; j < man->junction_angle_count; j++)
            appendf(buf, sizeof(buf), &off, "%s%d", j > 0 ? "," : "", (int)man->junction_angles[j]);
        key_str(b, "m", idx, "junction_angles", buf);
    }
    /* Slot version: bumps only when a different iOS maneuver is assigned to this slot.
     * Java uses this to detect maneuver transitions even when type/angles are identical.
     * Separate from slot_seq (LRU) which bumps on every data update. */
    key_int(b, "m", idx, "ver", (g_rgd.route.slot_ver[idx] % 65535) + 1);
#if RGD_TRACE_RAW_FULL
    if (man->present & RGD_MAN_LANE_GUIDANCE_RAW) key_str(b, "m", idx, "lane_guidance_hex", man->lane_guidance_hex);
#endif
    if (man->present & RGD_MAN_LINKED_LANE_INDEX) {
        int linked_slot = rgd_lane_slot_for_iap_index(man->linked_lane_guidance_index, false);
        key_int(b, "m", idx, "linked_lane_guidance_index", man->linked_lane_guidance_index);
        if (linked_slot >= 0) key_int(b, "m", idx, "linked_lane_guidance_slot", linked_slot);
    }
    /* A raw linked-lane payload alone still sends the lane keys, empty, to keep them coherent. */
    if (man->present & RGD_MAN_LANE_GUIDANCE_RAW) key_lanes(b, "m", idx, NULL, 0);
#if RGD_TRACE_RAW_FULL
    if (man->present & RGD_MAN_EXIT_INFO_RAW) key_str(b, "m", idx, "exit_info_hex", man->exit_info_hex);
#endif
    if (man->present & RGD_MAN_EXIT_INFO_STR) key_str(b, "m", idx, "exit_info", man->exit_info_str);
}

static void write_lane_data_keys(bus_text_builder_t* b, unsigned idx, const rgd_lane_guidance_t* lane) {
    if (!lane || !(lane->present & RGD_LANE_GUIDANCE_INDEX) || !(lane->present & RGD_LANE_INFORMATIONS)) return;
    key_int(b, "lg", idx, "index", lane->lane_guidance_index);
    key_int(b, "lg", idx, "lane_complete", lane->lane_complete);
    key_lanes(b, "lg", idx, lane->lanes, lane->lane_count);
}

static void write_lane_clear_keys(bus_text_builder_t* b, unsigned idx) {
    key_int(b, "lg", idx, "index", -1);
    key_int(b, "lg", idx, "lane_complete", 0);
    key_lanes(b, "lg", idx, NULL, -1);
}

static bool rgd_lane_slot_has_data(int slot) {
    if (slot < 0 || slot >= MANEUVER_CACHE_SIZE) return false;
    return (g_rgd.route.lane_cache[slot].present & (RGD_LANE_GUIDANCE_INDEX | RGD_LANE_INFORMATIONS)) ==
           (RGD_LANE_GUIDANCE_INDEX | RGD_LANE_INFORMATIONS);
}

static bool rgd_prune_stale_lane_cache(void) {
    int min_idx;
    bool pruned = false;

    if (!g_rgd.route.current_list_present) return false;
    if (g_rgd.route.current_list_count == 0) return false;
    if (g_rgd.last_route_state == RGD_STATE_REROUTING) return false;

    min_idx = rgd_min_current_index();
    for (int i = 0; i < MANEUVER_CACHE_SIZE; i++) {
        uint16_t lane_idx = g_rgd.route.lane_slot_to_iap_idx[i];
        if (lane_idx == 0xFFFF) continue;
        if ((int)lane_idx >= min_idx) continue;
        if ((g_rgd.route.update_cache.present & RGD_UPD_LANE_INDEX) &&
            g_rgd.route.update_cache.lane_guidance_index == lane_idx) {
            continue;
        }

        g_rgd.route.lane_slot_to_iap_idx[i] = 0xFFFF;
        g_rgd.route.lane_slot_seq[i] = 0;
        g_rgd.route.lane_cache[i].present = 0;
        pruned = true;
    }
    return pruned;
}

static void write_bus_maneuver_partial(const rgd_maneuver_t* man) {
    if (!man) return;
    if (man->present == 0) return;
    if (!(man->present & RGD_MAN_INDEX)) return;

    uint64_t write_mask = RGD_MAN_TYPE |
                          RGD_MAN_EXIT_ANGLE |
                          RGD_MAN_JUNCTION_TYPE |
                          RGD_MAN_DRIVING_SIDE |
                          RGD_MAN_DISTANCE_BETWEEN |
                          RGD_MAN_DESCRIPTION |
                          RGD_MAN_AFTER_ROAD |
                          RGD_MAN_JUNCTION_ANGLES |
                          RGD_MAN_LANE_GUIDANCE_RAW |
                          RGD_MAN_LINKED_LANE_INDEX |
                          RGD_MAN_EXIT_INFO_RAW |
                          RGD_MAN_EXIT_INFO_STR;
    if ((man->present & write_mask) == 0) return;

    unsigned iap_idx = man->index;
    if (!rgd_can_process_maneuver_index((uint16_t)iap_idx)) return;
    int slot = rgd_slot_for_iap_index((uint16_t)iap_idx, true);
    if (slot < 0) return;

    /* Cache for re-publish on 0x5201 */
    g_rgd.route.slot_cache[slot] = *man;

    write_bus_snapshot_from_cache(slot, &g_rgd.route.slot_cache[slot], NULL);
}

static void write_bus_lane_guidance_partial(const rgd_lane_guidance_t* lane) {
    if (!lane) return;
    if ((lane->present & (RGD_LANE_GUIDANCE_INDEX | RGD_LANE_INFORMATIONS)) == 0) return;

    rgd_update_t upd;
    bool have_upd = false;
    memset(&upd, 0, sizeof(upd));

    uint16_t iap_idx = 0xFFFF;
    if (lane->present & RGD_LANE_GUIDANCE_INDEX) {
        iap_idx = lane->lane_guidance_index;
    } else if (g_rgd.route.current_list_present && g_rgd.route.current_list_count > 0) {
        /*
         * Fallback when 0x5204 omits laneGuidanceIndex: attach to current primary maneuver.
         * Native path usually provides index explicitly.
         */
        iap_idx = g_rgd.route.current_list[0];
    }

    bool have_slot = false;
    int slot = -1;
    if (lane->present & RGD_LANE_INFORMATIONS) {
        if (iap_idx != 0xFFFF) {
            slot = rgd_lane_slot_for_iap_index(iap_idx, true);
            if (slot >= 0) {
                rgd_lane_guidance_t* dst = &g_rgd.route.lane_cache[slot];
                *dst = *lane;
                dst->lane_guidance_index = iap_idx;
                dst->present |= RGD_LANE_GUIDANCE_INDEX | RGD_LANE_INFORMATIONS;

                have_slot = true;
            }
        }
    }

    if (iap_idx != 0xFFFF) {
        int publish_slot = (slot >= 0) ? slot : rgd_lane_slot_for_iap_index(iap_idx, false);
        /*
         * 0x5204's TLV1 (LANE_GUIDANCE_INDEX) is iOS's
         * composedGuidanceEventIndex — the identifier of WHICH lane event
         * this packet describes, NOT a flag that this is the active one.
         * iOS Maps.app's CarMetadataNavigationListener routinely batch-
         * sends 0x5204 for every event in the route (including future
         * precache) via CRAccNavController::sendLaneGuidances, while the
         * active is set separately via setCurrentLaneGuidanceIndex: ->
         * 0x5201 InfoType 16.  Advancing Java's active from 0x5204 alone
         * would flip the displayed lanes to a future event prematurely.
         *
         * Only refresh lane_guidance_slot when this 0x5204's iap_idx
         * already matches the cached active.  Otherwise keep the cache
         * fresh (lgN_* keys), do not touch top-level slot/index.
         */
        if ((g_rgd.route.update_cache.present & RGD_UPD_LANE_INDEX) &&
            g_rgd.route.update_cache.lane_guidance_index == iap_idx) {
            upd.present |= RGD_UPD_LANE_SLOT;
            upd.lane_guidance_slot = (publish_slot >= 0) ? (int16_t)publish_slot : (int16_t)-1;
            have_upd = true;
        }
    }

    if (!have_upd && !have_slot) return;

    if (have_upd) {
        rgd_update_cache_merge(&upd);
    }

    if (have_slot) {
        write_bus_snapshot_from_cache(slot, &g_rgd.route.slot_cache[slot], have_upd ? &upd : NULL);
    } else {
        write_bus_snapshot_from_cache(-1, NULL, &upd);
    }
}

static void write_bus_snapshot_from_cache(int extra_slot, const rgd_maneuver_t* extra_man,
                                          const rgd_update_t* current_upd) {
    const rgd_update_t* upd = &g_rgd.route.update_cache;
    uint64_t present = upd->present & RGD_UPD_WRITE_MASK;
    bool have_extra_slot = (extra_man && extra_man->present != 0 &&
                            extra_slot >= 0 && extra_slot < MANEUVER_CACHE_SIZE);
    bool have_lane_cache = false;
    bool pruned_lane_slots;

    pruned_lane_slots = rgd_prune_stale_lane_cache();

    for (int i = 0; i < MANEUVER_CACHE_SIZE && !have_lane_cache; i++)
        have_lane_cache = rgd_lane_slot_has_data(i);
    if (present == 0 && !have_extra_slot && !have_lane_cache && !pruned_lane_slots) return;

    /* NOTE on storage choice: a previous version used `static __thread`
     * 64K scratch to avoid per-snapshot malloc/free.  This crashed
     * dio_manager — gcc 4.4 on QNX 6.5 emits __thread as emulated TLS
     * (__emutls_v.* + __emutls_get_address) which is buggy in
     * LD_PRELOAD'd .so on this target.  Reverted to heap allocation. */
    bus_text_builder_t _b_storage;
    bus_text_builder_t* b = &_b_storage;
    if (bus_text_begin_heap(b, "routeguidance", BUS_TEXT_BUILDER_LARGE_CAP) != HOOK_OK) {
        LOG_WARN(LOG_MODULE, "snapshot: text builder alloc failed");
        return;
    }

    bus_text_uint(b, "route_generation", g_rgd.route.route_generation);
    if (present & RGD_UPD_ROUTE_STATE)
        bus_text_int(b, "route_state", upd->route_state);
    if (present & RGD_UPD_MANEUVER_STATE)
        bus_text_int(b, "maneuver_state", upd->maneuver_state);
    if (present & RGD_UPD_DISTANCE_REMAINING)
        bus_text_uint(b, "dist_dest_m", upd->distance_remaining);
    if (present & RGD_UPD_DIST_TO_MANEUVER)
        bus_text_uint(b, "dist_maneuver_m", upd->dist_to_maneuver);
    if (present & RGD_UPD_ETA)
        bus_text_uint(b, "eta_seconds", upd->eta);
    if (present & RGD_UPD_TIME_REMAINING)
        bus_text_uint(b, "time_remaining_seconds", upd->time_remaining);
    if (present & RGD_UPD_CURRENT_ROAD)
        bus_text_str(b, "current_road", upd->current_road);
    if (present & RGD_UPD_DESTINATION)
        bus_text_str(b, "destination", upd->destination);
    if (present & RGD_UPD_LANE_INDEX) {
        bus_text_int(b, "lane_guidance_index", upd->lane_guidance_index);
        /* Prefer lookup from the current lane_guidance_index.  RGD_UPD_LANE_SLOT
         * is a bus-only helper and can be stale if cache pruning/remap happened
         * after the update was merged. */
        bus_text_int(b, "lane_guidance_slot", rgd_lane_slot_for_iap_index(upd->lane_guidance_index, false));
    } else if (present & RGD_UPD_LANE_SLOT) {
        bus_text_int(b, "lane_guidance_slot", upd->lane_guidance_slot);
    }
    if (present & RGD_UPD_LANE_SHOWING)
        bus_text_int(b, "lane_guidance_showing", upd->lane_guidance_showing);
    if (present & RGD_UPD_SOURCE_SUPPORTS_RG)
        bus_text_int(b, "source_supports_rg", upd->source_supports_route_guidance);
    if (present & RGD_UPD_VISIBLE_IN_APP)
        bus_text_int(b, "visible_in_app", upd->visible_in_app);
    if (present & RGD_UPD_COMPONENT_IDS) {
        char list_buf[128];
        size_t loff = 0;
        list_buf[0] = '\0';
        bus_text_int(b, "component_count", upd->component_count);
        for (uint16_t i = 0; i < upd->component_count; i++)
            appendf(list_buf, sizeof(list_buf), &loff, "%s%u", i > 0 ? "," : "", (unsigned)upd->component_ids[i]);
        bus_text_str(b, "component_ids", list_buf);
    }
    if (present & RGD_UPD_MANEUVER_COUNT)
        bus_text_int(b, "maneuver_count", upd->maneuver_count);

    int listed_slots[MAX_MANEUVER_LIST];
    int listed_count = 0;
    /*
     * Maneuver list persistence:
     * - 0x5201 with ManeuverList TLV present updates current_list (including explicit empty list)
     * - 0x5201 without ManeuverList TLV keeps previous current_list
     * - 0x5202 writes (current_upd == NULL) also keep previous current_list
     *
     * This avoids publishing a transient empty maneuver_list during partial 0x5201 updates.
     */
    if (current_upd && (current_upd->present & RGD_UPD_MANEUVER_LIST)) {
        g_rgd.route.current_list_present = true;
        g_rgd.route.current_list_count = 0;
        for (uint16_t i = 0; i < current_upd->maneuver_list_count && g_rgd.route.current_list_count < MAX_MANEUVER_LIST; i++) {
            g_rgd.route.current_list[g_rgd.route.current_list_count++] = current_upd->maneuver_list[i];
        }
        /* The slot cache is not flushed when the list shrinks or goes backwards: iOS never
         * resends 0x5202 for indices it already sent, so a flush would orphan the slot data.
         * Only a route_state 0 reset (rgd_maneuver_map_reset) clears it. */
    }

    if (rgd_prune_stale_lane_cache())
        pruned_lane_slots = true;

    if (g_rgd.route.current_list_present) {
        /* An empty list is a real clear: either the source sent one, or every ManeuverList
         * index lacks cached type data (evicted), so Java must not keep the previous maneuver. */
        char list_buf[128];
        size_t loff = 0;
        list_buf[0] = '\0';
        for (uint16_t i = 0; i < g_rgd.route.current_list_count; i++) {
            /* Snapshot writer: pure lookup, no LRU touch, no allocation.
             * Bumping seq here on every PPS write demoted real-time-used
             * slots to eviction victims; creating a slot from snapshot
             * (idx not yet seen via 0x5202) would publish an empty slot. */
            int slot = rgd_find_slot_for_iap_index_no_touch(g_rgd.route.current_list[i]);
            if (slot < 0) continue;
            if (!(g_rgd.route.slot_cache[slot].present & RGD_MAN_TYPE)) continue;
            appendf(list_buf, sizeof(list_buf), &loff, "%s%u", listed_count > 0 ? "," : "", (unsigned)slot);
            listed_slots[listed_count++] = slot;
        }
        bus_text_str(b, "maneuver_list", list_buf);
    }

    for (int i = 0; i < listed_count; i++)
        write_slot_data_keys(b, (unsigned)listed_slots[i], &g_rgd.route.slot_cache[listed_slots[i]]);

    for (int i = 0; i < MANEUVER_CACHE_SIZE; i++) {
        if (rgd_lane_slot_has_data(i)) {
            write_lane_data_keys(b, (unsigned)i, &g_rgd.route.lane_cache[i]);
        } else {
            write_lane_clear_keys(b, (unsigned)i);
        }
    }

    /*
     * Extra slot (from 0x5202) that is already in the listed maneuver_list
     * was written above.  Do NOT write non-listed extra slots: Java only
     * consumes slots present in maneuver_list, and the extra data bloats
     * the bus payload.
     * The cached slot data will be included automatically when the next
     * 0x5201 adds the slot to maneuver_list.
     */

    bus_send_text(EVT_RGD_UPDATE, BUS_FLAG_STICKY, b);
}

void rgd_clear_state(const char* reason) {
    g_rgd.active = false;
    pthread_mutex_lock(&g_rgd_start_lock);
    g_rgd.got_520x = false;
    g_rgd.last_5200_ms = 0;
    g_rgd.start_5200_attempts = 0;
    g_rgd.first_520x_ms = 0;
    pthread_mutex_unlock(&g_rgd_start_lock);
    rgd_drop_kept_route("state cleared");
    g_rgd.have_update = false;
    g_rgd.last_route_state = 0;
    /* Disconnect bypasses time-based debounce — emit state=0 immediately
     * regardless of debounce window, and reset bookkeeping for next session.
     * Lock guards against concurrent rgd_periodic_tick() (heartbeat thread)
     * reading these fields. */
    pthread_mutex_lock(&g_rgd_debounce_lock);
    g_rgd.state_zero_started_ms = 0;
    g_rgd.state_zero_deadline_ms = 0;
    g_rgd.state_zero_from_reroute = false;
    g_rgd.emitted_route_state = 0;
    pthread_mutex_unlock(&g_rgd_debounce_lock);
    rgd_maneuver_map_reset();
    rgd_update_cache_reset();

    {
        bus_text_builder_t _b_storage;
        bus_text_builder_t* b = &_b_storage;
        uint8_t scratch[256];
        bus_text_begin_with(b, "routeguidance", scratch, sizeof(scratch));
        bus_text_uint(b, "route_generation", g_rgd.route.route_generation);
        bus_text_int(b, "route_state", RGD_STATE_NO_ROUTE_SET);
        bus_text_int(b, "maneuver_count", 0);
        if (reason) bus_text_str(b, "disconnect_reason", reason);
        bus_send_text(EVT_RGD_UPDATE, BUS_FLAG_STICKY, b);
    }

    rgd_update_session_active();

    LOG_INFO(LOG_MODULE, "State cleared: %s", reason ? reason : "unknown");
}

hook_result_t rgd_request_updates(void) {
    uint64_t now = now_monotonic_ms();
    uint64_t retry_ms;
    unsigned int attempt;

    /* Serialize the transport callback and 1 Hz heartbeat.  Claim the retry
     * timestamp before injection so both threads cannot emit the same 0x5200. */
    pthread_mutex_lock(&g_rgd_start_lock);
    if (!hook_is_ready()) {
        pthread_mutex_unlock(&g_rgd_start_lock);
        return HOOK_ERR_INIT;
    }
    if (g_rgd.got_520x) {
        pthread_mutex_unlock(&g_rgd_start_lock);
        return HOOK_OK;
    }
    retry_ms = (g_rgd.start_5200_attempts < RGD_START_FAST_ATTEMPTS)
             ? RGD_START_FAST_RETRY_MS : RGD_START_SLOW_RETRY_MS;
    if (g_rgd.last_5200_ms != 0 && now - g_rgd.last_5200_ms < retry_ms) {
        pthread_mutex_unlock(&g_rgd_start_lock);
        return HOOK_OK;
    }
    g_rgd.last_5200_ms = now;
    attempt = ++g_rgd.start_5200_attempts;
    pthread_mutex_unlock(&g_rgd_start_lock);

    /*
     * Mirror native libesoiap2 0x5200 shape:
     * - 0x0000 component ID (u16)
     * - 0x0001 sourceName request (presence TLV, empty payload)
     * - 0x0002 sourceSupportsRouteGuidance request (presence TLV)
     * - 0x0003 supportsExitInfo request (presence TLV)
     */
    const uint8_t tlv[] = {
        0, 6, 0, RGD_START_TLV_COMPONENT_ID, (uint8_t)(g_rgd.component_id >> 8), (uint8_t)g_rgd.component_id,
        0, 4, 0, RGD_START_TLV_SOURCE_NAME,
        0, 4, 0, RGD_START_TLV_SOURCE_SUPPORTS_RG,
        0, 4, 0, RGD_START_TLV_SUPPORTS_EXIT_INFO,
    };

    hook_result_t res = hook_inject_message(IAP2_MSG_ROUTE_GUIDANCE_START, tlv, sizeof(tlv));
    if (res == HOOK_OK) {
        g_rgd.active = true;
        rgd_update_session_active();
        LOG_INFO(LOG_MODULE, "Sent 0x5200 attempt=%u (component=0x%04X, opts=source_name+source_supports_rg+exit_info)",
                 attempt, g_rgd.component_id);
    } else {
        /* A transport failure was not a real attempt.  Make the next heartbeat
         * retry immediately instead of waiting for the backoff interval. */
        pthread_mutex_lock(&g_rgd_start_lock);
        if (g_rgd.start_5200_attempts > 0) --g_rgd.start_5200_attempts;
        g_rgd.last_5200_ms = 0;
        pthread_mutex_unlock(&g_rgd_start_lock);
    }
    return res;
}

#if RGD_TRACE_RAW_FULL
/* Raw packet full log before parsing.  INFO, so it needs the carplay_verbose marker too. */
static void rgd_log_raw_packet(const char* label, const uint8_t* data, size_t len) {
    if (!data || len == 0 || !label) return;
    LOG_INFO(LOG_MODULE, "%s len=%zu", label, len);

    for (size_t off = 0; off < len; off += 16) {
        char line[128];
        int pos = snprintf(line, sizeof(line), "%s %04zx:", label, off);
        for (size_t i = 0; i < 16 && (off + i) < len && pos < (int)sizeof(line) - 4; i++) {
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %02X", data[off + i]);
        }
        log_write(LOG_LEVEL_INFO, LOG_MODULE, "%s", line);
    }
}
#endif

static void rgd_mark_first_response(uint16_t msgid) {
    bool first = false;
    pthread_mutex_lock(&g_rgd_start_lock);
    if (!g_rgd.got_520x) {
        g_rgd.got_520x = true;
        g_rgd.first_520x_ms = now_monotonic_ms();
        first = true;
    }
    pthread_mutex_unlock(&g_rgd_start_lock);
    if (first) {
        rgd_update_session_active();
        LOG_INFO(LOG_MODULE,
                 "*** FIRST RouteGuidance message received! msgid=0x%04X; 0x5200 retry stopped ***",
                 msgid);
    }
}

/* Message handler - incoming 0x5201/0x5202/0x5204 */
static bool rgd_message_handler(hook_context_t* ctx, const iap2_frame_t* frame) {
    rgd_lazy_init();  /* Ensure bus/session state is ready */

    if (frame->msgid == IAP2_MSG_ROUTE_GUIDANCE_UPDATE) {
#if RGD_TRACE_RAW_FULL
        rgd_log_raw_packet("RGD 0x5201 raw", frame->data, frame->frame_len);
#endif

        rgd_update_t upd;
        if (!rgd_parse_update(frame->data, frame->frame_len, &upd)) {
            LOG_WARN(LOG_MODULE, "Ignored malformed RGD message 0x%04X", frame->msgid);
            return false; /* Preserve stock dispatch; publish no partial delta. */
        }
        rgd_mark_first_response(frame->msgid);

        LOG_INFO(LOG_MODULE, "Update: state=%u road=\"%s\" dest=\"%s\"",
                 upd.route_state, upd.current_road, upd.destination);

        /*
         * Track update presence and handle route_state transitions.
         *
         * CarPlay-aware zero handling:
         * - state=0 is a pending reset frame, not immediately a route end.
         * - state=0 after REROUTING has no timeout; bad connectivity can
         *   keep Maps in reroute for a long time.
         * - any active route evidence cancels the pending zero.
         * - hard clear still bypasses this path via rgd_clear_state() or
         *   source_supports_rg=0.
         */
        g_rgd.have_update = true;
        if ((upd.present & RGD_UPD_MANEUVER_LIST) && !rgd_kept_list_is(&upd))
            rgd_drop_kept_route("a new maneuver list");
        bool suppress_update = false;
        bool hard_clear = (upd.present & RGD_UPD_SOURCE_SUPPORTS_RG)
                       && upd.source_supports_route_guidance == 0;
        bool active_evidence = rgd_update_has_active_evidence(&upd);

        if (upd.present & RGD_UPD_ROUTE_STATE) {
            pthread_mutex_lock(&g_rgd_debounce_lock);
            uint8_t prev_state = g_rgd.last_route_state;
            g_rgd.last_route_state = upd.route_state;

            if (upd.route_state == 0) {
                if (hard_clear) {
                    LOG_INFO(LOG_MODULE, "Route hard clear: source_supports_rg=0");
                    rgd_drop_kept_route("hard clear");
                    rgd_update_cache_reset();
                    rgd_maneuver_map_reset();
                    rgd_cancel_pending_zero_locked("hard clear", 0);
                    g_rgd.emitted_route_state = 0;
                } else if (g_rgd.emitted_route_state == 0) {
                    /* Already emitted state=0 (or never emitted anything yet) —
                     * pass through.  Re-emitting state=0 is a no-op for Java. */
                } else {
                    uint64_t now = now_monotonic_ms();
                    bool from_reroute = (prev_state == RGD_STATE_REROUTING)
                                     || (g_rgd.emitted_route_state == RGD_STATE_REROUTING);

                    /* A repeated state=0 frame (map24: two in a row) leaves the kept route. */
                    if (prev_state > 0) {
                        if (!from_reroute && g_rgd.first_520x_ms
                                && now - g_rgd.first_520x_ms <= RGD_START_BURST_MS)
                            rgd_keep_route();
                        else
                            rgd_drop_kept_route("reset frame outside the start burst");
                    }
                    rgd_update_cache_reset();
                    if (prev_state > 0) {
                        LOG_INFO(LOG_MODULE,
                                 "Route reset frame (state %u->0): flushing slot cache, holding Java update",
                                 prev_state);
                        rgd_maneuver_map_reset();
                    }
                    if (g_rgd.state_zero_started_ms == 0) {
                        g_rgd.state_zero_started_ms = now;
                        g_rgd.state_zero_from_reroute = from_reroute;
                        g_rgd.state_zero_deadline_ms = from_reroute
                            ? 0
                            : (now + ROUTE_ZERO_NORMAL_GRACE_MS);
                    }
                    suppress_update = true;
                }
            } else {
                /* state > 0 */
                if (g_rgd.state_zero_started_ms != 0) {
                    rgd_restore_kept_route();       /* none kept after a different list (above) */
                    rgd_cancel_pending_zero_locked("route_state>0", upd.route_state);
                }
                g_rgd.emitted_route_state = upd.route_state;
            }
            pthread_mutex_unlock(&g_rgd_debounce_lock);
        } else if (active_evidence) {
            rgd_cancel_pending_zero("active 0x5201");
        } else if (!hard_clear) {
            pthread_mutex_lock(&g_rgd_debounce_lock);
            if (g_rgd.state_zero_started_ms != 0) {
                suppress_update = true;
            }
            pthread_mutex_unlock(&g_rgd_debounce_lock);
        }
        if (!suppress_update)
            write_bus_update_partial(&upd);
    }
    else if (frame->msgid == IAP2_MSG_ROUTE_GUIDANCE_MANEUVER) {
#if RGD_TRACE_RAW_FULL
        rgd_log_raw_packet("RGD 0x5202 raw", frame->data, frame->frame_len);
#endif

        rgd_maneuver_t man;
        if (!rgd_parse_maneuver(frame->data, frame->frame_len, &man)) {
            LOG_WARN(LOG_MODULE, "Ignored malformed RGD message 0x%04X", frame->msgid);
            return false; /* Preserve stock dispatch; publish no partial delta. */
        }
        rgd_mark_first_response(frame->msgid);

        LOG_INFO(LOG_MODULE, "Maneuver: idx=%u type=%u desc=\"%s\"",
                 man.index, man.maneuver_type, man.description);
        rgd_drop_kept_route("new maneuver data");
        rgd_cancel_pending_zero("0x5202 maneuver");
        write_bus_maneuver_partial(&man);
    }
    else if (frame->msgid == IAP2_MSG_ROUTE_GUIDANCE_LANE) {
#if RGD_TRACE_RAW_FULL
        rgd_log_raw_packet("RGD 0x5204 raw", frame->data, frame->frame_len);
#endif

        rgd_lane_guidance_t lane;
        if (!rgd_parse_lane_guidance(frame->data, frame->frame_len, &lane)) {
            LOG_WARN(LOG_MODULE, "Ignored malformed RGD message 0x%04X", frame->msgid);
            return false; /* Preserve stock dispatch; publish no partial delta. */
        }
        rgd_mark_first_response(frame->msgid);

        LOG_INFO(LOG_MODULE, "Lane guidance: idx=%u lanes=%u", lane.lane_guidance_index, lane.lane_count);
        rgd_drop_kept_route("new lane data");
        rgd_cancel_pending_zero("0x5204 lane");
        for (int li = 0; log_verbose() && li < lane.lane_count && li < MAX_LANE_GUIDANCE; li++) {
            const rgd_lane_t* l = &lane.lanes[li];
            char abuf[128];
            int ao = 0;
            for (int ai = 0; ai < l->angle_count && ai < MAX_LANE_ANGLES; ai++)
                ao += snprintf(abuf + ao, sizeof(abuf) - (size_t)ao, "%s%d",
                               ai > 0 ? "," : "", (int)l->angles[ai]);
            if (l->angle_count == 0) abuf[0] = '\0';
            LOG_INFO(LOG_MODULE, "  lane[%d] pos=%u dir=%d status=%u angles(%u)=[%s]",
                     li, l->position, (int)l->direction, l->status,
                     l->angle_count, abuf);
        }
        write_bus_lane_guidance_partial(&lane);
    }

    return false;
}

/* Transport send callback - trigger 0x5200 injection on 0xFFFB */
static void rgd_transport_handler(hook_context_t* ctx, uint16_t msgid) {
    (void)ctx;

    /* LocationInfo remains the earliest trigger. rgd_request_updates() now
     * contains the retry/backoff gate, so later LocationInfo frames are also a
     * safe recovery opportunity after a phone/HU warm-start race. */
    if (msgid == IAP2_MSG_LOCATION_INFO && hook_is_ready()) {
        rgd_request_updates();
    }
}

/* Grow the Cinemo NmeArray identify buffer so the RGD component TLV fits.  The NmeArray struct at
 * ctx->_priv is { uint8_t* base@+0; uint32 len@+4; uint32 cap@+8 }.
 *
 * SAFETY: this reallocs a buffer Cinemo owns, which is sound BY CONSTRUCTION on this platform —
 * NmeIAP2Message::Encode (libNmeBaseClasses 0x11ffe8) grows this same base@+0 with libc realloc, so
 * the storage is the process libc heap and we call the same allocator from the same process.  We also
 * run inside the Encode interpose before returning to Cinemo, so Cinemo holds no stale base pointer.
 * The one residual risk is a firmware variant where the array is NOT libc-backed or the fields are
 * unexpected; guard against it below and SKIP the append (identify goes out unpatched → no RGI this
 * cycle, retried next identify) rather than corrupt the heap.  An identify is small, so any plausible
 * size stays well under RGD_IDENTIFY_MAX. */
#define RGD_IDENTIFY_MAX 8192u
static uint8_t* rgd_resize_identify_buffer(hook_context_t* ctx, size_t len, size_t new_len, size_t* out_cap) {
    if (!ctx || !ctx->_priv) return NULL;
    void* out_array = ctx->_priv;
    uint8_t* base = *(uint8_t**)((char*)out_array + 0);
    unsigned int arr_len = *(unsigned int*)((char*)out_array + 4);
    unsigned int cap = *(unsigned int*)((char*)out_array + 8);
    /* Invariant check before touching a Cinemo-owned buffer: a sane NmeArray has a non-NULL base,
     * len<=cap, and identify-plausible sizes.  Anything else → skip (fail-safe, never realloc). */
    if (!base || cap == 0 || cap > RGD_IDENTIFY_MAX || arr_len > cap || new_len > RGD_IDENTIFY_MAX) {
        LOG_WARN(LOG_MODULE,
                 "identify NmeArray failed invariant (base=%p len=%u cap=%u need=%zu) — skip append, no heap touch",
                 (void*)base, arr_len, cap, new_len);
        return NULL;
    }
    if (cap >= new_len) { if (out_cap) *out_cap = cap; return base; }
    size_t alloc = new_len + 64;
    uint8_t* new_buf = (uint8_t*)realloc(base, alloc);
    if (!new_buf) {
        new_buf = (uint8_t*)malloc(alloc);
        if (!new_buf) return NULL;
        if (base && len > 0) memcpy(new_buf, base, len);
        if (base) free(base);
    }
    *(uint8_t**)((char*)out_array + 0) = new_buf;
    *(unsigned int*)((char*)out_array + 8) = (unsigned int)alloc;
    if (out_cap) *out_cap = alloc;
    return new_buf;
}

/* Identify patcher */
static size_t rgd_identify_patcher(hook_context_t* ctx, uint8_t* buf, size_t len, size_t max_len) {
    rgd_lazy_init();  /* Ensure initialized */
    if (!buf || len < 6) return len;

    uint16_t existing_len = 0;
    size_t existing_off = iap2_find_tlv_offset(buf + 6, len - 6, IDENT_TLV_ROUTE_GUIDANCE_COMPONENT, &existing_len);
    bool existing_found = (existing_len > 0);

    if (existing_found) {
        existing_off += 6;
        uint16_t comp_id = rgd_extract_component_id(buf + existing_off, existing_len);
        if (comp_id != 0) {
            g_rgd.component_id = comp_id;
            LOG_INFO(LOG_MODULE, "Found existing 0x001E component=0x%04X", comp_id);
        }
    } else {
        LOG_INFO(LOG_MODULE, "Appending 0x001E component=0x%04X", g_rgd.component_id);
    }

    uint8_t new_tlv[256];
    size_t new_tlv_len = rgd_build_component_tlv(new_tlv, sizeof(new_tlv), g_rgd.component_id);
    if (new_tlv_len == 0) return len;

    size_t new_len = existing_found ? (len - existing_len + new_tlv_len) : (len + new_tlv_len);
    if (new_len > max_len) {
        /* Grow the NmeArray storage instead of skipping.  Skipping left RGD disabled: the phone
         * never learns the HU supports route guidance -> count=0, no maneuvers ("RGI не идёт").
         * The old prod impl reallocated this buffer without the feared heap corruption, so on this
         * platform the array's storage is malloc-compatible.  Grow, then patch into the new buffer. */
        size_t new_cap = 0;
        uint8_t* grown = rgd_resize_identify_buffer(ctx, len, new_len, &new_cap);
        if (!grown) {
            LOG_WARN(LOG_MODULE, "Identify patch skipped: resize failed (need %zu > cap %zu)",
                     new_len, max_len);
            return len;
        }
        buf = grown;
        max_len = new_cap;
        LOG_INFO(LOG_MODULE, "Identify buffer grown to %zu (need %zu)", new_cap, new_len);
    }

    if (existing_found) {
        size_t after_off = existing_off + existing_len;
        size_t after_len = len - after_off;
        if (after_len > 0) memmove(buf + existing_off + new_tlv_len, buf + after_off, after_len);
        memcpy(buf + existing_off, new_tlv, new_tlv_len);
    } else {
        memcpy(buf + len, new_tlv, new_tlv_len);
    }

    write_be16(buf + 2, (uint16_t)new_len);
    if (ctx && ctx->_priv) {
        *(unsigned int*)((char*)ctx->_priv + 4) = (unsigned int)new_len;
    }

    ctx->rgd_component_valid = true;
    ctx->identify_patched = true;

    LOG_INFO(LOG_MODULE, "Identify patched: %zu -> %zu bytes", len, new_len);
    return new_len;
}

/* State handler */
static void rgd_state_handler(hook_context_t* ctx, int event) {
    switch (event) {
        case HOOK_EVENT_IDENTIFY_START:
            /* dio_manager may survive a fast phone reconnect.  Never carry
             * got_520x/retry state into the new iAP2 identification cycle. */
            rgd_clear_state("new_identify");
            break;
        case HOOK_EVENT_SHUTDOWN:
            /* Process teardown is not a live protocol transition.  Never call
             * stock SendIAP2 from an ELF destructor: the link may already be
             * gone and the synchronous call has no timeout. */
            rgd_clear_state("shutdown");
            break;
        case HOOK_EVENT_IDENTIFY_END:
            /* IdentifyEnd means the phone accepted the component we just
             * advertised.  The component stays valid when 0x5200 had not yet
             * won the startup race; the heartbeat will request/retry as soon
             * as auth + injection context make hook_is_ready() true. */
            if (!g_rgd.active) {
                LOG_INFO(LOG_MODULE,
                         "Identify ended before RGI became active; retaining component=0x%04X ctx_valid=%d",
                         g_rgd.component_id, (ctx && ctx->rgd_component_valid) ? 1 : 0);
            }
            break;
    }
}

/* Public API */

/* Periodic tick (1 Hz from bus timer thread).
 *
 * Flushes non-reroute pending state=0 when iOS goes silent after a
 * single reset message.  Reroute reset frames deliberately do not time
 * out: Maps can remain in REROUTING for a long time with poor network,
 * and route teardown must then come from hard-clear/disconnect evidence.
 *
 * Cross-thread safety: this runs on the bus timer thread, while
 * the iAP2 handler thread may concurrently be inside rgd_message_handler
 * mutating g_rgd.route.update_cache via write_bus_update_partial().  We
 * **deliberately do not** go through write_bus_update_partial() here —
 * that path reads/writes g_rgd.route.update_cache without synchronisation.
 * Instead emit a self-contained minimal bus frame (route_state=0,
 * maneuver_count=0) directly, which is exactly what Java needs to see
 * for "route ended".  bus_send_text() is itself thread-safe.
 *
 * Idempotent: re-emitting state=0 to a Java side that already saw it
 * is a no-op (Java's RG handler short-circuits identical states). */
void rgd_periodic_tick(void) {
    /* Recover an already-active phone navigation session when the first 0x5200
     * raced Maps restoration and produced no initial snapshot.  This is a
     * no-op after any 0x5201/2/4 and is rate-limited inside the request helper. */
    rgd_request_updates();

    pthread_mutex_lock(&g_rgd_debounce_lock);
    if (g_rgd.state_zero_started_ms == 0 || g_rgd.emitted_route_state == 0) {
        pthread_mutex_unlock(&g_rgd_debounce_lock);
        return;
    }
    if (g_rgd.state_zero_from_reroute || g_rgd.state_zero_deadline_ms == 0) {
        pthread_mutex_unlock(&g_rgd_debounce_lock);
        return;
    }
    uint64_t now = now_monotonic_ms();
    if (now < g_rgd.state_zero_deadline_ms) {
        pthread_mutex_unlock(&g_rgd_debounce_lock);
        return;
    }
    uint64_t elapsed = now - g_rgd.state_zero_started_ms;

    /* Window expired — claim the emission.  Mark fields before
     * unlocking so a concurrent handler doesn't re-do the same work. */
    g_rgd.emitted_route_state = 0;
    g_rgd.state_zero_started_ms = 0;
    g_rgd.state_zero_deadline_ms = 0;
    g_rgd.state_zero_from_reroute = false;
    pthread_mutex_unlock(&g_rgd_debounce_lock);

    rgd_drop_kept_route("route ended");
    LOG_INFO(LOG_MODULE, "Route reset timeout: emitting deferred state=0 to Java (elapsed=%llums)",
             (unsigned long long)elapsed);

    /* Direct minimal bus frame — bypass update_cache (not synchronized
     * across threads).  bus_send_text is internally thread-safe. */
    bus_text_builder_t _b_storage;
    bus_text_builder_t* b = &_b_storage;
    uint8_t scratch[128];
    bus_text_begin_with(b, "routeguidance", scratch, sizeof(scratch));
    bus_text_int(b, "route_state", 0);
    bus_text_int(b, "maneuver_count", 0);
    bus_send_text(EVT_RGD_UPDATE, BUS_FLAG_STICKY, b);
}

/* Called lazily on the first message: marks the initialization boundary and
 * (re)publishes the cleared snapshot over the bus. */
static void rgd_init_once(void) {
    rgd_clear_state("init");
    /* Wire the bus timer to drive our 1 Hz pending-zero flush. */
    bus_set_periodic_tick(rgd_periodic_tick);
    LOG_INFO(LOG_MODULE, "Route Guidance module initialized");
}

/* Reached from the Encode thread (identify patcher) and the Decode thread. */
static void rgd_lazy_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, rgd_init_once);
}

void rgd_shutdown(void) {
    /* Defensive: deregister our 1 Hz tick before tearing down state.
     * Avoids a heartbeat-thread callback racing with a half-disposed
     * RGD module during process shutdown. */
    bus_set_periodic_tick(NULL);
    /* Do not emit 0x5203 while dio_manager/DSI is already exiting.  A blocking
     * stock SendIAP2 here prevented the child from completing destruction and
     * SI eventually killed it as TIMEOUT_WATCHDOG. */
    rgd_clear_state("module_shutdown");
}
