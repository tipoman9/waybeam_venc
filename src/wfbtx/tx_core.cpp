// -*- C++ -*-
//
// Copyright (C) 2017 - 2024 Vasily Evseenko <svpcom@p2ptech.org>

/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 3.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/resource.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/random.h>
#include <inttypes.h>

#include <string>
#include <memory>
#include <vector>
#include <set>

#include "zfex.h"


using namespace std;

#include "wifibroadcast.hpp"
#include "tx.hpp"

Transmitter::Transmitter(int k, int n, const string &keypair, uint64_t epoch, uint32_t channel_id, uint32_t fec_delay, vector<tags_item_t> &tags) : \
    fec_p(NULL), fec_k(-1), fec_n(-1),
    block_idx(0), fragment_idx(0),
    max_packet_size(0),
    epoch(epoch),
    channel_id(channel_id),
    fec_delay(fec_delay),
    tx_secretkey{},
    rx_publickey{},
    session_key{},
    session_packet{},
    session_packet_size(0),
    tags(tags),
    encrypt_us_acc(0), fec_us_acc(0), inject_us_acc(0),
    metrics_window_start_us(0),
    delay_us_sum(0), delay_count(0), delay_max_us(0),
    pkt_ring{}, ring_pos(0), ring_fill(0), group10_max_us(0)
{

    FILE *fp;
    if ((fp = fopen(keypair.c_str(), "r")) == NULL)
    {
        throw runtime_error(string_format("Unable to open %s: %s", keypair.c_str(), strerror(errno)));
    }
    if (fread(tx_secretkey, crypto_box_SECRETKEYBYTES, 1, fp) != 1)
    {
        fclose(fp);
        throw runtime_error(string_format("Unable to read tx secret key: %s", strerror(errno)));
    }
    if (fread(rx_publickey, crypto_box_PUBLICKEYBYTES, 1, fp) != 1)
    {
        fclose(fp);
        throw runtime_error(string_format("Unable to read rx public key: %s", strerror(errno)));
    }
    fclose(fp);

    init_session(k, n);
}

Transmitter::~Transmitter()
{
    if (fec_p != NULL)
    {
        deinit_session();
    }
}


void Transmitter::deinit_session(void)
{
    for(int i=0; i < fec_n; i++)
    {
        free(block[i]);
    }

    delete[] block;

    zfex_status_code_t rc = fec_free(fec_p);
    assert(rc == ZFEX_SC_OK);

    block = NULL;
    fec_p = NULL;
    fec_k = -1;
    fec_n = -1;
}

void Transmitter::init_session(int k, int n)
{
    if (fec_p != NULL)
    {
        deinit_session();
    }

    assert(fec_p == NULL);
    assert(k >= 1);
    assert(n >= 1);
    assert(n < 256);
    assert(k <= n);

    fec_k = k;
    fec_n = n;

    zfex_status_code_t rc = fec_new(fec_k, fec_n, &fec_p);
    assert(rc == ZFEX_SC_OK);

    block = new uint8_t*[fec_n];
    for(int i=0; i < fec_n; i++)
    {
        int _rc = posix_memalign((void**)&block[i], ZFEX_SIMD_ALIGNMENT, ZFEX_ROUND_UP_SIMD(MAX_FEC_PAYLOAD));
        assert(_rc == 0);
    }

    block_idx = 0;
    fragment_idx = 0;

    // init session key
    randombytes_buf(session_key, sizeof(session_key));

    // fill packet header
    wsession_hdr_t *session_hdr = (wsession_hdr_t *)session_packet;
    session_hdr->packet_type = WFB_PACKET_SESSION;

    randombytes_buf(session_hdr->session_nonce, sizeof(session_hdr->session_nonce));

    // fill packet contents

    uint8_t tmp[MAX_SESSION_PACKET_SIZE - crypto_box_MACBYTES - sizeof(wsession_hdr_t)];

    // Fill fixed headers
    {
        wsession_data_t* session_data = (wsession_data_t*)tmp;
        assert(sizeof(*session_data) <= sizeof(tmp));

        session_data->epoch = htobe64(epoch);
        session_data->channel_id = htobe32(channel_id);
        session_data->fec_type = WFB_FEC_VDM_RS;
        session_data->k = (uint8_t)fec_k;
        session_data->n = (uint8_t)fec_n;

        assert(sizeof(session_data->session_key) == sizeof(session_key));
        memcpy(session_data->session_key, session_key, sizeof(session_key));
    }

    // Fill optional Tags

    uint32_t session_data_size = sizeof(wsession_data_t);
    for(auto it = tags.begin(); it != tags.end(); it++)
    {
        tlv_hdr_t* tlv = (tlv_hdr_t*)((uint8_t*)tmp + session_data_size);
        session_data_size += sizeof(tlv_hdr_t) + it->value.size();
        assert(session_data_size <= sizeof(tmp));

        tlv->id = it->id;
        tlv->len = it->value.size();
        memcpy(tlv->value, &it->value[0], it->value.size());
    }

    if (crypto_box_easy(session_packet + sizeof(wsession_hdr_t),
                        (uint8_t*)tmp, session_data_size,
                        session_hdr->session_nonce, rx_publickey, tx_secretkey) != 0)
    {
        throw runtime_error("Unable to make session key!");
    }

    session_packet_size = sizeof(wsession_hdr_t) + session_data_size + crypto_box_MACBYTES;
    assert(session_packet_size <= MAX_SESSION_PACKET_SIZE);
}


