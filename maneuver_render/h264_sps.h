/* The colour range an H.264 stream signals in its sequence parameter set. */
#ifndef CR_H264_SPS_H
#define CR_H264_SPS_H

#include <stdint.h>

/* The range the parameter sets in Annex B `unit` signal (H.264 VUI video_full_range_flag):
 * 1 full, 0 video, -1 none signalled (video, the default); `matrix` the matrix coefficients,
 * -1 when not given. */
int h264_sps_range(const uint8_t *unit, uint32_t bytes, int *matrix);

#endif
