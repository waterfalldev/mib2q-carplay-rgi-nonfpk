/*
 * Route Guidance TLV Definitions and Parsing
 *
 * Defines TLV IDs for iAP2 Route Guidance messages:
 * - 0x5200 StartRouteGuidanceUpdates
 * - 0x5201 RouteGuidanceUpdate
 * - 0x5202 RouteGuidanceManeuverUpdate
 * - 0x5203 StopRouteGuidanceUpdates
 * - 0x5204 RouteGuidanceLaneGuidanceInformation
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#ifndef RGD_TLV_H
#define RGD_TLV_H

#include "../framework/common.h"

/* Raw RGD packet tracing (opt-in: build with -DRGD_TRACE_RAW_FULL=1). Value-based (#if) so a
 * -D...=0 build is consistent between the struct fields below and every writer. Presence-based
 * #ifdef would make -D...=0 enable the trace, and — worse — would give rgd_tlv.c a different
 * rgd_maneuver_t layout from rgd_hook.c the moment any translation unit sees a default define. */
#ifndef RGD_TRACE_RAW_FULL
#define RGD_TRACE_RAW_FULL 0
#endif

/* ============================================================
 * Identify TLV for RouteGuidanceDisplayComponent (0x001E)
 * ============================================================ */
#define IDENT_TLV_ROUTE_GUIDANCE_COMPONENT  0x001E

/* RouteGuidanceDisplayComponent inner TLV IDs
 *
 * Matches MHI3 libesoiap2.so component layout (9 TLVs, IDs 0-8).
 * Verified via CIOSTraceHelper::toString(RouteGuidanceDisplayComponent):
 *   compId, name, maxCurrentRoadNameLength, maxDestinationNameLength,
 *   maxAfterManeuverRoadNameLength, maxManeuverDescriptionLength,
 *   maxGuidanceManeuverStorageCapacity, maxLaneGuidanceDescriptionLength,
 *   maxLaneGuidanceStorageCapacity
 */
#define RGD_COMP_COMPONENT_ID               0x0000
#define RGD_COMP_NAME                       0x0001
#define RGD_COMP_MAX_CURRENT_ROAD_NAME      0x0002
#define RGD_COMP_MAX_DEST_NAME              0x0003
#define RGD_COMP_MAX_AFTER_MANEUVER_NAME    0x0004
#define RGD_COMP_MAX_MANEUVER_DESC          0x0005
#define RGD_COMP_MAX_MANEUVER_CAPACITY      0x0006
#define RGD_COMP_MAX_LANE_GUIDANCE_DESC     0x0007
#define RGD_COMP_MAX_LANE_GUIDANCE_CAPACITY 0x0008

/* ============================================================
 * StartRouteGuidanceUpdates (0x5200) TLV IDs
 * ============================================================ */
#define RGD_START_TLV_COMPONENT_ID       0x0000
#define RGD_START_TLV_SOURCE_NAME        0x0001
#define RGD_START_TLV_SOURCE_SUPPORTS_RG 0x0002
#define RGD_START_TLV_SUPPORTS_EXIT_INFO 0x0003

/* ============================================================
 * RouteGuidanceUpdate (0x5201) TLV IDs
 * ============================================================ */
#define RGD_TLV_COMPONENT_ID            0x0000
#define RGD_TLV_ROUTE_GUIDANCE_STATE    0x0001
#define RGD_TLV_MANEUVER_STATE          0x0002
#define RGD_TLV_CURRENT_ROAD_NAME       0x0003
#define RGD_TLV_DESTINATION_NAME        0x0004
#define RGD_TLV_ETA                     0x0005
#define RGD_TLV_TIME_REMAINING          0x0006
#define RGD_TLV_DISTANCE_REMAINING      0x0007
#define RGD_TLV_DISTANCE_STRING         0x0008
#define RGD_TLV_DISTANCE_UNITS          0x0009
#define RGD_TLV_DIST_TO_MANEUVER        0x000A
#define RGD_TLV_DIST_TO_MANEUVER_STRING 0x000B
#define RGD_TLV_DIST_TO_MANEUVER_UNITS  0x000C
#define RGD_TLV_MANEUVER_LIST           0x000D
#define RGD_TLV_MANEUVER_COUNT          0x000E
#define RGD_TLV_VISIBLE_IN_APP          0x000F
#define RGD_TLV_LANE_GUIDANCE_INDEX     0x0010
#define RGD_TLV_LANE_GUIDANCE_TOTAL     0x0011
#define RGD_TLV_LANE_GUIDANCE_SHOWING   0x0012
#define RGD_TLV_SOURCE_NAME             0x0013
#define RGD_TLV_SOURCE_SUPPORTS_RG      0x0014