RawSocketTransmitter::RawSocketTransmitter(int k, int n, const string &keypair, uint64_t epoch, uint32_t channel_id, uint32_t fec_delay,
                                           vector<tags_item_t> &tags, const vector<string> &wlans, radiotap_header_t &radiotap_header,
                                           uint8_t frame_type, bool use_qdisc, uint32_t fwmark_base, uint32_t inject_retries, uint32_t inject_retry_delay) : \
    Transmitter(k, n, keypair, epoch, channel_id, fec_delay, tags),
    channel_id(channel_id),
    current_output(0),
    ieee80211_seq(0),
    radiotap_header(radiotap_header),
    frame_type(frame_type),
    use_qdisc(use_qdisc),
    fwmark_base(fwmark_base),
    fwmark(fwmark_base),
    inject_retries(inject_retries),
    inject_retry_delay(inject_retry_delay)
{
    for(auto it=wlans.begin(); it!=wlans.end(); it++)
    {
        int fd = socket(PF_PACKET, SOCK_RAW, 0);
        if (fd < 0)
        {
            throw runtime_error(string_format("Unable to open PF_PACKET socket: %s", strerror(errno)));
        }

        if(!use_qdisc)
        {
            const int optval = 1;
            if(setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, (const void *)&optval , sizeof(optval)) !=0)
            {
                close(fd);
                throw runtime_error(string_format("Unable to set PACKET_QDISC_BYPASS: %s", strerror(errno)));
            }
        }

        struct ifreq ifr;
        memset(&ifr, '\0', sizeof(ifr));
        strncpy(ifr.ifr_name, it->c_str(), sizeof(ifr.ifr_name) - 1);

        if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
        {
            close(fd);
            throw runtime_error(string_format("Unable to get interface index for %s: %s", it->c_str(), strerror(errno)));
        }

        struct sockaddr_ll sll;
        memset(&sll, '\0', sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = ifr.ifr_ifindex;
        sll.sll_protocol = 0;

        if (::bind(fd, (struct sockaddr *) &sll, sizeof(sll)) < 0)
        {
            close(fd);
            throw runtime_error(string_format("Unable to bind to %s: %s", it->c_str(), strerror(errno)));
        }

        sockfds.push_back(fd);
        fd_fwmarks[fd] = 0;
    }
}

