/*
 * CarPlay Hook Framework - iAP2 Protocol Helpers
 *
 * iAP2 frame parsing, building, and checksum utilities
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#ifndef CARPLAY_IAP2_PROTOCOL_H
#define CARPLAY_IAP2_PROTOCOL_H

#include "common.h"

/* iAP2 sync bytes */
#define IAP2_SYNC1      0x40
#define IAP2_SYNC2      0x40

/* iAP2 link header sync bytes */
#define IAP2_LINK_SYNC1 0xFF
#define IAP2_LINK_SYNC2 0x5A

/* Minimum frame size (sync + len + msgid) */
#define IAP2_MIN_FRAME  6

/* Common iAP2 message IDs */
#define IAP2_MSG_IDENTIFY_START           0x1D00
#define IAP2_MSG_IDENTIFY                 0x1D01
#define IAP2_MSG_IDENTIFY_ACCEPTED        0x1D02
#define IAP2_MSG_IDENTIFY_END             0x1D03
#define IAP2_MSG_AUTH_COMPLETE            0xAA05
#define IAP2_MSG_NOW_PLAYING_START        0x5000
#define IAP2_MSG_NOW_PLAYING_UPDATE       0x5001
#define IAP2_MSG_ROUTE_GUIDANCE_START     0x5200
#define IAP2_MSG_ROUTE_GUIDANCE_UPDATE    0x5201
#define IAP2_MSG_ROUTE_GUIDANCE_MANEUVER  0x5202
#define IAP2_MSG_ROUTE_GUIDANCE_STOP      0x5203
#define IAP2_MSG_ROUTE_GUIDANCE_LANE      0x5204
#define IAP2_MSG_LOCATION_INFO            0xFFFB
#define IAP2_MSG_STOP_LOCATION            0xFFFC

/* iAP2 frame info */
typedef struct {
    uint16_t msgid;             /* Message ID */
    uint16_t frame_len;         /* Frame length (from header) */
    size_t offset;              /* Offset to frame start in buffer */
    const uint8_t* data;        /* Frame start (the 0x4040 sync); set by the Decode/Encode taps only */
    const uint8_t* payload;     /* Pointer to payload (after msgid) */
    size_t payload_len;         /* Payload length */
} iap2_frame_t;

/* Find iAP2 frame in buffer
 * Returns true if found, fills frame info */
bool iap2_find_frame(const uint8_t* buf, size_t len, iap2_frame_t* frame);

/* Validate frame (check sync, length, etc.) */
bool iap2_validate_frame(const uint8_t* buf, size_t len);

/* Parse message ID from buffer */
int iap2_parse_msgid(const uint8_t* buf, size_t len);

/* Find TLV offset in payload (returns 0 if not found) */
size_t iap2_find_tlv_offset(const uint8_t* payload, size_t len, uint16_t tlv_id, uint16_t* out_len);

/* Build simple iAP2 frame (sync + len + msgid + payload)
 * Returns frame size, or 0 on error */
size_t iap2_build_frame(uint8_t* out, size_t out_max, uint16_t msgid,
                        const uint8_t* payload, size_t payload_len);

/* iAP2 link-layer checksum: negated 8-bit sum of payload bytes. */
uint8_t iap2_cksum_neg(const uint8_t* buf, size_t len);

/* Link header helpers */
typedef struct {
    uint16_t length;
    uint8_t ctrl;
    uint8_t seq;
    uint8_t ack;
    uint8_t session;
    uint8_t checksum;
} iap2_link_header_t;

/* Parse link header (9 bytes: FF 5A len[2] ctrl seq ack sess cksum) */
bool iap2_parse_link_header(const uint8_t* buf, size_t len, iap2_link_header_t* hdr);

#endif /* CARPLAY_IAP2_PROTOCOL_H */
