#include "maruko_config.h"

#include "codec_config.h"
#include "pipeline_common.h"
#include "rtp_packetizer.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

void maruko_config_defaults(MarukoBackendConfig *cfg)
{
	if (!cfg) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	cfg->sensor_width = 1920;
	cfg->sensor_height = 1080;
	cfg->image_width = 1920;
	cfg->image_height = 1080;
	cfg->sensor_fps = 30;
	cfg->venc_max_rate = 8192;
	cfg->venc_gop_size = 30;
	cfg->venc_gop_seconds = 1.0;
	cfg->max_frame_size = RTP_DEFAULT_PAYLOAD;
	cfg->udp_sink_ip = inet_addr("127.0.0.1");
	cfg->udp_sink_port = 5000;
	cfg->rc_codec = PT_H265;
	cfg->rc_mode = 3;
	cfg->stream_mode = MARUKO_STREAM_RTP;
	cfg->forced_sensor_pad = (MI_SNR_PAD_ID_e)-1;
	cfg->forced_sensor_mode = -1;
	cfg->isp_bin_path = NULL;
	cfg->vpe_level_3dnr = 1;
	cfg->verbose = 0;
}

int maruko_config_from_venc(const VencConfig *vcfg, MarukoBackendConfig *cfg)
{
	VencOutputUri output_uri;

	if (!vcfg || !cfg) {
		return -1;
	}

	maruko_config_defaults(cfg);

	cfg->sensor_width = vcfg->video0.width;
	cfg->sensor_height = vcfg->video0.height;
	cfg->image_width = vcfg->video0.width;
	cfg->image_height = vcfg->video0.height;
	cfg->sensor_fps = vcfg->video0.fps;
	cfg->venc_max_rate = vcfg->video0.bitrate;
	cfg->venc_gop_seconds = vcfg->video0.gop_size;
	cfg->venc_gop_size = pipeline_common_gop_frames(cfg->venc_gop_seconds,
		cfg->sensor_fps);

	cfg->max_frame_size = vcfg->outgoing.max_payload_size;
	cfg->rtp_payload_size = vcfg->outgoing.max_payload_size;
	cfg->sidecar_port = vcfg->outgoing.sidecar_port;
	cfg->output_uri_type = VENC_OUTPUT_URI_UDP;

	if (vcfg->outgoing.server[0]) {
		if (venc_config_parse_output_uri(vcfg->outgoing.server,
		    &output_uri) != 0)
			return -1;

		cfg->output_uri_type = output_uri.type;
		if (output_uri.type == VENC_OUTPUT_URI_SHM) {
			snprintf(cfg->shm_name, sizeof(cfg->shm_name), "%s",
				output_uri.endpoint);
		} else if (output_uri.type == VENC_OUTPUT_URI_UNIX) {
			snprintf(cfg->unix_socket_name, sizeof(cfg->unix_socket_name), "%s",
				output_uri.endpoint);
		} else {
			cfg->udp_sink_ip = inet_addr(output_uri.host);
			cfg->udp_sink_port = output_uri.port;
		}
	}

	cfg->stream_mode =
		(strcmp(vcfg->outgoing.stream_mode, "compact") == 0) ?
		MARUKO_STREAM_COMPACT : MARUKO_STREAM_RTP;

	if (codec_config_resolve_codec_rc(vcfg->video0.codec, vcfg->video0.rc_mode,
	    &cfg->rc_codec, &cfg->rc_mode) != 0) {
		return -1;
	}

	cfg->forced_sensor_pad = (vcfg->sensor.index >= 0) ?
		(MI_SNR_PAD_ID_e)vcfg->sensor.index : (MI_SNR_PAD_ID_e)-1;
	cfg->forced_sensor_mode = vcfg->sensor.mode;
	cfg->isp_bin_path = vcfg->isp.sensor_bin[0] ?
		vcfg->isp.sensor_bin : NULL;
	cfg->vpe_level_3dnr = vcfg->fpv.noise_level;
	cfg->exposure_cap_us = vcfg->isp.exposure * 1000;
	cfg->sensor_unlock = (SensorUnlockConfig){
		.enabled = vcfg->sensor.unlock_enabled ? 1 : 0,
		.cmd_id = vcfg->sensor.unlock_cmd,
		.reg = vcfg->sensor.unlock_reg,
		.value = vcfg->sensor.unlock_value,
		.dir = (MI_SNR_CustDir_e)vcfg->sensor.unlock_dir,
	};
	cfg->verbose = vcfg->system.verbose ? 1 : 0;

	return 0;
}