void RawSocketTransmitter::inject_packet(const uint8_t *buf, size_t size)
{
    assert(size <= MAX_FORWARDER_PACKET_SIZE);
    uint8_t ieee_hdr[sizeof(ieee80211_header)];

    // fill default values
    memcpy(ieee_hdr, ieee80211_header, sizeof(ieee80211_header));

    // frame_type
    ieee_hdr[0] = frame_type;

    // channel_id
    uint32_t channel_id_be = htobe32(channel_id);
    memcpy(ieee_hdr + SRC_MAC_THIRD_BYTE, &channel_id_be, sizeof(uint32_t));
    memcpy(ieee_hdr + DST_MAC_THIRD_BYTE, &channel_id_be, sizeof(uint32_t));

    // sequence number
    ieee_hdr[FRAME_SEQ_LB] = ieee80211_seq & 0xff;
    ieee_hdr[FRAME_SEQ_HB] = (ieee80211_seq >> 8) & 0xff;
    ieee80211_seq += 16;

    struct iovec iov[3] = \
        {
            // radiotap header
            { .iov_base = (void*)&radiotap_header.header[0],
              .iov_len = radiotap_header.header.size()
            },
            // ieee80211 header
            { .iov_base = (void*)ieee_hdr,
              .iov_len = sizeof(ieee_hdr)
            },
            // packet payload
            { .iov_base = (void*)buf,
              .iov_len = size
            }
        };

    struct msghdr msghdr = \
        { .msg_name = NULL,
          .msg_namelen = 0,
          .msg_iov = iov,
          .msg_iovlen = 3,
          .msg_control = NULL,
          .msg_controllen = 0,
          .msg_flags = 0};

    if (current_output >= 0)
    {
        // Normal mode - only one card do packet transmission in a time
        uint64_t start_us = get_time_us();
        int fd = sockfds[current_output];

        if (use_qdisc && fd_fwmarks[fd] != fwmark)
        {
            uint32_t sockopt = fwmark;

            if(setsockopt(fd, SOL_SOCKET, SO_MARK, (const void *)&sockopt , sizeof(sockopt)) !=0)
            {
                throw runtime_error(string_format("Unable to set SO_MARK fd(%d)=%u: %s", fd, sockopt, strerror(errno)));
            }

            fd_fwmarks[fd] = fwmark;
        }

        int rc = -1;
        for(uint32_t i=0; rc < 0 && i <= inject_retries; i++)
        {
            if (i > 0)
            {
                struct timespec t = {
                    .tv_sec = (time_t)(inject_retry_delay / 1000000),
                    .tv_nsec = (suseconds_t)(inject_retry_delay % 1000000) * 1000
                };

                int rc2 = clock_nanosleep(CLOCK_MONOTONIC, 0, &t, NULL);

                if (rc2 != 0 && rc2 != EINTR)
                {
                    throw runtime_error(string_format("clock_nanosleep: %s", strerror(rc2)));
                }
            }

            rc = sendmsg(fd, &msghdr, 0);

            if (rc < 0 && errno != ENOBUFS)
            {
                throw runtime_error(string_format("Unable to inject packet: %s", strerror(errno)));
            }
        }

        uint64_t end_us = get_time_us();
        uint64_t key = (uint64_t)(current_output) << 8 | (uint64_t)0xff;
        antenna_stat[key].log_latency(end_us - start_us, rc >= 0, size);
        inject_us_acc += end_us - start_us;
    }
    else
    {
        // Mirror mode - transmit packet via all cards
        // Use only for different frequency channels

        vector<int> rc_vec;
        vector<uint64_t> lat_vec;
        int socks_pending = 0;

        for(auto it=sockfds.begin(); it != sockfds.end(); it++)
        {
            rc_vec.push_back(-1);
            lat_vec.push_back(0);
            socks_pending += 1;
        }

        for(uint32_t i=0; i <= inject_retries && socks_pending > 0; i++)
        {
            if (i > 0)
            {
                struct timespec t = {
                    .tv_sec = (time_t)(inject_retry_delay / 1000000),
                    .tv_nsec = (suseconds_t)(inject_retry_delay % 1000000) * 1000
                };

                int rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &t, NULL);

                if(rc != 0 && rc != EINTR)
                {
                    throw runtime_error(string_format("clock_nanosleep: %s", strerror(rc)));
                }
            }

            int sock_idx = 0;
            for(auto it=sockfds.begin(); it != sockfds.end() && socks_pending > 0; it++, sock_idx++)
            {
                // skip cards that already sent packet
                if (rc_vec[sock_idx] >= 0) continue;

                uint64_t start_us = get_time_us();
                int fd = *it;

                if (use_qdisc && fd_fwmarks[fd] != fwmark)
                {
                    uint32_t sockopt = fwmark;

                    if(setsockopt(fd, SOL_SOCKET, SO_MARK, (const void *)&sockopt , sizeof(sockopt)) != 0)
                    {
                        throw runtime_error(string_format("Unable to set SO_MARK fd(%d)=%u: %s", fd, sockopt, strerror(errno)));
                    }

                    fd_fwmarks[fd] = fwmark;
                }

                rc_vec[sock_idx] = sendmsg(fd, &msghdr, 0);

                if (rc_vec[sock_idx] < 0 && errno != ENOBUFS)
                {
                    throw runtime_error(string_format("Unable to inject packet: %s", strerror(errno)));
                }

                if (rc_vec[sock_idx] >= 0)
                {
                    socks_pending -= 1;
                }

                lat_vec[sock_idx] += (get_time_us() - start_us);

                // log success transmission or if no more retries available
                if (rc_vec[sock_idx] >= 0 || i == inject_retries)
                {
                    uint64_t key = (uint64_t)(sock_idx) << 8 | (uint64_t)0xff;
                    antenna_stat[key].log_latency(lat_vec[sock_idx] + i * inject_retry_delay,
                                                  rc_vec[sock_idx] >= 0,
                                                  size);
                }
            }
        }
    }

}

