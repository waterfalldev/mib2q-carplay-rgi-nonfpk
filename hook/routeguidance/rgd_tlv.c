/*
 * Route Guidance TLV Parsing and Building Implementation
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#include "rgd_tlv.h"
#include "../framework/logging.h"

DEFINE_LOG_MODULE(RGD);

#if RGD_TRACE_RAW_FULL
static void hex_encode_preview(const uint8_t* data, int len, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!data || len <= 0) return;

    size_t max_bytes = (size_t)len;
    if (max_bytes > RGD_HEX_PREVIEW_MAX) max_bytes = RGD_HEX_PREVIEW_MAX;

    size_t off = 0;
    for (size_t i = 0; i < max_bytes && off + 4 < out_sz; i++) {
        off += (size_t)snprintf(out + off, out_sz - off,
                                "%02X%s", data[i], (i + 1 < max_bytes) ? " " : "");
    }
    out[off] = '\0';
}
#endif

/* Replace newlines (\r\n or \n) with " / " separator in-place.
 * iOS packs multi-part location info (street + bus stop, etc.) into a single
 * string separated by newlines.  " / " keeps it readable on the HUD.  The result is cut
 * at buf_sz - 1 bytes (at most 256: the string fields' size), so a string that fills the
 * buffer without a newline is kept whole. */
static void replace_newlines(char* s, size_t buf_sz) {
    char tmp[256];
    size_t si = 0;
    for (size_t i = 0; s[i] != '\0' && si < buf_sz - 1; i++) {
        if (s[i] != '\r' && s[i] != '\n') {
            tmp[si++] = s[i];
            continue;
        }
        if (s[i] == '\r' && s[i + 1] == '\n') i++;
        for (const char* p = " / "; *p && si < buf_sz - 1; p++) tmp[si++] = *p;
    }
    memcpy(s, tmp, si);
    s[si] = '\0';
}

/* A string TLV value into dst: cut to dst_sz - 1 bytes, terminated, newlines replaced when asked. */
static void tlv_string(char* dst, size_t dst_sz, const uint8_t* val, int val_len, bool newlines) {
    size_t n = (size_t)val_len < dst_sz - 1 ? (size_t)val_len : dst_sz - 1;
    memcpy(dst, val, n);
    dst[n] = '\0';
    if (newlines) replace_newlines(dst, dst_sz);
}

/* One TLV into out->field (and its present bit), when the value is wide enough. */
#define TLV_U8(field, bit)  do { if (val_len >= 1) { out->field = val[0]; out->present |= (bit); } } while (0)
#define TLV_U16(field, bit) do { if (val_len >= 2) { out->field = read_be16(val); out->present |= (bit); } } while (0)
#define TLV_U32(field, bit) do { if (val_len >= 4) { out->field = read_be32(val); out->present |= (bit); } } while (0)
#define TLV_U64(field, bit) do { if (val_len >= 8) { out->field = read_be64(val); out->present |= (bit); } } while (0)
#define TLV_STR(field, bit) do { tlv_string(out->field, sizeof(out->field), val, val_len, true); out->present |= (bit); } while (0)

static void copy_printable_string_preview(const uint8_t* data, int len, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!data || len <= 0) return;

    /*
     * Copy until NUL or max.
     *
     * Exit info on MHI3 is fed into util::UnicodeString8, so it can contain UTF-8.
     * Preserve bytes >= 0x80 (UTF-8 sequences) and replace only ASCII control bytes.
     */
    size_t max_bytes = (size_t)len;
    if (max_bytes > out_sz - 1) max_bytes = out_sz - 1;

    size_t n = 0;
    for (; n < max_bytes; n++) {
        uint8_t c = data[n];
        if (c == 0) break;
        if (c >= 0x80) {
            out[n] = (char)c;
        } else if (c >= 0x20 && c <= 0x7E) {
            out[n] = (char)c;
        } else {
            out[n] = '?';
        }
    }
    out[n] = '\0';
}

/*
 * Decode scalar MAN_TLV_LINKED_LANE_GUIDANCE (0x000C) payload.
 *
 * Native parser (sub_72C70 case 0x000C) consumes the first big-endian u16 only.
 */
static bool decode_linked_lane_index(const uint8_t* val, int val_len, uint16_t* out_idx) {
    if (!val || !out_idx || val_len < 2) return false;
    *out_idx = read_be16(val);
    return true;
}

