/*
 * The phone's cluster stream decoded by the head unit's hardware H.264 decoder, the way stock
 * libairplay decodes its CarPlay screen (docs/cluster/most-map-view.md, the native pipeline):
 * the OpenMAX core (CD_OMX_CORE, loaded at run time) and its CD_COMPONENT, the output port in
 * decode order, the parameter sets as a CODECCONFIG buffer, every access unit one ENDOFFRAME
 * buffer, the output in Qualcomm's TILE_4x2 (64 x 32 tiles: the only output this decoder
 * writes, map17-map18).  Unlike stock it decodes into buffers the decoder allocates, not the
 * window's, and the map thread untiles the newest picture into the NV12 map window, so the
 * window keeps its own life (map14) and a decoder that fails costs only the picture.
 *
 * cluster_decoder_feed / _poll / _close run on one thread (cluster_video.c); _copy and _wait on
 * the map thread (map_layer.c).  _copy untiles outside the decoder's lock, so the decoder never waits on it.
 */
#ifndef CR_CLUSTER_DECODER_H
#define CR_CLUSTER_DECODER_H

#include <stdint.h>

#ifndef CD_OMX_CORE
#define CD_OMX_CORE      "libOmxCore.so"             /* NULL: this program's own symbols (host tests) */
#endif
#define CD_COMPONENT     "OMX.qcom.video.decoder.avc"
#define CD_WIDTH         800                         /* the cluster display the hook advertises */
#define CD_HEIGHT        298
#define CD_FPS           30
#define CD_INPUT_BUFFERS 8                           /* at least; the decoder may want more */
#define CD_OUTPUT_EXTRA  2                           /* beyond the decoder's minimum: one shown, one copied */
#ifndef CD_STATE_WAIT_MS
#define CD_STATE_WAIT_MS 2000
#endif

/* One access unit of stream `stream` (Annex B; `config` for the parameter sets).  The first
 * config of a stream opens the decoder; a new stream closes the old one first.  0 when the
 * unit reached the decoder. */
int cluster_decoder_feed(uint32_t stream, int config, const uint8_t *unit, uint32_t bytes, uint64_t time_ns);

/* Hands pictures already copied back to the decoder; call between units. */
void cluster_decoder_poll(void);

/* Ends the current stream's decoder and logs its totals. */
void cluster_decoder_close(void);

/* Whether a picture newer than `serial` is waiting. */
int cluster_decoder_fresh(unsigned serial);

/* Copies the newest picture into an NV12 target of w x h when it is newer than *serial (which
 * it then updates), expanded from video to full range: 1 when copied. */
int cluster_decoder_copy(unsigned *serial, uint8_t *y, int y_stride, uint8_t *uv, int uv_stride, int w, int h);

/* Waits up to `ns` for a picture newer than `serial`, waking as soon as one arrives: 1 when one
 * is waiting. */
int cluster_decoder_wait(unsigned serial, int64_t ns);

#endif