void RawSocketTransmitter::dump_stats(uint64_t ts, uint32_t &injected_packets, uint32_t &dropped_packets, uint32_t &injected_bytes)
{
    for(auto it = antenna_stat.begin(); it != antenna_stat.end(); it++)
    {
        IPC_MSG("%" PRIu64 "\tTX_ANT\t%" PRIx64 "\t%u:%u:%" PRIu64 ":%" PRIu64 ":%" PRIu64 "\n",
                ts, it->first,
                it->second.count_p_injected, it->second.count_p_dropped,
                it->second.latency_min,
                it->second.latency_sum / (it->second.count_p_injected + it->second.count_p_dropped),
                it->second.latency_max);

        injected_packets += it->second.count_p_injected;
        dropped_packets += it->second.count_p_dropped;
        injected_bytes += it->second.count_b_injected;
    }
    antenna_stat.clear();
}

RawSocketTransmitter::~RawSocketTransmitter()
{
    for(auto it=sockfds.begin(); it != sockfds.end(); it++)
    {
        close(*it);
    }
}


RemoteTransmitter::RemoteTransmitter(int k, int n, const string &keypair, uint64_t epoch, uint32_t channel_id, uint32_t fec_delay,
                                     vector<tags_item_t> &tags, const vector<pair<string, vector<uint16_t>>> &remote_hosts, radiotap_header_t &radiotap_header,
                                     uint8_t frame_type, bool use_qdisc, uint32_t fwmark_base, int snd_buf_size) : \
    Transmitter(k, n, keypair, epoch, channel_id, fec_delay, tags),
    channel_id(channel_id),
    current_output(0),
    ieee80211_seq(0),
    radiotap_header(radiotap_header),
    frame_type(frame_type),
    use_qdisc(use_qdisc),
    fwmark_base(fwmark_base),
    fwmark(fwmark_base)
{

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) throw std::runtime_error(string_format("Error opening socket: %s", strerror(errno)));

    if (snd_buf_size > 0)
    {
        if(setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const void *)&snd_buf_size , sizeof(snd_buf_size)) !=0)
        {
            close(sockfd);
            throw runtime_error(string_format("Unable to set SO_SNDBUF: %s", strerror(errno)));
        }
    }

    int output = 0;
    for(auto h_it=remote_hosts.begin(); h_it!=remote_hosts.end(); h_it++)
    {
        uint8_t wlan_id = 0;
        for(auto p_it=h_it->second.begin(); p_it != h_it->second.end(); p_it++, output++, wlan_id++)
        {
            struct sockaddr_in saddr;
            memset(&saddr, '\0', sizeof(saddr));
            saddr.sin_family = AF_INET;
            saddr.sin_addr.s_addr = inet_addr(h_it->first.c_str());
            saddr.sin_port = htons((unsigned short)*p_it);
            sockaddrs.push_back(saddr);
            output_to_ant_id[output] = ((uint64_t)ntohl(saddr.sin_addr.s_addr) << 32) | (uint64_t)(wlan_id) << 8 | (uint64_t)0xff;
        }
    }
}