static void rgd_lane_reset(rgd_lane_t* lane, uint16_t fallback_position) {
    if (!lane) return;
    memset(lane, 0, sizeof(*lane));
    lane->position = fallback_position;
    lane->direction = 1000;
}

/*
 * Parse one LaneInformations TLV (0x5204 / id=0x0002) payload.
 *
 * Native parser path:
 * processLaneGuidanceInformation -> parseLaneInformation
 * fields:
 *   0x0000 index
 *   0x0001 laneStatus
 *   0x0002 laneAngles (repeated)
 *   0x0003 laneAngleHighlight
 *
 * Returns number of parsed lanes (can be >1 if payload packs multiple index groups).
 */
static uint8_t rgd_parse_lane_information_value(const uint8_t* val, size_t val_len,
                                                rgd_lane_t* out_lanes, uint8_t out_max, uint8_t *complete) {
    if (!val || !out_lanes || out_max == 0 || val_len < 4) {*complete=0;return 0;}

    size_t off = 0;
    int lane_idx = -1;
    uint8_t lane_count = 0;
    bool lane_has_highlight = false;
    bool saw_any = false;
    uint8_t fields[MAX_LANE_GUIDANCE] = {0};

    while (off + 4 <= val_len) {
        uint16_t sub_len = read_be16(val + off);
        uint16_t sub_id = read_be16(val + off + 2);
        int sub_val_len = (int)sub_len - 4;
        const uint8_t* sub_val = val + off + 4;

        if (sub_len < 4 || off + sub_len > val_len) {
            *complete=0;
            LOG_WARN(LOG_MODULE, "Malformed 0x5204 lane-information TLV: len=%u off=%zu total=%zu",
                     sub_len, off, val_len);
            break;
        }

        if (sub_id == LANE_INFO_TLV_INDEX) {
            if (lane_count >= out_max) {
                *complete=0;
                LOG_WARN(LOG_MODULE, "0x5204 lane information exceeds MAX_LANE_GUIDANCE=%u",
                         (unsigned)MAX_LANE_GUIDANCE);
                break;
            }

            lane_idx = lane_count;
            rgd_lane_reset(&out_lanes[lane_idx], (uint16_t)lane_idx);
            if(sub_val_len>=1)fields[lane_idx]|=1;
            if (sub_val_len >= 2) {
                out_lanes[lane_idx].position = read_be16(sub_val);
            } else if (sub_val_len >= 1) {
                out_lanes[lane_idx].position = (uint16_t)sub_val[0];
            }
            lane_count++;
            lane_has_highlight = false;
            saw_any = true;
            off += sub_len;
            continue;
        }

        if (sub_id == LANE_INFO_TLV_STATUS
            || sub_id == LANE_INFO_TLV_ANGLES
            || sub_id == LANE_INFO_TLV_ANGLE_HIGHLIGHT) {
            if (lane_idx < 0) {
                if (lane_count >= out_max) {
                    *complete=0;
                    LOG_WARN(LOG_MODULE, "0x5204 lane information exceeds MAX_LANE_GUIDANCE=%u",
                             (unsigned)MAX_LANE_GUIDANCE);
                    break;
                }
                lane_idx = lane_count;
                rgd_lane_reset(&out_lanes[lane_idx], (uint16_t)lane_idx);
                lane_count++;
                lane_has_highlight = false;
            }
            saw_any = true;
        }

        switch (sub_id) {
            case LANE_INFO_TLV_STATUS:
                if (lane_idx >= 0 && sub_val_len >= 1) {
                    out_lanes[lane_idx].status = sub_val[0];
                    fields[lane_idx]|=2;
                }
                break;

            case LANE_INFO_TLV_ANGLES:
                if (lane_idx >= 0 && sub_val_len >= 1) {
                    int p = 0;
                    while (p < sub_val_len && out_lanes[lane_idx].angle_count < MAX_LANE_ANGLES) {
                        int16_t angle;
                        if (p + 1 < sub_val_len) {
                            angle = (int16_t)read_be16(sub_val + p);
                            p += 2;
                        } else {
                            angle = (int16_t)sub_val[p];
                            p++;
                        }

                        out_lanes[lane_idx].angles[out_lanes[lane_idx].angle_count++] = angle;

                        /*
                         * laneAngleHighlight (0x0003) is authoritative. If it is absent,
                         * use the first laneAngles value as directional fallback.
                         */
                        if (!lane_has_highlight && out_lanes[lane_idx].direction == 1000) {
                            out_lanes[lane_idx].direction = angle;
                        }
                    }
                    if(p<sub_val_len)*complete=0;
                }
                break;

            case LANE_INFO_TLV_ANGLE_HIGHLIGHT:
                if (lane_idx >= 0 && sub_val_len >= 1) {
                    out_lanes[lane_idx].direction = (sub_val_len >= 2)
                        ? (int16_t)read_be16(sub_val)
                        : (int16_t)sub_val[0];
                    lane_has_highlight = true;
                }
                break;

            default:
                LOG_WARN(LOG_MODULE, "Unknown 0x5204 lane-information sub-TLV: id=0x%04X len=%u",
                         sub_id, sub_len);
                break;
        }

        off += sub_len;
    }

    if(off!=val_len)*complete=0;
    for(uint8_t i=0;i<lane_count;++i)if(fields[i]!=3)*complete=0;
    return saw_any ? lane_count : 0;
}

