#ifndef OPTFLOW_H
#define OPTFLOW_H

#include <stdint.h>

typedef struct OptFlowState OptFlowState;
struct DebugOsdState;

/** Create Star6E optical-flow tracker state. Returns NULL on allocation failure.
 *  osd may be NULL to disable the OSD overlay. */
OptFlowState *optflow_create(uint32_t capture_width, uint32_t capture_height,
	uint32_t osd_space_width, uint32_t osd_space_height,
	int verbose, uint32_t fps, const char *mode,
	const void *vpe_port, struct DebugOsdState *osd);

/** Notify the tracker that one encoded frame was observed in the stream loop. */
void optflow_on_stream(OptFlowState *state, uint32_t pack_count);

/** Draw the current optflow overlay (marker + track points) onto the shared
 *  OSD canvas.  Must be called between debug_osd_begin_frame and
 *  debug_osd_end_frame.  No-op if OSD was not configured. */
void optflow_draw_osd(OptFlowState *state);

/** Destroy tracker state and emit a short shutdown summary. */
void optflow_destroy(OptFlowState *state);

#endif /* OPTFLOW_H */