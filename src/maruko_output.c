#include "maruko_output.h"

#include "venc_config.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void maruko_output_reset(MarukoOutput *output)
{
	if (!output)
		return;

	output->socket_handle = -1;
	output->ring = NULL;
	output->dst_len = 0;
	output->transport = MARUKO_OUTPUT_TRANSPORT_UDP;
	memset(&output->dst, 0, sizeof(output->dst));
}

static int maruko_output_open_socket(MarukoOutput *output,
	MarukoOutputTransport transport)
{
	int domain = transport == MARUKO_OUTPUT_TRANSPORT_UNIX ? AF_UNIX : AF_INET;

	if (!output)
		return -1;

	if (output->socket_handle >= 0) {
		close(output->socket_handle);
		output->socket_handle = -1;
	}

	output->socket_handle = socket(domain, SOCK_DGRAM, 0);
	if (output->socket_handle < 0) {
		fprintf(stderr, "ERROR: [maruko] unable to create %s socket\n",
			transport == MARUKO_OUTPUT_TRANSPORT_UNIX ? "Unix" : "UDP");
		return -1;
	}
	output->transport = transport;
	return 0;
}

int maruko_output_init(MarukoOutput *output, uint32_t sink_ip,
	uint16_t sink_port)
{
	struct sockaddr_in *dst;

	if (!output)
		return -1;

	maruko_output_reset(output);
	if (maruko_output_open_socket(output, MARUKO_OUTPUT_TRANSPORT_UDP) != 0)
		return -1;

	dst = (struct sockaddr_in *)&output->dst;
	dst->sin_family = AF_INET;
	dst->sin_port = htons(sink_port);
	dst->sin_addr.s_addr = sink_ip;
	output->dst_len = sizeof(*dst);
	return 0;
}

int maruko_output_init_unix(MarukoOutput *output, const char *socket_name)
{
	struct sockaddr_un *dst;
	size_t name_len;

	if (!output || !socket_name || !socket_name[0])
		return -1;

	maruko_output_reset(output);
	if (maruko_output_open_socket(output, MARUKO_OUTPUT_TRANSPORT_UNIX) != 0)
		return -1;

	name_len = strlen(socket_name);
	if (name_len > sizeof(dst->sun_path) - 2) {
		fprintf(stderr, "ERROR: [maruko] unix:// socket name too long\n");
		return -1;
	}

	dst = (struct sockaddr_un *)&output->dst;
	dst->sun_family = AF_UNIX;
	memcpy(dst->sun_path + 1, socket_name, name_len);
	output->dst_len = (socklen_t)(sizeof(sa_family_t) + name_len + 1);
	return 0;
}

int maruko_output_init_shm(MarukoOutput *output, const char *shm_name,
	uint16_t max_payload)
{
	uint32_t slot_data;

	if (!output || !shm_name || !shm_name[0])
		return -1;

	maruko_output_reset(output);

	slot_data = (uint32_t)max_payload + 12;
	output->ring = venc_ring_create(shm_name, 512, slot_data);
	if (!output->ring) {
		fprintf(stderr, "ERROR: [maruko] venc_ring_create(%s) failed\n",
			shm_name);
		return -1;
	}

	printf("> [maruko] SHM output: %s (slot_data=%u)\n", shm_name,
		slot_data);
	return 0;
}

int maruko_output_apply_server(MarukoOutput *output, const char *uri)
{
	VencOutputUri parsed;

	if (!output || !uri)
		return -1;

	/* SHM output doesn't support live server change */
	if (output->ring) {
		fprintf(stderr, "ERROR: [maruko] cannot change server in SHM mode\n");
		return -1;
	}

	if (venc_config_parse_output_uri(uri, &parsed) != 0)
		return -1;
	if (parsed.type == VENC_OUTPUT_URI_SHM) {
		fprintf(stderr, "ERROR: [maruko] cannot change server to shm:// live\n");
		return -1;
	}
	if (parsed.type == VENC_OUTPUT_URI_UNIX)
		return maruko_output_init_unix(output, parsed.endpoint);

	return maruko_output_init(output, inet_addr(parsed.host), parsed.port);
}

void maruko_output_teardown(MarukoOutput *output)
{
	if (!output)
		return;

	if (output->ring) {
		venc_ring_destroy(output->ring);
		output->ring = NULL;
	}
	if (output->socket_handle >= 0) {
		close(output->socket_handle);
		output->socket_handle = -1;
	}
	memset(&output->dst, 0, sizeof(output->dst));
	output->dst_len = 0;
	output->transport = MARUKO_OUTPUT_TRANSPORT_UDP;
}
