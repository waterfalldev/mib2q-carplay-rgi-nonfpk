#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "framework/iap2_protocol.h"
#include "framework/logging.h"
#include "framework/state_trace.h"

static char captured[16384];
static size_t captured_len;

/* state_trace.c writes through the production logger.  Capture its formatted
 * records synchronously here so the parser/state machine can be tested on the
 * host without creating the logger thread. */
void log_write(log_level_t level, const char *module, const char *fmt, ...)
{
    va_list ap;
    int written;
    (void)level;
    (void)module;
    if (captured_len >= sizeof(captured) - 1u) return;
    va_start(ap, fmt);
    written = vsnprintf(captured + captured_len,
                        sizeof(captured) - captured_len, fmt, ap);
    va_end(ap);
    if (written <= 0) return;
    if ((size_t)written >= sizeof(captured) - captured_len)
        captured_len = sizeof(captured) - 1u;
    else
        captured_len += (size_t)written;
    if (captured_len < sizeof(captured) - 1u)
        captured[captured_len++] = '\n';
    captured[captured_len] = '\0';
}

static size_t append_tlv(unsigned char *out, size_t at, unsigned id,
                         const unsigned char *value, size_t value_len)
{
    write_be16(out + at, (uint16_t)(4u + value_len));
    write_be16(out + at + 2u, (uint16_t)id);
    if (value_len) memcpy(out + at + 4u, value, value_len);
    return at + 4u + value_len;
}

static int substring_count(const char *haystack, const char *needle)
{
    int count = 0;
    size_t needle_len = strlen(needle);
    while ((haystack = strstr(haystack, needle)) != NULL) {
        count++;
        haystack += needle_len;
    }
    return count;
}

int main(void)
{
    unsigned char media[16], playback[8], payload[40];
    unsigned char ack_packet[9] = {
        0xff, 0x5a, 0x00, 0x09, 0x80, 0x22, 0x12, 0x07, 0x00
    };
    size_t media_len = 0, playback_len = 0, payload_len = 0;

    assert(state_trace_begin_generation() == 1u);
    assert(strstr(captured, "name=IAP_IDENTIFY_START") != NULL);

    state_trace_note_iap_transport_tx(IAP2_MSG_NOW_PLAYING_START,
                                      0, 64, 1, 0x40, 0x11, 0x08, 0x07, 64);
    state_trace_feed_iap_transport_rx(ack_packet, 4);
    state_trace_feed_iap_transport_rx(ack_packet + 4, sizeof(ack_packet) - 4u);
    assert(strstr(captured, "name=IAP_LINK_RX_AFTER_5000") != NULL);
    assert(strstr(captured, "ack_eq_next=1") != NULL);

    media_len = append_tlv(media, media_len, 4, NULL, 0);
    media_len = append_tlv(media, media_len, 6,
                           (const unsigned char *)"x", 1);
    playback_len = append_tlv(playback, playback_len, 2, NULL, 0);
    payload_len = append_tlv(payload, payload_len, 0, media, media_len);
    payload_len = append_tlv(payload, payload_len, 1, playback, playback_len);
    state_trace_note_iap_semantic(MSG_DIR_INCOMING,
                                  IAP2_MSG_NOW_PLAYING_UPDATE,
                                  payload, payload_len);
    assert(strstr(captured, "media_groups=1 media_attrs=2") != NULL);
    assert(strstr(captured, "playback_groups=1 playback_attrs=1") != NULL);
    assert(strstr(captured, "media_mask=00000050") != NULL);
    assert(strstr(captured, "media_nonzero=00000040 media_value_bytes=1") != NULL);
    assert(strstr(captured, "playback_mask=00000004") != NULL);
    assert(strstr(captured, "playback_nonzero=00000000 playback_value_bytes=0") != NULL);

    state_trace_note_iap_semantic(MSG_DIR_OUTGOING,
                                  IAP2_MSG_ROUTE_GUIDANCE_START, NULL, 0);
    state_trace_note_iap_semantic(MSG_DIR_OUTGOING,
                                  IAP2_MSG_ROUTE_GUIDANCE_START, NULL, 0);
    assert(substring_count(captured, "name=IAP_RGI_FIRST") == 1);

    return 0;
}