/*
 * RouteGuidanceState values (as observed in MHI3 dio_manager type-info strings).
 *
 * mediabase::CDIONavigationMetadataTypeInfo::toString(ERouteGuidanceState):
 *   0=NO_ROUTE_SET, 1=ROUTE_SET, 2=ARRIVED, 3=LOADING, 4=LOCATING,
 *   5=REROUTING, 6=PROCEED_TO_ROUTE
 */
#define RGD_STATE_NO_ROUTE_SET      0
#define RGD_STATE_REROUTING         5

/* ============================================================
 * RouteGuidanceManeuverUpdate (0x5202) TLV IDs
 * ============================================================ */
#define MAN_TLV_COMPONENT_ID            0x0000
#define MAN_TLV_INDEX                   0x0001
#define MAN_TLV_DESCRIPTION             0x0002
#define MAN_TLV_TYPE                    0x0003
#define MAN_TLV_AFTER_ROAD_NAME         0x0004
#define MAN_TLV_DISTANCE_BETWEEN        0x0005
#define MAN_TLV_DISTANCE_STRING         0x0006
#define MAN_TLV_DISTANCE_UNITS          0x0007
#define MAN_TLV_DRIVING_SIDE            0x0008
#define MAN_TLV_JUNCTION_TYPE           0x0009
#define MAN_TLV_JUNCTION_ANGLES         0x000A /* JunctionElementAngle (iOS) */
#define MAN_TLV_EXIT_ANGLE              0x000B /* JunctionElementExitAngle (iOS) */
#define MAN_TLV_LINKED_LANE_GUIDANCE    0x000C
#define MAN_TLV_EXIT_INFO               0x000D

/* ============================================================
 * RouteGuidanceLaneGuidanceInformation (0x5204) TLV IDs
 * ============================================================ */
#define LANE_MSG_TLV_COMPONENT_ID            0x0000
#define LANE_MSG_TLV_LANE_GUIDANCE_INDEX     0x0001
#define LANE_MSG_TLV_LANE_INFORMATIONS       0x0002
#define LANE_MSG_TLV_LANE_GUIDANCE_DESC      0x0003

/* ============================================================
 * Data Structures
 * ============================================================ */

#define MAX_MANEUVER_LIST   16
#define MAX_COMPONENT_LIST  8
#define MAX_LANE_GUIDANCE   8
#define MAX_LANE_ANGLES     16
/* Must align with Java RouteGuidance.MAX_MANEUVERS.
 * Sized for routes with many active events.  iOS publishes the entire
 * route (sendLaneGuidances batch-sends every cached event;
 * sendManeuvers walks the full source) — long highway trips routinely
 * cross 20+ events.  iOS's own CRAccNavInfoCache defaults to 1000
 * entries; we keep a much smaller LRU here, with active-list eviction
 * protection (rgd_is_active_index / rgd_is_active_lane_index). */
#define MANEUVER_CACHE_SIZE 32
#define RGD_HEX_PREVIEW_MAX 96
/*
 * Exit info (MAN_TLV_EXIT_INFO) in MHI3 dio_manager is treated as a string and fed into util::UnicodeString8
 * without an explicit length (CRouteGuidanceUpdateProcessorImpl::getRouteGuidanceManeuverInformation, a1+240).
 * We keep the raw+hex preview for debugging, but also publish a decoded string key to the bus.
 */