void RemoteTransmitter::inject_packet(const uint8_t *buf, size_t size)
{
    assert(size <= MAX_FORWARDER_PACKET_SIZE);
    uint8_t ieee_hdr[sizeof(ieee80211_header)];

    // fill default values
    memcpy(ieee_hdr, ieee80211_header, sizeof(ieee80211_header));

    // frame_type
    ieee_hdr[0] = frame_type;

    // channel_id
    uint32_t channel_id_be = htobe32(channel_id);
    memcpy(ieee_hdr + SRC_MAC_THIRD_BYTE, &channel_id_be, sizeof(uint32_t));
    memcpy(ieee_hdr + DST_MAC_THIRD_BYTE, &channel_id_be, sizeof(uint32_t));

    // sequence number
    ieee_hdr[FRAME_SEQ_LB] = ieee80211_seq & 0xff;
    ieee_hdr[FRAME_SEQ_HB] = (ieee80211_seq >> 8) & 0xff;
    ieee80211_seq += 16;

    uint32_t _fwmark = use_qdisc ? htonl(this->fwmark) : 0;

    struct iovec iov[4] = \
        {
            // fwmark
            {
                .iov_base = (void*)&_fwmark,
                .iov_len = sizeof(_fwmark),
            },
            // radiotap header
            { .iov_base = (void*)&radiotap_header.header[0],
              .iov_len = radiotap_header.header.size()
            },
            // ieee80211 header
            { .iov_base = (void*)ieee_hdr,
              .iov_len = sizeof(ieee_hdr)
            },
            // packet payload
            { .iov_base = (void*)buf,
              .iov_len = size
            }
        };

    struct msghdr msghdr = \
        { .msg_name = NULL,
          .msg_namelen = 0,
          .msg_iov = iov,
          .msg_iovlen = 4,
          .msg_control = NULL,
          .msg_controllen = 0,
          .msg_flags = 0};

    struct sockaddr_in saddr;

    if (current_output >= 0)
    {
        // Normal mode - only one card do packet transmission in a time
        uint64_t start_us = get_time_us();

        saddr = sockaddrs[current_output];
        msghdr.msg_name = &saddr;
        msghdr.msg_namelen = sizeof(saddr);

        int rc = sendmsg(sockfd, &msghdr, 0);

        if (rc < 0 && errno != ENOBUFS)
        {
            throw runtime_error(string_format("Unable to inject packet: %s", strerror(errno)));
        }

        uint64_t key = output_to_ant_id[current_output];
        antenna_stat[key].log_latency(get_time_us() - start_us, rc >= 0, size);
    }
    else
    {
        // Mirror mode - transmit packet via all cards
        // Use only for different frequency channels
        int i = 0;
        for(auto it=sockaddrs.begin(); it != sockaddrs.end(); it++, i++)
        {
            uint64_t start_us = get_time_us();

            saddr = *it;
            msghdr.msg_name = &saddr;
            msghdr.msg_namelen = sizeof(saddr);

            int rc = sendmsg(sockfd, &msghdr, 0);

            if (rc < 0 && errno != ENOBUFS)
            {
                throw runtime_error(string_format("Unable to inject packet: %s", strerror(errno)));
            }

            uint64_t key = output_to_ant_id[i];
            antenna_stat[key].log_latency(get_time_us() - start_us, rc >= 0, size);
        }
    }

}

void RemoteTransmitter::dump_stats(uint64_t ts, uint32_t &injected_packets, uint32_t &dropped_packets, uint32_t &injected_bytes)
{
    for(auto it = antenna_stat.begin(); it != antenna_stat.end(); it++)
    {
        IPC_MSG("%" PRIu64 "\tTX_ANT\t%" PRIx64 "\t%u:%u:%" PRIu64 ":%" PRIu64 ":%" PRIu64 "\n",
                ts, it->first,
                it->second.count_p_injected, it->second.count_p_dropped,
                it->second.latency_min,
                it->second.latency_sum / (it->second.count_p_injected + it->second.count_p_dropped),
                it->second.latency_max);

        injected_packets += it->second.count_p_injected;
        dropped_packets += it->second.count_p_dropped;
        injected_bytes += it->second.count_b_injected;
    }
    antenna_stat.clear();
}



void Transmitter::send_block_fragment(size_t packet_size)
{
    uint8_t ciphertext[MAX_FORWARDER_PACKET_SIZE];
    wblock_hdr_t *block_hdr = (wblock_hdr_t*)ciphertext;
    long long unsigned int ciphertext_len;

    assert(packet_size <= MAX_FEC_PAYLOAD);

    block_hdr->packet_type = WFB_PACKET_DATA;
    block_hdr->data_nonce = htobe64(((block_idx & BLOCK_IDX_MASK) << 8) + fragment_idx);

    // encrypted payload
    {
        uint64_t t0 = get_time_us();
        if (crypto_aead_chacha20poly1305_encrypt(ciphertext + sizeof(wblock_hdr_t), &ciphertext_len,
                                                 block[fragment_idx], packet_size,
                                                 (uint8_t*)block_hdr, sizeof(wblock_hdr_t),
                                                 NULL, (uint8_t*)(&(block_hdr->data_nonce)), session_key) < 0)
        {
            throw runtime_error("Unable to encrypt packet!");
        }
        encrypt_us_acc += get_time_us() - t0;
    }

    inject_packet(ciphertext, sizeof(wblock_hdr_t) + ciphertext_len);
}

