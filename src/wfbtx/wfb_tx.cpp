// Embedded wfb-ng TX module for venc.
// Wraps RawSocketTransmitter with a C-compatible API.
// Key file /etc/drone.key is the default; override with -K.

#include "wfb_tx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

#include "zfex.h"

using namespace std;

#include "wifibroadcast.hpp"
#include "tx.hpp"

static unique_ptr<RawSocketTransmitter> g_tx;
static uint64_t g_session_key_ts = 0;

int wfb_tx_init(int argc, char * const *argv)
{
    int opt;
    uint8_t k = 8, n = 12, radio_port = 0;
    uint32_t fec_delay = 0;
    uint32_t link_id = 0;
    uint64_t epoch = 0;
    int bandwidth = 20;
    int short_gi = 0;
    int stbc = 0;
    int ldpc = 0;
    int mcs_index = 1;
    int vht_nss = 1;
    bool vht_mode = false;
    bool mirror = false;
    bool use_qdisc = false;
    uint32_t fwmark = 0;
    uint32_t inject_retries = 10;
    uint32_t inject_retry_delay = 5000;
    string keypair = "/etc/drone.key";
    uint8_t frame_type = FRAME_TYPE_DATA;

    optind = 1;
    while ((opt = getopt(argc, (char * const *)argv,
                         "K:k:n:p:F:B:G:S:L:M:N:i:e:f:mVQP:J:E:U:C:T:d:")) != -1) {
        switch (opt) {
        case 'K': keypair = optarg; break;
        case 'k': k = (uint8_t)atoi(optarg); break;
        case 'n': n = (uint8_t)atoi(optarg); break;
        case 'p': radio_port = (uint8_t)atoi(optarg); break;
        case 'F': fec_delay = (uint32_t)atoi(optarg); break;
        case 'B':
            bandwidth = atoi(optarg);
            if (bandwidth >= 80) vht_mode = true;
            break;
        case 'G': short_gi = (optarg[0] == 's' || optarg[0] == 'S') ? 1 : 0; break;
        case 'S': stbc = atoi(optarg); break;
        case 'L': ldpc = atoi(optarg); break;
        case 'M': mcs_index = atoi(optarg); break;
        case 'N': vht_nss = atoi(optarg); break;
        case 'i': link_id = ((uint32_t)atoi(optarg)) & 0xffffff; break;
        case 'e': epoch = (uint64_t)atoll(optarg); break;
        case 'm': mirror = true; break;
        case 'V': vht_mode = true; break;
        case 'f':
            frame_type = (strcmp(optarg, "rts") == 0) ?
                         FRAME_TYPE_RTS : FRAME_TYPE_DATA;
            break;
        case 'Q': use_qdisc = true; break;
        case 'P': fwmark = (uint32_t)atoi(optarg); break;
        case 'J': inject_retries = (uint32_t)atoi(optarg); break;
        case 'E': inject_retry_delay = (uint32_t)atoi(optarg); break;
        /* Socket-transport flags from the standalone wfb_tx CLI — silently
         * ignored in embedded mode since there is no socket to read from. */
        case 'U': case 'C': case 'T': case 'd': break;
        default:
            WFB_ERR("wfb_tx_init: unknown option\n");
            return -1;
        }
    }

    if (optind >= argc) {
        WFB_ERR("wfb_tx_init: no wlan interface specified\n");
        return -1;
    }

    if (sodium_init() < 0) {
        WFB_ERR("wfb_tx_init: libsodium init failed\n");
        return -1;
    }

    try {
        auto radiotap_header = init_radiotap_header(
            (uint8_t)stbc, (bool)ldpc, (bool)short_gi,
            (uint8_t)bandwidth, (uint8_t)mcs_index,
            (bool)vht_mode, (uint8_t)vht_nss);

        uint32_t channel_id = (link_id << 8) + radio_port;

        vector<string> wlans;
        for (int i = optind; i < argc; i++)
            wlans.push_back(string(argv[i]));

        vector<tags_item_t> tags;

        g_tx.reset(new RawSocketTransmitter(
            k, n, keypair, epoch, channel_id, fec_delay,
            tags, wlans, radiotap_header, frame_type,
            use_qdisc, fwmark, inject_retries, inject_retry_delay));

        g_tx->select_output(mirror ? -1 : 0);

        // Initial session key burst
        int fec_k, fec_n;
        g_tx->get_fec(fec_k, fec_n);
        for (int i = 0; i < fec_n - fec_k + 1; i++)
            g_tx->send_session_key();

        g_session_key_ts = get_time_ms() + SESSION_KEY_ANNOUNCE_MSEC;

        WFB_INFO("wfb_tx: init ok, k=%d n=%d bw=%d mcs=%d iface=%s\n",
                 fec_k, fec_n, bandwidth, mcs_index, wlans[0].c_str());
    } catch (runtime_error &e) {
        WFB_ERR("wfb_tx_init: %s\n", e.what());
        return -1;
    }

    return 0;
}