#define RGD_EXIT_INFO_MAX   256

/*
 * MAN_TLV_LINKED_LANE_GUIDANCE (0x000C) is treated as native scalar linked-lane index
 * (u16, with occasional component+index compact forms).
 * Detailed per-lane content is parsed from 0x5204 LaneInformations.
 */

/*
 * Lane-information sub-TLV IDs inside 0x5204 / TLV 0x0002.
 *
 * Native parser (libesoiap2 processLaneGuidanceInformation + parseLaneInformation):
 * 0x0000=index, 0x0001=laneStatus, 0x0002=laneAngles (repeated), 0x0003=laneAngleHighlight.
 */
#define LANE_INFO_TLV_INDEX           0x0000  /* u16 */
#define LANE_INFO_TLV_STATUS          0x0001  /* u8 */
#define LANE_INFO_TLV_ANGLES          0x0002  /* repeated u16 */
#define LANE_INFO_TLV_ANGLE_HIGHLIGHT 0x0003  /* u16, sentinel 1000 is valid */

/* Lane guidance per-lane info */
typedef struct {
    uint16_t position;       /* lane position from TLV 0x0000 (fallback: parse index) */
    int16_t  direction;      /* raw u16/s16 lane direction/angle; preserves sentinel 1000 */
    uint8_t  status;         /* 0=NOT_GOOD, 1=GOOD, 2=PREFERRED */
    int16_t  angles[MAX_LANE_ANGLES]; /* full laneAngles vector from 0x5204/0x0002 */
    uint8_t  angle_count;
} rgd_lane_t;

/* Lane guidance data (from 0x5204) */
typedef struct {
    uint64_t present;
    uint16_t lane_guidance_index;
    rgd_lane_t lanes[MAX_LANE_GUIDANCE];
    uint8_t lane_count;
    uint8_t lane_complete; /* full, explicit ordered snapshot; no capacity truncation */
} rgd_lane_guidance_t;

/* Route update data (from 0x5201).  Only the fields the bus snapshot publishes are kept. */
typedef struct {
    uint64_t present;
    uint16_t component_ids[MAX_COMPONENT_LIST];
    uint16_t component_count;
    uint8_t route_state;
    uint8_t maneuver_state;
    uint8_t visible_in_app;
    uint8_t lane_guidance_showing;
    uint8_t source_supports_route_guidance;
    uint16_t maneuver_count;
    uint16_t lane_guidance_index;
    int16_t lane_guidance_slot;  /* Bus-only remapped slot for lane_guidance_index */
    uint32_t distance_remaining;
    uint32_t dist_to_maneuver;
    uint64_t eta;
    uint64_t time_remaining;
    char current_road[256];
    char destination[256];
    uint16_t maneuver_list[MAX_MANEUVER_LIST];
    uint16_t maneuver_list_count;
} rgd_update_t;

/* rgd_update_t present bits */
#define RGD_UPD_COMPONENT_IDS        (1ULL << 0)
#define RGD_UPD_ROUTE_STATE          (1ULL << 1)
#define RGD_UPD_MANEUVER_STATE       (1ULL << 2)
#define RGD_UPD_CURRENT_ROAD         (1ULL << 3)
#define RGD_UPD_DESTINATION          (1ULL << 4)
#define RGD_UPD_ETA                  (1ULL << 5)
#define RGD_UPD_TIME_REMAINING       (1ULL << 6)
#define RGD_UPD_DISTANCE_REMAINING   (1ULL << 7)
#define RGD_UPD_DIST_TO_MANEUVER     (1ULL << 8)
#define RGD_UPD_MANEUVER_COUNT       (1ULL << 9)
#define RGD_UPD_MANEUVER_LIST        (1ULL << 10)
#define RGD_UPD_VISIBLE_IN_APP       (1ULL << 11)
#define RGD_UPD_LANE_INDEX           (1ULL << 12)
#define RGD_UPD_LANE_SHOWING         (1ULL << 13)
#define RGD_UPD_SOURCE_SUPPORTS_RG   (1ULL << 14)
#define RGD_UPD_LANE_SLOT            (1ULL << 15)  /* Bus-only remapped lane slot */

