#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Embedded wfb-ng TX module.
 *
 * wfb_tx_init()   — parse the same CLI args as wfb_tx (minus socket flags
 *                   -U/-u/-T/-C), construct the RawSocketTransmitter, and
 *                   send initial session keys.  The last positional argument
 *                   must be the wlan interface name.
 *                   Returns 0 on success, -1 on error.
 *
 * wfb_tx_setup_radio() — update radiotap / MCS parameters at runtime.
 *                        Mirrors CMD_SET_RADIO in data_source().
 *                        Returns 0 on success, -1 on error.
 *
 * wfb_tx_setup_fec()   — change FEC k/n at runtime, closes any open block,
 *                        reinitialises the session, and resends session keys.
 *                        Mirrors CMD_SET_FEC in data_source().
 *                        Returns 0 on success, -1 on error.
 *
 * wfb_tx_send()        — encrypt, FEC-encode, and inject one payload packet.
 *                        Announces the session key when the 1-second timer
 *                        fires.  Call once per RTP packet from the encoder.
 *
 * wfb_tx_destroy()     — tear down the transmitter and release all resources.
 */

int  wfb_tx_init(int argc, char * const *argv);

/* Convenience: parse a space-separated args string (no argv[0] needed)
 * and call wfb_tx_init().  Flags -U/-C/-T/-d are accepted and silently
 * ignored (they are socket-transport flags unused in embedded mode). */
int  wfb_tx_init_from_str(const char *args_str);

int  wfb_tx_setup_radio(uint8_t stbc, int ldpc, int short_gi,
                         uint8_t bandwidth, uint8_t mcs_index,
                         int vht_mode, uint8_t vht_nss);

int  wfb_tx_setup_fec(int k, int n);

void wfb_tx_send(const uint8_t *buf, size_t size);

void wfb_tx_destroy(void);

#ifdef __cplusplus
}
#endif
