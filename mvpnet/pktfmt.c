/*
 * Copyright (c) 2025, Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * pktfmt.c  ethernet packet formats for mvpnet
 * 06-May-2025  chuck@ece.cmu.edu
 */

#include <string.h>

#include "pktfmt.h"

#define IP_ADDRSZ   4                    /* size of IPv4 address */

/* ethernet broadcast address */
static uint8_t ether_bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/*
 * ARP protocol
 */
static uint8_t ether_proto_arp[2] = { 0x08, 0x06 };  /* protocol number */

/* fixed headers at front of arp ethernet/ipv4 request/reply */
static uint8_t arp_req_hdr[8] = {
    0x00, 0x01,     /* ethernet */
    0x08, 0x00,     /* IPv4 */
    0x06, 0x04,     /* hardware addr len==6, protocol addr len==4 */
    0x00, 0x01      /* arp 'request' operation */
};

static uint8_t arp_rep_hdr[8] = {
    0x00, 0x01,     /* ethernet */
    0x08, 0x00,     /* IPv4 */
    0x06, 0x04,     /* hardware addr len==6, protocol addr len==4 */
    0x00, 0x02      /* arp 'reply' operation */
};

#define ARP_HDR_SIZE    sizeof(arp_req_hdr)  /* req and rep are same size */

/* arp offsets, from start of ethernet frame */
#define ARP_HDR_OFF        ETH_DATAOFF
#define ARP_SHW_OFF        (ARP_HDR_OFF+ARP_HDR_SIZE)  /* sender hw addr */
#define ARP_SIP_OFF        (ARP_SHW_OFF+ETH_ADDRSIZE)  /* sender ip addr */
#define ARP_THW_OFF        (ARP_SIP_OFF+IP_ADDRSZ)     /* target hw addr */
#define ARP_TIP_OFF        (ARP_THW_OFF+ETH_ADDRSIZE)  /* target ip addr */

#define ARP_SIZE           (ARP_TIP_OFF+IP_ADDRSZ)     /* incl ether header */

/*
 * check if the frame is a valid broadcast ARP request packet.
 * return 0 if so, otherwise return -1.   we return the MPI
 * rank being queried in qrankp.  the caller should validate
 * this value to determine if it should respond to the ARP request.
 *
 * note that the encoded rank in the IP address is offset by
 * 1 to avoid using IP address 10.0.0.0 for rank 0 (some older
 * kernels are hardwired to treat 10.0.0.0 as a broadcast address...
 * this prevents it from being used as a normal address).
 * Thus, rank 0 is 10.0.0.1, rank 1 is 10.0.0.2, etc.
 */
int pktfmt_arp_req_qrank(uint8_t *ef, int efsz, int *qrankp) {
    int oqrank;

    /* must be an ethernet broadcast */
    if (memcmp(&ef[ETH_DSTOFF], ether_bcast, sizeof(ether_bcast)) != 0)
        return(-1);

    /* must be an ARP frame that is large enough */
    if (memcmp(&ef[ETH_TYPEOFF], ether_proto_arp,
               sizeof(ether_proto_arp)) != 0 ||  efsz < ARP_SIZE)
        return(-1);

    /* must have have fixed header */
    if (memcmp(&ef[ARP_HDR_OFF], arp_req_hdr, ARP_HDR_SIZE) != 0)
        return(-1);

    /* offset rank is the lower 3 bytes of IP address we are asking about */
    oqrank = (ef[ARP_TIP_OFF+1] << 16) |
             (ef[ARP_TIP_OFF+2] << 8)  |  ef[ARP_TIP_OFF+3];

    *qrankp = oqrank - 1;   /* remove offset to return MPI rank */
    return(0);
}

/*
 * generate ARP reply message from ARP request message.  caller
 * must ensure req and rep are the correct size.
 */
void pktfmt_arp_mkreply(uint8_t *req, int qrank, uint8_t *rep) {

    /*
     * set up ethernet frame header.  reply dst is req source.
     * generate reply src: copy first 2 bytes from req src and
     * generate the lower 4 bytes from qrank.
     */
    memcpy(&rep[ETH_DSTOFF], &req[ETH_SRCOFF], ETH_ADDRSIZE);
    memcpy(&rep[ETH_SRCOFF], &req[ETH_SRCOFF], 2);
    rep[ETH_SRCOFF+2] = (qrank >> 24) & 0xff;
    rep[ETH_SRCOFF+3] = (qrank >> 16) & 0xff;
    rep[ETH_SRCOFF+4] = (qrank >>  8) & 0xff;
    rep[ETH_SRCOFF+5] = qrank & 0xff;
    memcpy(&rep[ETH_TYPEOFF], &req[ETH_TYPEOFF], ETH_TYPESIZE);  /* ARP */

    /*
     * copy in arp headers.  the fixed part is just arp_rep_hdr.
     * the sender hw address is the same as the ethernet source
     * hardware address.  the sender ip address is the target ip
     * from the query.  the reply target hw and ip addresses are
     * copied from the request sender hw/ip.
     */
    memcpy(&rep[ARP_HDR_OFF], arp_rep_hdr, ARP_HDR_SIZE);
    memcpy(&rep[ARP_SHW_OFF], &rep[ETH_SRCOFF], ETH_ADDRSIZE);
    memcpy(&rep[ARP_SIP_OFF], &req[ARP_TIP_OFF], IP_ADDRSZ);
    memcpy(&rep[ARP_THW_OFF], &req[ARP_SHW_OFF], ETH_ADDRSIZE);
    memcpy(&rep[ARP_TIP_OFF], &req[ARP_SIP_OFF], IP_ADDRSZ);
}

/*
 * shutdown broadcast packet
 */
static uint8_t shutdown_pkt[PKTFMT_SHUTDOWN_LEN] = {
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff,    /* ETH_DST: broadcast */
     0x52, 0x55, 0x00, 0x00, 0x00, 0x00,    /* ETH_SRC: rank 0 */
     0x08, 0x88,                            /* ETH_TYPE: reuse old xyplex# */
     'm', 'v', 'p', 'n', 'e', 't', 0        /* magic string, zero fill rest */
};

/*
 * is the given frame the shutdown packet?   ret 1 if true, 0 ow.
 */
int pktfmt_is_shutdown(uint8_t *ef, int efsz) {
    if (efsz != PKTFMT_SHUTDOWN_LEN ||
        memcmp(ef, shutdown_pkt, PKTFMT_SHUTDOWN_LEN) != 0)
        return(0);
    return(1);
}

/*
 * load shutdown packet into frame.   caller must ensure ef is
 * the correct size (PKTFMT_SHUTDOWN_LEN).
 */
void pktfmt_load_shutdown(uint8_t *ef) {
    memcpy(ef, shutdown_pkt, PKTFMT_SHUTDOWN_LEN);
}