int wfb_tx_setup_radio(uint8_t stbc, int ldpc, int short_gi,
                        uint8_t bandwidth, uint8_t mcs_index,
                        int vht_mode, uint8_t vht_nss)
{
    if (!g_tx) return -1;

    try {
        auto hdr = init_radiotap_header(
            stbc, (bool)ldpc, (bool)short_gi,
            bandwidth, mcs_index,
            (bool)vht_mode, vht_nss);
        g_tx->update_radiotap_header(hdr);
        WFB_INFO("wfb_tx: radio updated bw=%d mcs=%d\n", bandwidth, mcs_index);
    } catch (runtime_error &e) {
        WFB_ERR("wfb_tx_setup_radio: %s\n", e.what());
        return -1;
    }
    return 0;
}

int wfb_tx_setup_fec(int k, int n)
{
    if (!g_tx) return -1;

    if (!(k <= n && k >= 1 && n >= 1 && n < 256)) {
        WFB_ERR("wfb_tx_setup_fec: invalid k=%d n=%d\n", k, n);
        return -1;
    }

    try {
        // Close any open FEC block before reinitialising
        while (g_tx->send_packet(NULL, 0, WFB_PACKET_FEC_ONLY));

        g_tx->init_session(k, n);

        int fec_k, fec_n;
        g_tx->get_fec(fec_k, fec_n);
        for (int i = 0; i < fec_n - fec_k + 1; i++)
            g_tx->send_session_key();

        g_session_key_ts = get_time_ms() + SESSION_KEY_ANNOUNCE_MSEC;
        WFB_INFO("wfb_tx: FEC updated k=%d n=%d\n", fec_k, fec_n);
    } catch (runtime_error &e) {
        WFB_ERR("wfb_tx_setup_fec: %s\n", e.what());
        return -1;
    }
    return 0;
}

void wfb_tx_send(const uint8_t *buf, size_t size)
{
    if (!g_tx) return;

    try {
        uint64_t cur_ts = get_time_ms();

        if (cur_ts >= g_session_key_ts) {
            g_tx->send_session_key();
            g_session_key_ts = cur_ts + SESSION_KEY_ANNOUNCE_MSEC;
        }

        g_tx->send_packet(buf, size, 0);
    } catch (runtime_error &e) {
        WFB_ERR("wfb_tx_send: %s\n", e.what());
    }
}

void wfb_tx_destroy(void)
{
    g_tx.reset();
    g_session_key_ts = 0;
}

int wfb_tx_init_from_str(const char *args_str)
{
    char buf[512];
    char *argv_ptrs[32];
    int argc = 0;
    char *tok, *saveptr;

    if (!args_str)
        return -1;

    strncpy(buf, args_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    argv_ptrs[argc++] = (char *)"wfb_tx";
    tok = strtok_r(buf, " \t", &saveptr);
    while (tok && argc < 31) {
        argv_ptrs[argc++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }
    argv_ptrs[argc] = NULL;

    return wfb_tx_init(argc, argv_ptrs);
}