/* rgd_lane_guidance_t present bits */
#define RGD_LANE_GUIDANCE_INDEX      (1ULL << 0)
#define RGD_LANE_INFORMATIONS        (1ULL << 1)

/* Maneuver data (from 0x5202) */
typedef struct {
    uint64_t present;
    uint16_t index;
    uint8_t maneuver_type;
    uint8_t driving_side;
    uint8_t junction_type;
    int16_t exit_angle;
    int16_t junction_angles[MAX_MANEUVER_LIST];
    uint16_t junction_angle_count;
    uint32_t distance_between;
    char description[256];
    char after_road_name[256];
    rgd_lane_t lanes[MAX_LANE_GUIDANCE];
    uint8_t lane_count;
#if RGD_TRACE_RAW_FULL
    char lane_guidance_hex[(RGD_HEX_PREVIEW_MAX * 3) + 1];
#endif
    uint16_t linked_lane_guidance_index;
#if RGD_TRACE_RAW_FULL
    char exit_info_hex[(RGD_HEX_PREVIEW_MAX * 3) + 1];
#endif
    char exit_info_str[RGD_EXIT_INFO_MAX + 1];
} rgd_maneuver_t;

/* rgd_maneuver_t present bits */
#define RGD_MAN_INDEX                (1ULL << 0)
#define RGD_MAN_DESCRIPTION          (1ULL << 1)
#define RGD_MAN_TYPE                 (1ULL << 2)
#define RGD_MAN_AFTER_ROAD           (1ULL << 3)
#define RGD_MAN_DISTANCE_BETWEEN     (1ULL << 4)
#define RGD_MAN_DRIVING_SIDE         (1ULL << 5)
#define RGD_MAN_JUNCTION_TYPE        (1ULL << 6)
#define RGD_MAN_JUNCTION_ANGLES      (1ULL << 7)
#define RGD_MAN_EXIT_ANGLE           (1ULL << 8)
#define RGD_MAN_LANE_GUIDANCE_RAW    (1ULL << 9)
#define RGD_MAN_EXIT_INFO_RAW        (1ULL << 10)
#define RGD_MAN_EXIT_INFO_STR        (1ULL << 11)
#define RGD_MAN_LANE_GUIDANCE        (1ULL << 12)  /* Parsed lane data in lanes[] */
#define RGD_MAN_LINKED_LANE_INDEX    (1ULL << 13)  /* Scalar linked lane guidance index */

/* ============================================================
 * Parsing Functions
 * ============================================================ */

/* Parse complete iAP2 messages. False rejects the entire malformed delta;
 * output remains empty (maneuver linked-lane sentinel remains 0xFFFF). */
/* Parse RouteGuidanceUpdate (0x5201) payload */
bool rgd_parse_update(const uint8_t* buf, size_t len, rgd_update_t* out);

/* Parse RouteGuidanceManeuverUpdate (0x5202) payload */
bool rgd_parse_maneuver(const uint8_t* buf, size_t len, rgd_maneuver_t* out);

/* Parse RouteGuidanceLaneGuidanceInformation (0x5204) payload */
bool rgd_parse_lane_guidance(const uint8_t* buf, size_t len, rgd_lane_guidance_t* out);

/* ============================================================
 * Building Functions (for Identify patching)
 * ============================================================ */

/* Build RouteGuidanceDisplayComponent TLV for Identify
 * Returns total TLV length, or 0 on error */
size_t rgd_build_component_tlv(uint8_t* out, size_t max_len, uint16_t component_id);

/* Extract component ID from existing RouteGuidanceDisplayComponent
 * Returns component ID, or 0 if not found */
uint16_t rgd_extract_component_id(const uint8_t* tlv_start, size_t tlv_len);

#endif /* RGD_TLV_H */