/* Check the entire delta before any prefix can reach the caches. Unknown TLVs
 * remain forward-compatible. LaneInformations is the only nested container;
 * validation has a fixed maximum depth of two. */
static bool rgd_tlv_sequence_valid(const uint8_t* buf, size_t len, bool lane_message) {
    size_t off = 0;
    while (off < len) {
        if (len - off < 4) return false;
        uint16_t n = read_be16(buf + off);
        if (n < 4 || n > len - off) return false;
        if (lane_message && read_be16(buf + off + 2) == LANE_MSG_TLV_LANE_INFORMATIONS
            && !rgd_tlv_sequence_valid(buf + off + 4, n - 4, false)) return false;
        off += n;
    }
    return true;
}

static bool rgd_message_valid(const uint8_t* buf, size_t len, uint16_t msgid) {
    return buf && len >= 6 && buf[0] == 0x40 && buf[1] == 0x40
        && read_be16(buf + 2) == len && read_be16(buf + 4) == msgid
        && rgd_tlv_sequence_valid(buf + 6, len - 6, msgid == 0x5204);
}

bool rgd_parse_update(const uint8_t* buf, size_t len, rgd_update_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!rgd_message_valid(buf, len, 0x5201)) return false;

    size_t off = 6; /* Skip iAP2 header */
    while (off + 4 <= len) {
        uint16_t tlv_len = read_be16(buf + off);
        uint16_t tlv_id = read_be16(buf + off + 2);
        int val_len = (int)tlv_len - 4;
        const uint8_t* val = buf + off + 4;

        if (tlv_len < 4 || off + tlv_len > len) break;

        switch (tlv_id) {
            case RGD_TLV_COMPONENT_ID:
                out->component_count = 0;
                for (int i = 0; i + 1 < val_len && out->component_count < MAX_COMPONENT_LIST; i += 2) {
                    out->component_ids[out->component_count++] = read_be16(val + i);
                }
                out->present |= RGD_UPD_COMPONENT_IDS;
                break;

            case RGD_TLV_ROUTE_GUIDANCE_STATE:  TLV_U8(route_state, RGD_UPD_ROUTE_STATE); break;
            case RGD_TLV_MANEUVER_STATE:        TLV_U8(maneuver_state, RGD_UPD_MANEUVER_STATE); break;
            case RGD_TLV_CURRENT_ROAD_NAME:     TLV_STR(current_road, RGD_UPD_CURRENT_ROAD); break;
            case RGD_TLV_DESTINATION_NAME:      TLV_STR(destination, RGD_UPD_DESTINATION); break;
            case RGD_TLV_ETA:                   TLV_U64(eta, RGD_UPD_ETA); break;
            case RGD_TLV_TIME_REMAINING:        TLV_U64(time_remaining, RGD_UPD_TIME_REMAINING); break;
            case RGD_TLV_DISTANCE_REMAINING:    TLV_U32(distance_remaining, RGD_UPD_DISTANCE_REMAINING); break;
            case RGD_TLV_DIST_TO_MANEUVER:      TLV_U32(dist_to_maneuver, RGD_UPD_DIST_TO_MANEUVER); break;
            case RGD_TLV_MANEUVER_COUNT:        TLV_U16(maneuver_count, RGD_UPD_MANEUVER_COUNT); break;

            case RGD_TLV_MANEUVER_LIST:
                out->maneuver_list_count = 0;
                for (int i = 0; i + 1 < val_len && out->maneuver_list_count < MAX_MANEUVER_LIST; i += 2) {
                    out->maneuver_list[out->maneuver_list_count++] = read_be16(val + i);
                }
                out->present |= RGD_UPD_MANEUVER_LIST;
                break;

            case RGD_TLV_VISIBLE_IN_APP:        TLV_U8(visible_in_app, RGD_UPD_VISIBLE_IN_APP); break;
            case RGD_TLV_LANE_GUIDANCE_INDEX:   TLV_U16(lane_guidance_index, RGD_UPD_LANE_INDEX); break;
            case RGD_TLV_LANE_GUIDANCE_SHOWING: TLV_U8(lane_guidance_showing, RGD_UPD_LANE_SHOWING); break;
            case RGD_TLV_SOURCE_SUPPORTS_RG:    TLV_U8(source_supports_route_guidance, RGD_UPD_SOURCE_SUPPORTS_RG); break;

            case RGD_TLV_DISTANCE_STRING:
            case RGD_TLV_DISTANCE_UNITS:
            case RGD_TLV_DIST_TO_MANEUVER_STRING:
            case RGD_TLV_DIST_TO_MANEUVER_UNITS:
            case RGD_TLV_LANE_GUIDANCE_TOTAL:
            case RGD_TLV_SOURCE_NAME:
            case 0x0015: /* destinationTimeZoneOffsetMinutes (iOS 17+) */
            case 0x0016: /* stopType (iOS 17+) */
            case 0x0017: /* electricVehicleDestination (iOS 17+) */
            case 0x0018: /* reserved/future iOS extension */
            case 0x0019: /* reserved/future iOS extension */
            case 0x001A: /* reserved/future iOS extension */
                /*
                 * Known but unconsumed TLVs.  The strings, units, lane total and source
                 * name are not published to the bus.  iOS 17+ extends 0x5201 with
                 * EV / multi-stop / timezone metadata that we do not need
                 * for cluster RG.  Silently skip so logs stay clean.
                 * Mapping verified against
                 * CarPlay.framework/CarPlay/CPRouteGuidance.mm:
                 * paramKey:0..26 with selectors
                 * destinationTimeZoneOffsetMinutes(21), stopType(22),
                 * electricVehicleDestination(23).
                 */
                break;
            default:
                LOG_WARN(LOG_MODULE, "Unknown RGD TLV (0x5201): id=0x%04X len=%u", tlv_id, tlv_len);
                break;
        }
        off += tlv_len;
    }
    return true;
}

