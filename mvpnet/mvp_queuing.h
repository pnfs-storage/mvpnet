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
 * mvp_queuing.h  packet data buffering and queue management for mvpnet
 * 01-May-2025  chuck@ece.cmu.edu
 */
#ifndef MVP_QUEUING_H
#define MVP_QUEUING_H

#include <inttypes.h>
#include <pthread.h>

#include <sys/queue.h>

#include "fbufmgr.h"

/*
 * we consolidate all the data buffer and queue management required
 * by mvpnet here.   data flows as follows:
 *
 *  qem           |          fdio thread         |       MPI thread
 *  w/wrapper     |                              |
 * +---------+         +-----------+         -----------+
 * |         | ------->|  fbufmgr  |-------->  send q   |-------> MPI_Isend
 * |  guest  |         |-----------+    \    -----------+
 * |         | <----+   main or bcast    \
 * +---------+       \                    -> +-----------         MPI_Iprobe
 *              unix  -----------------------| recv q    <----+-- MPI_Recv
 *              domain                       +-----------     |
 *              socket(s)                                  +---------------+
 *                                                         | recv buf pool |
 *                                                         +---------------+
 *
 * so we need the following structures:
 *  - the main fbufmgr (fdio thread reads data from qemu sockets here)
 *  - the bcast fbufmgr (fdio thread replicates broacast frames here)
 *  - MPI send q (fdio thread queues frames for MPI thread to send here)
 *  - MPI recv q (MPI thread queues frames for fdio thread to recv here)
 *
 * locking:
 *
 *  - unix domain socket fds: only accessed by the fdio thread, so
 *             no locking required for I/O on these descriptors.
 *             but we do want to ensure that the fdio thread does not
 *             block in the kernel during these I/Os, so we'll use
 *             poll() and O_NONBLOCK to ensure this.
 *
 *  - fbufmgr: only the fdio thread can add data to the fbufmgrs, so
 *             no locking is required for that.  the fdio thread loans
 *             fbufs with complete frames to the MPI thread, so the
 *             loan counter and fbuf loaned/free lists use the fbufmgr
 *             lock to serialize access.  (MPI thread will drop the
 *             loan for a send when the async Isend completes.)
 *
 *  - send q/recv q: both the fdio thread and the MPI thread access
 *             the queues, so we will provide a queue lock to serialize
 *             access.
 *
 *  - recv buf pool: MPI thread allocates, passes to fdio thread
 *             (via recv q), and the fdio thread will free when done.
 *             we will use queue lock to serialize access to the
 *             recv buf pool.
 *
 * notifications:
 *  - unix domain sockets: fdio thread will poll() the sockets for I/O state
 *  - send q: polled by MPI thread (with actual polling, not poll(2)).
 *  - recv q: fdio thread notified (by pipe) when MPI adds new packets
 */

/*
 * config.  we want the min free space in an fbuf to be a bit more
 * than the largest ethernet MTU (allow for jumbo frames).
 */
#define MVP_FBUFMIN      16384              /* min free space in an fbuf */
#define MVP_FBUFSIZE     (MVP_FBUFMIN*32)   /* fbuf size */

#define MVP_BPOOLMIN     64                 /* smallest recv buf alloc */
#define MVP_BPOOLMAX     MVP_FBUFMIN        /* largest recv buf alloc */

/*
 * stats collected by mvp queuing (except for bininfo)
 */
struct que_stats {
    struct fbuf_stats fbsmain;              /* main fbufmgr */
    struct fbuf_stats fbsbcast;             /* broadcast fbufmgr */
    int nsqe;                               /* # send queue entries */
    int mpimaxsqlen;                        /* max size sendq has grown to */
    int nrqe;                               /* number of rqes allocated */
    int mpimaxrqlen;                        /* max size recvq has grown to */
};

/*
 * MPI send queue: producer=fdio thread, consumer=MPI thread
 */

/* send queue entry */
struct sendq_entry {
    int dest;                     /* MPI rank to send to */ 
    char *frame;                  /* frame data to send */
    int flen;                     /* length of the frame buffer */
    struct fbuf *owner;           /* fbuf that has loaned us the buffer */
    TAILQ_ENTRY(sendq_entry) sq;  /* linkage */
};

/* sendqlist is a list of sendq_entry structs */
TAILQ_HEAD(sendqlist, sendq_entry);

