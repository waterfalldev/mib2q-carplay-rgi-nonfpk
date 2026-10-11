/* The colour range an H.264 stream signals in its SPS (h264_sps.h). */
#include "h264_sps.h"

#include <stddef.h>
#include <string.h>

/* An SPS read as bits, emulation prevention removed.  Past the end it reads zeros and sets
 * `over`. */
typedef struct { uint8_t b[256]; size_t n, at; int over; } sps_bits_t;

static unsigned sps_u(sps_bits_t *s, int bits) {
    unsigned v = 0;
    while (bits-- > 0) {
        if (s->at >= s->n * 8) { s->over = 1; v <<= 1; continue; }
        v = v << 1 | (unsigned)(s->b[s->at / 8] >> (7 - s->at % 8) & 1);
        ++s->at;
    }
    return v;
}

static unsigned sps_ue(sps_bits_t *s) {
    int zeros = 0;
    while (!sps_u(s, 1) && !s->over && zeros < 31) ++zeros;
    return ((1u << zeros) - 1) + sps_u(s, zeros);
}

static int sps_se(sps_bits_t *s) {
    unsigned k = sps_ue(s);
    return k & 1 ? (int)((k + 1) / 2) : -(int)(k / 2);
}

static void sps_skip_scaling_list(sps_bits_t *s, int size) {
    int last = 8, next = 8, j;
    for (j = 0; j < size; ++j) {
        if (next) next = (last + sps_se(s) + 256) % 256;
        if (next) last = next;
    }
}

int h264_sps_range(const uint8_t *unit, uint32_t bytes, int *matrix) {
    sps_bits_t s;
    uint32_t i = 0;
    unsigned profile, v, k;
    *matrix = -1;
    while (i + 3 < bytes && !(unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1 && (unit[i + 3] & 0x1f) == 7)) ++i;
    if (i + 3 >= bytes) return -1;
    memset(&s, 0, sizeof(s));
    for (i += 4; i < bytes && s.n < sizeof(s.b); ++i) {
        if (i + 2 < bytes && unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] <= 1) break;   /* next start code */
        if (i >= 2 && unit[i] == 3 && unit[i - 1] == 0 && unit[i - 2] == 0) continue;     /* emulation prevention */
        s.b[s.n++] = unit[i];
    }
    profile = sps_u(&s, 8);
    sps_u(&s, 16);                                              /* constraints, level */
    sps_ue(&s);                                                 /* seq_parameter_set_id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83
        || profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139
        || profile == 134 || profile == 135) {
        unsigned chroma = sps_ue(&s);
        if (chroma == 3) sps_u(&s, 1);
        sps_ue(&s);
        sps_ue(&s);                                             /* bit depths */
        sps_u(&s, 1);
        if (sps_u(&s, 1))                                       /* seq_scaling_matrix_present */
            for (k = 0; k < (chroma == 3 ? 12u : 8u); ++k)
                if (sps_u(&s, 1)) sps_skip_scaling_list(&s, k < 6 ? 16 : 64);
    }
    sps_ue(&s);                                                 /* log2_max_frame_num */
    v = sps_ue(&s);                                             /* pic_order_cnt_type */
    if (v == 0) sps_ue(&s);
    else if (v == 1) {
        sps_u(&s, 1);
        sps_ue(&s);
        sps_ue(&s);
        for (k = sps_ue(&s); k > 0 && !s.over; --k) sps_ue(&s);
    }
    sps_ue(&s);
    sps_u(&s, 1);                                               /* reference frames, gaps */
    sps_ue(&s);
    sps_ue(&s);                                                 /* size */
    if (!sps_u(&s, 1)) sps_u(&s, 1);                            /* frame_mbs_only, else mbaff */
    sps_u(&s, 1);                                               /* direct_8x8 */
    if (sps_u(&s, 1)) for (k = 0; k < 4; ++k) sps_ue(&s);       /* cropping */
    if (!sps_u(&s, 1)) return -1;                               /* no VUI */
    if (sps_u(&s, 1) && sps_u(&s, 8) == 255) sps_u(&s, 32);     /* aspect ratio */
    if (sps_u(&s, 1)) sps_u(&s, 1);                             /* overscan */
    if (!sps_u(&s, 1)) return -1;                               /* no video signal type */
    sps_u(&s, 3);
    v = sps_u(&s, 1);
    if (sps_u(&s, 1)) {                                         /* colour description */
        sps_u(&s, 16);
        k = sps_u(&s, 8);
        if (!s.over) *matrix = (int)k;
    }
    return s.over ? -1 : (int)v;
}