bool rgd_parse_maneuver(const uint8_t* buf, size_t len, rgd_maneuver_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->linked_lane_guidance_index = 0xFFFF;
    if (!rgd_message_valid(buf, len, 0x5202)) return false;

    size_t off = 6; /* Skip iAP2 header */
    while (off + 4 <= len) {
        uint16_t tlv_len = read_be16(buf + off);
        uint16_t tlv_id = read_be16(buf + off + 2);
        int val_len = (int)tlv_len - 4;
        const uint8_t* val = buf + off + 4;

        if (tlv_len < 4 || off + tlv_len > len) break;

        switch (tlv_id) {
            case MAN_TLV_COMPONENT_ID:
            case MAN_TLV_DISTANCE_STRING:
            case MAN_TLV_DISTANCE_UNITS:
                /* Not published to the bus. */
                break;

            case MAN_TLV_INDEX:            TLV_U16(index, RGD_MAN_INDEX); break;
            case MAN_TLV_DESCRIPTION:      TLV_STR(description, RGD_MAN_DESCRIPTION); break;
            case MAN_TLV_TYPE:             TLV_U8(maneuver_type, RGD_MAN_TYPE); break;
            case MAN_TLV_AFTER_ROAD_NAME:  TLV_STR(after_road_name, RGD_MAN_AFTER_ROAD); break;
            case MAN_TLV_DISTANCE_BETWEEN: TLV_U32(distance_between, RGD_MAN_DISTANCE_BETWEEN); break;
            case MAN_TLV_DRIVING_SIDE:     TLV_U8(driving_side, RGD_MAN_DRIVING_SIDE); break;
            case MAN_TLV_JUNCTION_TYPE:    TLV_U8(junction_type, RGD_MAN_JUNCTION_TYPE); break;

            case MAN_TLV_JUNCTION_ANGLES:
                /* iAP2 sends one angle per repeated TLV - don't reset count
                 * (memset at function entry already initializes to 0) */
                for (int i = 0; i + 1 < val_len && out->junction_angle_count < MAX_MANEUVER_LIST; i += 2) {
                    out->junction_angles[out->junction_angle_count++] = (int16_t)read_be16(val + i);
                }
                out->present |= RGD_MAN_JUNCTION_ANGLES;
                break;

            case MAN_TLV_EXIT_ANGLE:       TLV_U16(exit_angle, RGD_MAN_EXIT_ANGLE); break;

            case MAN_TLV_LINKED_LANE_GUIDANCE:
#if RGD_TRACE_RAW_FULL
                hex_encode_preview(val, val_len, out->lane_guidance_hex, sizeof(out->lane_guidance_hex));
#endif
                out->present |= RGD_MAN_LANE_GUIDANCE_RAW;
                {
                    uint16_t linked_idx = 0xFFFF;
                    if (decode_linked_lane_index(val, val_len, &linked_idx)) {
                        out->linked_lane_guidance_index = linked_idx;
                        out->present |= RGD_MAN_LINKED_LANE_INDEX;
                    }
                }
                break;

            case MAN_TLV_EXIT_INFO:
#if RGD_TRACE_RAW_FULL
                hex_encode_preview(val, val_len, out->exit_info_hex, sizeof(out->exit_info_hex));
#endif
                copy_printable_string_preview(val, val_len, out->exit_info_str, sizeof(out->exit_info_str));
                out->present |= RGD_MAN_EXIT_INFO_RAW;
                out->present |= RGD_MAN_EXIT_INFO_STR;
                break;
            default:
                LOG_WARN(LOG_MODULE, "Unknown MAN TLV (0x5202): id=0x%04X len=%u", tlv_id, tlv_len);
                break;
        }
        off += tlv_len;
    }

    /*
     * MHI3 dio_manager (CRouteGuidanceUpdateProcessorImpl::getRouteGuidanceManeuverInformation)
     * explicitly sets a default exit angle when the field is not present.
     * Mirror that so Java-side mapping doesn't see "-1"/unset for valid maneuvers.
     */
    if ( (out->present & RGD_MAN_INDEX) && !(out->present & RGD_MAN_EXIT_ANGLE) ) {
        out->exit_angle = 1000;
        out->present |= RGD_MAN_EXIT_ANGLE;
    }
    return true;
}

