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
 * pktfmt.h  ethernet packet formats for mvpnet
 * 06-May-2025  chuck@ece.cmu.edu
 */

#ifndef MVP_PKTFMT_H
#define MVP_PKTFMT_H

#include <inttypes.h>

#define ETH_ADDRSIZE 6
#define ETH_TYPESIZE 2
#define ETH_HDRSIZE  (ETH_ADDRSIZE+ETH_ADDRSIZE+ETH_TYPESIZE) /* dst src type */

#define ETH_DSTOFF   0                   /* dst ether addr */
#define ETH_SRCOFF   ETH_ADDRSIZE        /* src ether addr */
#define ETH_TYPEOFF  (ETH_ADDRSIZE*2)    /* ethernet frame type offset */
#define ETH_DATAOFF  (ETH_TYPEOFF+2)     /* data offset */

int pktfmt_arp_req_qrank(uint8_t *ef, int efsz);
void pktfmt_arp_mkreply(uint8_t *req, int qrank, uint8_t *rep);

#endif /* MVP_PKTFMT_H */