void Transmitter::send_session_key(void)
{
    WFB_DBG("Announce session key\n");
    inject_packet((uint8_t*)session_packet, session_packet_size);
}

bool Transmitter::send_packet(const uint8_t *buf, size_t size, uint8_t flags)
{
    assert(size <= MAX_PAYLOAD_SIZE);

    // FEC-only packets are only for closing already opened blocks
    if (fragment_idx == 0 && (flags & WFB_PACKET_FEC_ONLY))
    {
        return false;
    }

    wpacket_hdr_t *packet_hdr = (wpacket_hdr_t*)block[fragment_idx];

    packet_hdr->flags = flags;
    packet_hdr->packet_size = htobe16(size);

    if(size > 0)
    {
        assert(buf != NULL);
        memcpy(block[fragment_idx] + sizeof(wpacket_hdr_t), buf, size);
    }

    memset(block[fragment_idx] + sizeof(wpacket_hdr_t) + size, '\0', MAX_FEC_PAYLOAD - (sizeof(wpacket_hdr_t) + size));

    // mark data packets with fwmark
    if(fragment_idx == 0)
    {
        set_mark(0);
    }

    send_block_fragment(sizeof(wpacket_hdr_t) + size);
    max_packet_size = max(max_packet_size, sizeof(wpacket_hdr_t) + size);
    fragment_idx += 1;

    if (fragment_idx < fec_k)  return true;

    {
        uint64_t t0 = get_time_us();
        zfex_status_code_t _rc = fec_encode_simd(fec_p, (const uint8_t**)block, block + fec_k, ZFEX_ROUND_UP_SIMD(max_packet_size));
        fec_us_acc += get_time_us() - t0;
        assert(_rc == ZFEX_SC_OK);
    }

    // mark fec packets with fwmark + 1
    set_mark(1);

    while (fragment_idx < fec_n)
    {
        if(fec_delay > 0)
        {
            struct timespec t = { .tv_sec = (time_t)(fec_delay / 1000000),
                                  .tv_nsec = (suseconds_t)(fec_delay % 1000000) * 1000 };

            int rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &t, NULL);

            if(rc != 0 && rc != EINTR)
            {
                throw runtime_error(string_format("clock_nanosleep: %s", strerror(rc)));
            }
        }

        send_block_fragment(max_packet_size);
        fragment_idx += 1;
    }
    block_idx += 1;
    fragment_idx = 0;
    max_packet_size = 0;

    // Generate new session key after MAX_BLOCK_IDX blocks
    if (block_idx > MAX_BLOCK_IDX)
    {
        init_session(fec_k, fec_n);
        for(int i = 0; i < fec_n - fec_k + 1; i++)
        {
            send_session_key();
        }
    }

    return true;
}

void Transmitter::check_metrics(uint64_t now_us)
{
    if (metrics_window_start_us == 0) {
        metrics_window_start_us = now_us;
        return;
    }
    if (now_us - metrics_window_start_us < 1000000ULL)
        return;

    double avg_ms = delay_count > 0 ? (delay_us_sum / (double)delay_count) / 1000.0 : 0.0;
    printf("wfb_tx: inject=%ums enc=%ums fec=%ums avg=%.1fms max=%.1fms grp10max=%.1fms /s\n",
           (unsigned)(inject_us_acc / 1000),
           (unsigned)(encrypt_us_acc / 1000),
           (unsigned)(fec_us_acc / 1000),
           avg_ms,
           delay_max_us / 1000.0,
           group10_max_us / 1000.0);
    inject_us_acc = 0;
    encrypt_us_acc = 0;
    fec_us_acc = 0;
    delay_us_sum = 0;
    delay_count = 0;
    delay_max_us = 0;
    group10_max_us = 0;
    metrics_window_start_us = now_us;
}