bool rgd_parse_lane_guidance(const uint8_t* buf, size_t len, rgd_lane_guidance_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!rgd_message_valid(buf, len, 0x5204)) return false;
    out->lane_complete=1;

    size_t off = 6; /* Skip iAP2 header */
    while (off + 4 <= len) {
        uint16_t tlv_len = read_be16(buf + off);
        uint16_t tlv_id = read_be16(buf + off + 2);
        int val_len = (int)tlv_len - 4;
        const uint8_t* val = buf + off + 4;

        if (tlv_len < 4 || off + tlv_len > len) break;

        switch (tlv_id) {
            case LANE_MSG_TLV_COMPONENT_ID:
            case LANE_MSG_TLV_LANE_GUIDANCE_DESC:
                /* Not published to the bus. */
                break;

            case LANE_MSG_TLV_LANE_GUIDANCE_INDEX:
                if (val_len >= 2) {
                    out->lane_guidance_index = read_be16(val);
                    out->present |= RGD_LANE_GUIDANCE_INDEX;
                } else if (val_len >= 1) {
                    out->lane_guidance_index = val[0];
                    out->present |= RGD_LANE_GUIDANCE_INDEX;
                }
                break;

            case LANE_MSG_TLV_LANE_INFORMATIONS:
                out->present |= RGD_LANE_INFORMATIONS;
                if (val_len > 0 && out->lane_count < MAX_LANE_GUIDANCE) {
                    rgd_lane_t parsed[MAX_LANE_GUIDANCE];
                    uint8_t free_slots = (uint8_t)(MAX_LANE_GUIDANCE - out->lane_count);
                    uint8_t parsed_count = rgd_parse_lane_information_value(
                        val, (size_t)val_len, parsed, free_slots, &out->lane_complete);
                    for (uint8_t i = 0; i < parsed_count && out->lane_count < MAX_LANE_GUIDANCE; i++) {
                        out->lanes[out->lane_count++] = parsed[i];
                    }
                } else if(val_len>0)out->lane_complete=0;
                break;

            default:
                LOG_WARN(LOG_MODULE, "Unknown RGD TLV (0x5204): id=0x%04X len=%u", tlv_id, tlv_len);
                break;
        }

        off += tlv_len;
    }
    return true;
}

