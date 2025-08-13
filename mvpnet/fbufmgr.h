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
 * fbufmgr.h  framing buffer (fbuf) code.
 * 26-Apr-2025  chuck@ece.cmu.edu
 */
#ifndef MVP_FBUFMGR_H
#define MVP_FBUFMGR_H

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/queue.h>

/*
 * stats collected by fbufmgr
 */
struct fbuf_stats {
    int maxfbufs;             /* max # of fbufs allocated (from nfbufs) */
    /* retire stat counters */
    int retire_nol_empty;     /* # fbuf retires w/o loans or data */
    int retire_nol_compact;   /* # fbuf retires w/o loans w/compaction */
    int retire_loaned;        /* # fbuf retires w/active loans */
    /* socket recv/advance counters */
    int sockrcv_cnt;          /* # of socket recv() calls */
    int sockrcv_nodata;       /* # of socket recv() calls w/o data */
    uint64_t sockrcv_bytes;   /* # of socket bytes received */
    int advance_no;           /* # of advance() calls that did not advance */
    int advance_some;         /* # of advance() calls that left residual */
    int advance_all;          /* # of advance() calls that used up all bytes */
};

struct fbuf;                  /* forward decl */

TAILQ_HEAD(fbuflist, fbuf);   /* fbuflist is a list of fbufs */

/*
 * the fbuf manager (fbufmgr) owns a set of fbufs.
 */
struct fbufmgr {
    char *id;                 /* id/name of fbufmgr (for diagnostic logs) */
    size_t fbufsize;          /* fbuf allocation size */
    size_t min_avail;         /* min active fbuf space, min_avail < fbufsize */
    int nfbufs;               /* #of fbufs allocated for this fbufmgr */
    struct fbuf *active;      /* the active fbuf */
    pthread_mutex_t lck;      /* locks free, loaned, and loancnt (in fbuf) */
    struct fbuflist free;     /* free list */
    struct fbuflist loaned;   /* full fbufs with active loans */
    struct fbuf_stats fbs;    /* stats we collect */
};

/*
 * fbuf (owned by a fbufmgr)
 */
struct fbuf {
    /* these fields are constant after allocation */
    char *fbufbuf;            /* malloced fbuf buffer */
    struct fbufmgr *mgr;      /* our owner */
    char *end;                /* fbuf + mgr->fbufsize */
    /* the mgr changes these fields as it reads new data */
    char *fstart;             /* start of next frame (to be processed) */
    char *rp;                 /* recv pointer (fbuf <= fstart <= rp < end) */
    /* the remaining fields are locked by mgr->lck */
    int loancnt;              /* #of frames loaned out for processing */
    TAILQ_ENTRY(fbuf) l;      /* linkage for free/loaned lists */
};

/* fbufmgr_recv callback */
typedef size_t fbuf_cb_t(struct fbuf *fbuf, char *fstart,
                         size_t nbytes, void *arg);

int fbufmgr_init(struct fbufmgr *mgr, char *id, size_t fbsize,
                 size_t minavail);
int fbufmgr_loan_newframe(struct fbufmgr *mgr, size_t size, int nloan,
                          void **framep, struct fbuf **fbufp);
ssize_t fbufmgr_recv(struct fbufmgr *mgr, int sock, fbuf_cb_t fcb, void *arg);
void fbufmgr_stats(struct fbufmgr *mgr, struct fbuf_stats *fbs);
void fbufmgr_finalize(struct fbufmgr *mgr);

void fbuf_loan(struct fbuf *fbuf, int nloan);
void fbuf_return(struct fbuf *fbuf);

#endif /* MVP_FBUFMGR_H */
