/*
 * The phone's cluster stream as the renderer receives it: while the map window wants pictures,
 * one thread follows the hook's frame ring (common/cluster_video_ring.h) from the stream's
 * newest config and key frame and feeds every access unit to the hardware decoder
 * (cluster_decoder.h), starting over there whenever the decoder missed one.  When the ring
 * holds no decodable point it asks for a key frame (at most every CV_KEYFRAME_RETRY_MS).  The
 * decoder is closed when the window no longer wants pictures or the stream stops.
 */
#ifndef CR_CLUSTER_VIDEO_H
#define CR_CLUSTER_VIDEO_H

#define CV_KEYFRAME_RETRY_MS 2000

/* Starts the thread (once); cluster_video_stop() asks it to end within ~100 ms. */
void cluster_video_start(void);
void cluster_video_stop(void);

/* Whether the map window wants decoded pictures; from the map thread, every pass. */
void cluster_video_want(int wanted);

#endif