// Extract SO_RXQ_OVFL counter
uint32_t extract_rxq_overflow(struct msghdr *msg)
{
    struct cmsghdr *cmsg;
    uint32_t rtn;

    for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL) {
            memcpy(&rtn, CMSG_DATA(cmsg), sizeof(rtn));
            return rtn;
        }
    }
    return 0;
}

radiotap_header_t init_radiotap_header(uint8_t stbc,
                                       bool ldpc,
                                       bool short_gi,
                                       uint8_t bandwidth,
                                       uint8_t mcs_index,
                                       bool vht_mode,
                                       uint8_t vht_nss)
{
    radiotap_header_t res = {
        .header = {},
        .stbc = stbc,
        .ldpc = ldpc,
        .short_gi = short_gi,
        .bandwidth = bandwidth,
        .mcs_index = mcs_index,
        .vht_mode = vht_mode,
        .vht_nss = vht_nss,
    };

    if (!vht_mode)
    {
        // Set flags in HT radiotap header
        uint8_t flags = 0;

        switch(bandwidth)
        {
        case 10:
        case 20:
            flags |= IEEE80211_RADIOTAP_MCS_BW_20;
            break;
        case 40:
            flags |= IEEE80211_RADIOTAP_MCS_BW_40;
            break;
        default:
            throw runtime_error(string_format("Unsupported HT bandwidth: %d", bandwidth));
        }

        if (short_gi)
        {
            flags |= IEEE80211_RADIOTAP_MCS_SGI;
        }

        switch(stbc)
        {
        case 0:
            break;
        case 1:
            flags |= (IEEE80211_RADIOTAP_MCS_STBC_1 << IEEE80211_RADIOTAP_MCS_STBC_SHIFT);
            break;
        case 2:
            flags |= (IEEE80211_RADIOTAP_MCS_STBC_2 << IEEE80211_RADIOTAP_MCS_STBC_SHIFT);
            break;
        case 3:
            flags |= (IEEE80211_RADIOTAP_MCS_STBC_3 << IEEE80211_RADIOTAP_MCS_STBC_SHIFT);
            break;
        default:
            throw runtime_error(string_format("Unsupported HT STBC type: %d", stbc));
        }

        if (ldpc)
        {
            flags |= IEEE80211_RADIOTAP_MCS_FEC_LDPC;
        }

        copy(radiotap_header_ht, radiotap_header_ht + sizeof(radiotap_header_ht), back_inserter(res.header));

        res.header[MCS_FLAGS_OFF] = flags;
        res.header[MCS_IDX_OFF] = mcs_index;
    }
    else
    {
        // Set flags in VHT radiotap header
        uint8_t flags = 0;

        copy(radiotap_header_vht, radiotap_header_vht + sizeof(radiotap_header_vht), back_inserter(res.header));

        if (short_gi)
        {
            flags |= IEEE80211_RADIOTAP_VHT_FLAG_SGI;
        }

        if (stbc)
        {
            flags |= IEEE80211_RADIOTAP_VHT_FLAG_STBC;
        }

        switch(bandwidth)
        {
        case 10:
        case 20:
            res.header[VHT_BW_OFF] = IEEE80211_RADIOTAP_VHT_BW_20M;
            break;
        case 40:
            res.header[VHT_BW_OFF] = IEEE80211_RADIOTAP_VHT_BW_40M;
            break;
        case 80:
            res.header[VHT_BW_OFF] = IEEE80211_RADIOTAP_VHT_BW_80M;
            break;
        case 160:
            res.header[VHT_BW_OFF] = IEEE80211_RADIOTAP_VHT_BW_160M;
            break;
        default:
            throw runtime_error(string_format("Unsupported VHT bandwidth: %d", bandwidth));
        }

        if (ldpc)
        {
            res.header[VHT_CODING_OFF] = IEEE80211_RADIOTAP_VHT_CODING_LDPC_USER0;
        }

        res.header[VHT_FLAGS_OFF] = flags;
        res.header[VHT_MCSNSS0_OFF] |= ((mcs_index << IEEE80211_RADIOTAP_VHT_MCS_SHIFT) & IEEE80211_RADIOTAP_VHT_MCS_MASK);
        res.header[VHT_MCSNSS0_OFF] |= ((vht_nss << IEEE80211_RADIOTAP_VHT_NSS_SHIFT) & IEEE80211_RADIOTAP_VHT_NSS_MASK);
    }

    return res;
}