/*
 * MPI recv queue: producer=MPI thread, consumer=fdio thread
 *
 *   special case: fdio can directly queue data for guest (e.g. ARP reply).
 *                 it will use bcast fbufmgr buffer space in this case.
 *
 * bins: we bin recv queue entries by buffer size.   bin #0 is reserved
 * for recv bufs whose buffer has been loaned to us from a fbufmgr.
 */
struct recvq_entry {
    char *frame;                  /* frame data received */
    int flen;                     /* lenght of valid data in frame buffer */
    int bin;                      /* buf pool bin we belong to */
    struct fbuf *owner;           /* bin==0: fbuf that loan us the buffer */
                                  /* bin!=0: ignored (should be NULL) */
    TAILQ_ENTRY(recvq_entry) rq;  /* linkage */
};

/* recvqlist is a list of recvq_entry structs */
TAILQ_HEAD(recvqlist, recvq_entry);

/* recvq buffer pool bin info (mainly for collecting stats) */
struct rbp_bininfo {
    int cnt;                      /* number of allocation */
    int size;                     /* size (once cnt > 0, ow. 0) */
};

/* recvq buffer pool */
struct recvq_bpool {
    int minsize;                  /* smallest allocation we use */
    int maxsize;                  /* largest allocation we use (e.g. ~MTU) */
    int nbins;                    /* number of size bins we've allocated */
    struct recvqlist *freebins;   /* binned array of free lists (by size) */
    struct rbp_bininfo *bininfo;  /* array of bin info */
    int (*sz2bin)(int size, int *bufsize);
                                  /* map fn: maps frame size to bin */
};

/*
 * mvp_queuing: top-level queuing structure shared by fdio and MPI threads
 */
struct mvp_queuing {
    /* no locking required */
    int fdio_notify[2];           /* fdio thread notification pipe */

    /* fdio_notify pipe fd assignments */
#define NOTE_RD    0              /* fdio thread reads notifications fd */
#define NOTE_WR    1              /* write notifications on this fd */

    /* structures that have their own locks */
    struct fbufmgr fbm_main;      /* qemu socket reads land here */
    struct fbufmgr fbm_bcast;     /* for bcast-related data */

    /* structures locked by qlock */
    pthread_mutex_t qlock;        /* the queuing lock */
    struct sendqlist mpisendq;    /* MPI send queue */
    int mpisqlen;                 /* #sqes on sendq */
    int mpimaxsqlen;              /* max size sendq has grown to */
    struct sendqlist mpisndfree;  /* list of free sendq_entry structs */
    int nsqe;                     /* number of sqes allocated */
    struct recvqlist mpirecvq;    /* MPI recv queue */
    int mpirqlen;                 /* #rqes on recvq */
    int mpimaxrqlen;              /* max size recvq has grown to */
    struct recvq_bpool rbpool;    /* receive buffer pool */
    int nrqe;                     /* number of rqes allocated */
};

/* api function prototypes */
int mvp_notify_fdio(struct mvp_queuing *mq, char note);

struct sendq_entry *mvp_sq_queue(struct mvp_queuing *mq, int dest,
                                 char *frame, int len, struct fbuf *owner);
struct sendq_entry *mvp_sq_dequeue(struct mvp_queuing *mq);
void mvp_sq_release(struct mvp_queuing *mq, struct sendq_entry *sqe);

struct recvq_entry *mvp_rq_alloc(struct mvp_queuing *mq, int sz);
void mvp_rq_queue(struct mvp_queuing *mq, struct recvq_entry *rqe);
struct recvq_entry *mvp_rq_queue_fbuf(struct mvp_queuing *mq, char *extbuf,
                                      int extlen, struct fbuf *owner,
                                      int donotify);
struct recvq_entry *mvp_rq_dequeue(struct mvp_queuing *mq, int *morep);
void mvp_rq_release(struct mvp_queuing *mq, struct recvq_entry *rqe);

int mvp_queuing_init(struct mvp_queuing *mq);
void mvp_queuing_bininfo(struct mvp_queuing *mq, int *nbinp,
                         struct rbp_bininfo **bininfop);
void mvp_queuing_stats(struct mvp_queuing *mq, struct que_stats *qs);
void mvp_queuing_finalize(struct mvp_queuing *mq);

#endif /* MVP_QUEUING_H */
