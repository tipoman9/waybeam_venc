#ifndef STAR6E_HEVC_RTP_H
#define STAR6E_HEVC_RTP_H

#include "h26x_param_sets.h"
#include "rtp_packetizer.h"
#include "star6e.h"
#include "star6e_output.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t total_nals;
	uint32_t single_packets;
	uint32_t ap_packets;
	uint32_t ap_nals;
	uint32_t fu_packets;
	uint32_t rtp_packets;
	uint32_t rtp_payload_bytes;
} Star6eHevcRtpStats;

/** Packetize and send one encoder stream result as HEVC RTP packets.
 *  end_of_frame: 1 when this stream contains the last slice of a frame
 *  (always 1 in non-slice mode). Controls marker bit and timestamp advance.
 *  per_slice_au: when 1, each VCL slice is sent as its own RTP access unit
 *  with a VPS/SPS/PPS prefix, marker bit, and per-slice timestamp advance.
 *  slice_ticks: RTP timestamp increment per slice (== frame_ticks when
 *  per_slice_au=0; == frame_ticks/num_slices when per_slice_au=1,
 *  pre-computed from config by the caller). */
size_t star6e_hevc_rtp_send_frame(const MI_VENC_Stream_t *stream,
	Star6eOutput *output, RtpPacketizerState *rtp,
	uint32_t frame_ticks, uint32_t slice_ticks, H26xParamSets *params,
	size_t max_payload, Star6eHevcRtpStats *stats, int end_of_frame,
	int no_aggregation, int per_slice_au);

#endif /* STAR6E_HEVC_RTP_H */