static size_t put_u16_tlv(uint8_t* out, size_t off, uint16_t id, uint16_t value) {
    write_be16(out + off, 6);
    write_be16(out + off + 2, id);
    write_be16(out + off + 4, value);
    return off + 6;
}

size_t rgd_build_component_tlv(uint8_t* out, size_t max_len, uint16_t component_id) {
    /*
     * Match MHI3 libesoiap2.so RouteGuidanceDisplayComponent layout (9 TLVs).
     *
     * ID 0: Identifier (uint16)
     * ID 1: Name (utf8)
     * ID 2: MaxCurrentRoadNameLength (uint16)
     * ID 3: MaxDestinationNameLength (uint16)
     * ID 4: MaxAfterManeuverRoadNameLength (uint16)
     * ID 5: MaxManeuverDescriptionLength (uint16)
     * ID 6: MaxGuidanceManeuverStorageCapacity (uint16)
     * ID 7: MaxLaneGuidanceDescriptionLength (uint16)
     * ID 8: MaxLaneGuidanceStorageCapacity (uint16)
     *
     * NOTE: no GuidanceDisplayCapability at ID 7 and no boolean flags at
     * 0x000A-0x000F.  With them the lane capacity TLVs move to IDs 8/9:
     * iOS accepts the identify but silently sends no 0x5204 LaneGuidance.
     */
    static const char name[] = "RouteGuidanceDisplayComponent";
    static const struct { uint16_t id, value; } limits[] = {
        { RGD_COMP_MAX_CURRENT_ROAD_NAME,      0x0100 },
        { RGD_COMP_MAX_DEST_NAME,              0x0100 },
        { RGD_COMP_MAX_AFTER_MANEUVER_NAME,    0x0100 },
        { RGD_COMP_MAX_MANEUVER_DESC,          0x0100 },
        { RGD_COMP_MAX_MANEUVER_CAPACITY,      0x0006 },
        { RGD_COMP_MAX_LANE_GUIDANCE_DESC,     0x0100 },
        { RGD_COMP_MAX_LANE_GUIDANCE_CAPACITY, 0x0006 },
    };
    const size_t n_limits = sizeof(limits) / sizeof(limits[0]);
    size_t name_tlv_len = 4 + sizeof(name);                 /* sizeof includes the NUL */
    size_t outer_len = 4 + 6 + name_tlv_len + 6 * n_limits;

    if (!out || max_len < outer_len) return 0;

    write_be16(out, (uint16_t)outer_len);
    write_be16(out + 2, IDENT_TLV_ROUTE_GUIDANCE_COMPONENT);
    size_t off = put_u16_tlv(out, 4, RGD_COMP_COMPONENT_ID, component_id);

    write_be16(out + off, (uint16_t)name_tlv_len);
    write_be16(out + off + 2, RGD_COMP_NAME);
    memcpy(out + off + 4, name, sizeof(name));
    off += name_tlv_len;

    for (size_t i = 0; i < n_limits; i++) off = put_u16_tlv(out, off, limits[i].id, limits[i].value);
    return off;
}

uint16_t rgd_extract_component_id(const uint8_t* tlv_start, size_t tlv_len) {
    if (!tlv_start || tlv_len < 10) return 0;

    /* TLV structure: outer_len(2) + outer_id(2) + inner TLVs */
    size_t inner_off = 4;
    while (inner_off + 4 <= tlv_len) {
        uint16_t inner_len = read_be16(tlv_start + inner_off);
        uint16_t inner_id = read_be16(tlv_start + inner_off + 2);

        if (inner_len < 4 || inner_off + inner_len > tlv_len) break;

        if (inner_id == RGD_COMP_COMPONENT_ID && inner_len >= 6) {
            return read_be16(tlv_start + inner_off + 4);
        }
        inner_off += inner_len;
    }
    return 0;
}
