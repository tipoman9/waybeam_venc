#include "opt_flow.h"

#include "opt_flow_impl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*OptflowOnStreamFn)(void *state, uint32_t pack_count);
typedef void (*OptflowDestroyFn)(void *state);
typedef void (*OptflowDrawOsdFn)(void *state);

struct OptFlowState {
	void *impl_state;
	OptflowOnStreamFn on_stream;
	OptflowDestroyFn destroy;
	OptflowDrawOsdFn draw_osd;
	char mode[16];
};

static const char *normalize_mode(const char *mode)
{
	if (mode && strcmp(mode, "sad") == 0)
		return "sad";
	return "lk";
}

static const char *mode_label(const char *mode)
{
	if (strcmp(mode, "sad") == 0)
		return "SAD";
	return "LK";
}

OptFlowState *optflow_create(uint32_t capture_width, uint32_t capture_height,
	uint32_t osd_space_width, uint32_t osd_space_height,
	int verbose, uint32_t fps, const char *mode,
	const void *vpe_port, struct DebugOsdState *osd)
{
	OptFlowState *state;
	const char *normalized_mode = normalize_mode(mode);

	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	if (strcmp(normalized_mode, "sad") == 0) {
		state->impl_state = optflow_sad_create_impl(capture_width,
			capture_height, osd_space_width, osd_space_height,
			verbose, fps, vpe_port, osd);
		state->on_stream = optflow_sad_on_stream_impl;
		state->destroy = optflow_sad_destroy_impl;
		state->draw_osd = optflow_sad_draw_osd_impl;
	} else {
		state->impl_state = optflow_lk_create_impl(capture_width,
			capture_height, osd_space_width, osd_space_height,
			verbose, fps, vpe_port, osd);
		state->on_stream = optflow_lk_on_stream_impl;
		state->destroy = optflow_lk_destroy_impl;
		state->draw_osd = optflow_lk_draw_osd_impl;
	}

	if (!state->impl_state) {
		free(state);
		return NULL;
	}

	strncpy(state->mode, normalized_mode, sizeof(state->mode) - 1);
	state->mode[sizeof(state->mode) - 1] = '\0';
	printf("[optflow][%s] mode=%s\n", mode_label(state->mode), state->mode);
	fflush(stdout);
	return state;
}

void optflow_on_stream(OptFlowState *state, uint32_t pack_count)
{
	if (!state || !state->impl_state || !state->on_stream)
		return;
	state->on_stream(state->impl_state, pack_count);
}

void optflow_draw_osd(OptFlowState *state)
{
	if (!state || !state->impl_state || !state->draw_osd)
		return;
	state->draw_osd(state->impl_state);
}

void optflow_destroy(OptFlowState *state)
{
	if (!state)
		return;
	if (state->impl_state && state->destroy)
		state->destroy(state->impl_state);
	free(state);
}