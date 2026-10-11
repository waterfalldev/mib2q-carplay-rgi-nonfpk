/*
 * CarPlay Hook Framework - iAP2 Protocol Implementation
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#include "iap2_protocol.h"

bool iap2_find_frame(const uint8_t* buf, size_t len, iap2_frame_t* frame) {
    if (!buf || len < IAP2_MIN_FRAME || !frame) return false;

    size_t max = len - 5;
    for (size_t i = 0; i < max; i++) {
        if (buf[i] != IAP2_SYNC1 || buf[i + 1] != IAP2_SYNC2) continue;

        uint16_t flen = read_be16(buf + i + 2);
        if (flen < IAP2_MIN_FRAME) continue;
        if (i + flen > len) continue;

        frame->offset = i;
        frame->frame_len = flen;
        frame->msgid = read_be16(buf + i + 4);
        frame->payload = (flen > 6) ? (buf + i + 6) : NULL;
        frame->payload_len = (flen > 6) ? (flen - 6) : 0;
        return true;
    }
    return false;
}

bool iap2_validate_frame(const uint8_t* buf, size_t len) {
    if (!buf || len < IAP2_MIN_FRAME) return false;
    if (buf[0] != IAP2_SYNC1 || buf[1] != IAP2_SYNC2) return false;
    uint16_t flen = read_be16(buf + 2);
    if (flen < IAP2_MIN_FRAME || flen > len) return false;
    return true;
}

int iap2_parse_msgid(const uint8_t* buf, size_t len) {
    if (!buf || len < IAP2_MIN_FRAME) return -1;
    if (buf[0] != IAP2_SYNC1 || buf[1] != IAP2_SYNC2) return -1;
    return (int)read_be16(buf + 4);
}

size_t iap2_find_tlv_offset(const uint8_t* payload, size_t len, uint16_t tlv_id, uint16_t* out_len) {
    if (!payload || len < 4) return 0;

    size_t off = 0;
    while (off + 4 <= len) {
        uint16_t tlen = read_be16(payload + off);
        uint16_t tid = read_be16(payload + off + 2);

        if (tlen < 4 || off + tlen > len) return 0;

        if (tid == tlv_id) {
            if (out_len) *out_len = tlen;
            return off;
        }
        off += tlen;
    }
    return 0;
}

size_t iap2_build_frame(uint8_t* out, size_t out_max, uint16_t msgid,
                        const uint8_t* payload, size_t payload_len) {
    size_t frame_len = 6 + payload_len;
    if (!out || out_max < frame_len) return 0;

    out[0] = IAP2_SYNC1;
    out[1] = IAP2_SYNC2;
    write_be16(out + 2, (uint16_t)frame_len);
    write_be16(out + 4, msgid);

    if (payload && payload_len > 0) {
        memcpy(out + 6, payload, payload_len);
    }

    return frame_len;
}

/* iAP2 link-layer checksum: negated 8-bit sum.
 * Apple's iAP2 USB Host transport (R12+) uses this exclusively. */
uint8_t iap2_cksum_neg(const uint8_t* buf, size_t len) {
    uint8_t sum = 0;
    if (!buf) return 0;
    for (size_t i = 0; i < len; i++) {
        sum += buf[i];
    }
    return (uint8_t)(0u - (unsigned)sum);
}

bool iap2_parse_link_header(const uint8_t* buf, size_t len, iap2_link_header_t* hdr) {
    if (!buf || len < 9 || !hdr) return false;
    if (buf[0] != IAP2_LINK_SYNC1 || buf[1] != IAP2_LINK_SYNC2) return false;

    hdr->length = read_be16(buf + 2);
    hdr->ctrl = buf[4];
    hdr->seq = buf[5];
    hdr->ack = buf[6];
    hdr->session = buf[7];
    hdr->checksum = buf[8];

    return true;
}
