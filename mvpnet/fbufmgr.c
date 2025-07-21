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
 * fbufmgr.c  framing buffer (fbuf) code.
 * 26-Apr-2025  chuck@ece.cmu.edu
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/queue.h>
#include <sys/socket.h>

#include "fbufmgr.h"
#include "mvp_mlog.h"

/*
 * framing buffer (fbuf) and framing buffer manager (fbufmgr) code.
 *
 * fbufs are append-only buffers that are subdivided into frames
 * as new data is appended.  fbufs should be sized large enough
 * so that they can contain multiple frames.  depending on how
 * new data is received, the frame at the end of a fbuf may not
 * be complete.  data from incomplete frames remains buffered at
 * the end of a fbuf until the rest of the frame's data arrives.
 *
 * once a frame is complete, its memory can be "loaned" out to some
 * other object for processing.  when the processing is complete
 * the loan is returned.  each fbuf has a counter for the number
 * loans it currently has active.  fbufs with a loan count of
 * zero that are no longer being appended to can be freed for
 * reuse.
 *
 * the fbufmgr manages a set of fbufs.  the fbuf currently being
 * appended to is the "active" fbuf.  the other fbufs are either
 * fully filled fbufs that currently have data loaned out or are
 * free fbufs that are ready for reallocation.  the fbufmgr ensures
 * that the currently active fbuf has a minimum number of free
 * bytes (e.g. enough for the largest size frame).  if the number
 * of free bytes in the active fbuf drops below this value, then
 * the fbufmgr will retire the active fbuf and allocate a new one.
 * retired fbufs with loans remain allocated until the last loan
 * is returned, then the fbuf is added to the free list.
 *
 * we assume that the thread that owns the fbufmgr is the only
 * one that adds data to it (no need to lock).  the only data
 * fields shared with other threads are the fbuf loan counters
 * and the fbufmgr loaned/free lists (these are all protected by
 * the fbufmgr's lck mutex).
 *
 * fbuf data received from a socket file descriptor using
 * fbufmgr_recv() uses a callback function to parse and partition
 * the new data into frames.  fbufmgr_recv() is the only
 * function that can generate incomplete frames (e.g. when receiving
 * data on a SOCK_STREAM socket where message boundaries are not
 * preserved).
 *
 * the fbufmgr also support the fbufmgr_loan_newframe() API.  this
 * interface provides a non-socket non-callback based mechanism
 * for reserving space for a complete frame in a fbuf.  note
 * that fbufmgr_loan_newframe() will fail if called on a fbuf that
 * contains an incomplete frame.  to be safe, do not mix calls
 * to fbufmgr_recv() and fbufmgr_loan_newframe() on the same fbufmgr.
 * (instead, use two fbufmgrs: one for recv and one for loan_newframe.)
 */

/*
 * allocate a new fbuf for a mgr.  only mgr's thread should call this.
 * returns new fbuf or NULL on allocation failure.
 */
static struct fbuf *fbuf_alloc(struct fbufmgr *mgr) {
    struct fbuf *fb;

    if ( (fb = malloc(sizeof(*fb))) == NULL ||
         (fb->fbufbuf = malloc(mgr->fbufsize)) == NULL) {
        if (fb) free(fb);
        return(NULL);
    }

    mgr->nfbufs++;
    fb->mgr = mgr;
    fb->end = fb->fbufbuf + mgr->fbufsize;
    fb->fstart = fb->rp = fb->fbufbuf;
    fb->loancnt = 0;
    return(fb);
}

/*
 * init fbufmgr struct from caller for use.   allocates an
 * active fbuf to start (and maybe a spare for the free list).
 * return 0 on success, -1 on failure.
 */
int fbufmgr_init(struct fbufmgr *mgr, char *id, size_t fbsize,
                 size_t minavail) {
    struct fbuf *spare;

    if (fbsize < minavail)       /* sanity check */
        return(-1);

    mgr->id = id;
    mgr->fbufsize = fbsize;
    mgr->min_avail = minavail;
    mgr->nfbufs = 0;
    mgr->active = fbuf_alloc(mgr);
    if (!mgr->active) {
        mlog(FBUF_CRIT, "init: fbufmgr %s init alloc failed", id);
        return(-1);
    }
    pthread_mutex_init(&mgr->lck, NULL);
    TAILQ_INIT(&mgr->free);
    TAILQ_INIT(&mgr->loaned);
    if ((spare = fbuf_alloc(mgr)) != NULL) {
        TAILQ_INSERT_HEAD(&mgr->free, spare, l);
    }
    /* zero stats */
    memset(&mgr->fbs, 0, sizeof(mgr->fbs));
    mlog(FBUF_NOTE, "init: id=%s, sz=%zd, minavail=%zd", id, fbsize, minavail);
    mlog(FBUF_NOTE, "init: id=%s, act=%p, spare=%p", id, mgr->active, spare);
    return(0);
}

/*
 * retire active fbuf (it ran out of free space) and install a
 * new one.  we hold the lock throughout.  currently aborts on failure.
 */
static void fbufmgr_retire_active(struct fbufmgr *mgr) {
    struct fbuf *oldact;
    int resid;

    mlog(FBUF_DBG, "retire: id=%s, active=%p", mgr->id, mgr->active);
    pthread_mutex_lock(&mgr->lck);

    oldact = mgr->active;
    resid = oldact->rp - oldact->fstart; /* data waiting to be framed */

    if (oldact->loancnt == 0) {          /* no loans, reset and reuse active */
        if (resid == 0) {
            oldact->fstart = oldact->rp = oldact->fbufbuf;
            mgr->fbs.retire_nol_empty++;
            mlog(FBUF_DBG, "retire: id=%s reset active=%p", mgr->id, oldact);
            goto done;
        }
        if (oldact->fstart == oldact->fbufbuf) {  /* no room to compact? */
            mlog_abort(FBUF_CRIT,
                       "retire: id=%s compact fail active=%p, resid=%d, abort",
                       mgr->id, oldact, resid);  /* shouldn't happen */
            /*NOTREACHED*/
        }
        memcpy(oldact->fbufbuf, oldact->fstart, resid); /* cp resid to start */
        oldact->fstart = oldact->fbufbuf;
        oldact->rp = oldact->fstart + resid;
        mgr->fbs.retire_nol_compact++;
        mlog(FBUF_DBG, "retire: id=%s compact/reuse active %p, resid=%d",
             mgr->id, oldact, resid);
        /* oldact remains active after the reset */
        goto done;
    }

    /* put old active in loaned list and allocate a new one */
    TAILQ_INSERT_TAIL(&mgr->loaned, oldact, l);
    mgr->fbs.retire_loaned++;
    mlog(FBUF_DBG, "retire: id=%s mv active %p to loaned, lcnt=%d, resid=%d",
         mgr->id, oldact, oldact->loancnt, resid);

    if ((mgr->active = TAILQ_FIRST(&mgr->free)) != NULL) {
        TAILQ_REMOVE(&mgr->free, mgr->active, l);
        mgr->active->fstart = mgr->active->rp = mgr->active->fbufbuf;
        mlog(FBUF_DBG, "retire: id=%s new active %p (via free list)",
             mgr->id, mgr->active);
    } else {
        mgr->active = fbuf_alloc(mgr);
        if (!mgr->active) {
            mlog_abort(FBUF_CRIT, "%s: no fbuf memory - abort!", mgr->id);
            /*NOTREACHED*/
        }
        mlog(FBUF_DBG, "retire: id=%s new active %p (via fbuf_alloc)",
             mgr->id, mgr->active);
    }
    if (resid) {         /* copy data waiting to be framed to new active */
        memcpy(mgr->active->rp, oldact->fstart, resid);
        mgr->active->rp += resid;
    }

done:
    pthread_mutex_unlock(&mgr->lck);
}

/*
 * get space for a new frame in a fbufmgr.  we establish a loan
 * for the new frame.   the caller is responsible for copying the
 * frame data in.   the active fbuf in fbufmgr must not be holding
 * a partial frame (or we'll fail).   return 0 on success,
 * return a unix errno code on failure.
 */
int fbufmgr_loan_newframe(struct fbufmgr *mgr, size_t size, void **framep,
                         struct fbuf **fbufp) {
    struct fbuf *active = mgr->active;

    /* validate size - none of these should happen with normal usage */
    if (size > mgr->min_avail) {             /* size larger than max? */
        mlog(FBUF_ERR, "loan_newframe: id=%s EINVAL size=%zd, minava=%zd",
             mgr->id, size, mgr->min_avail);
        return(EINVAL);
    }
    if (size > (active->end - active->rp)) { /* no space for size? */
        mlog(FBUF_ERR, "loan_newframe: id=%s ENOSPC size=%zd, got=%zd",
             mgr->id, size, active->end - active->rp);
        return(ENOSPC);
    }
    if (active->fstart != active->rp)      { /* incomplete frame? */
        mlog(FBUF_ERR, "loan_newframe: id=%s loan_newframe EBUSY", mgr->id);
        return(EBUSY);
    }

    fbuf_loan(active);                       /* caller must return loan */
    *framep = active->fstart;
    *fbufp = active;

    active->rp += size;
    active->fstart = active->rp;
    mlog(FBUF_DBG, "loan_newframe: id=%s OK active=%p, size=%zd, resid=%zd",
         mgr->id, active, size, active->end - active->rp);

    if (active->end - active->rp < mgr->min_avail) {  /* below threshold? */
        fbufmgr_retire_active(mgr);
    }

    return(0);      /* success! */
}

/*
 * fbuf_cb_t:
 * fbuf callback takes an fbuf, a starting pointer within it, and
 * a length.   the callback parses the buffer into one or more frames
 * and loans them (from the fbuf) upward for processing.   returns
 * number of bytes to advance fstart (if any).  the callback can
 * drop frame data by advancing fstart without doing any processing.
 */

/*
 * recv data from a socket into a fbufmgr's currently active fbuf.
 * callers must ensure the socket is readable if they want to avoid
 * blocking in the recv() call.   the caller must provide a callback
 * to handle framing the received data.  returns number of bytes recvd
 * or -1 on on error.
 */
ssize_t fbufmgr_recv(struct fbufmgr *mgr, int sock,
                     fbuf_cb_t fcb, void *arg) {
    ssize_t got;
    size_t advance;

    struct fbuf *active = mgr->active;

    /* get some data */
    got = recv(sock, active->rp, active->end - active->rp, 0);
    mgr->fbs.sockrcv_cnt++;
    if (got < 1) {
        mgr->fbs.sockrcv_nodata++;
        mlog(FBUF_DBG, "recv: no-advance, id=%s, got=%zd", mgr->id, got);
        return(got);
    }
    active->rp += got;
    mgr->fbs.sockrcv_bytes += got;

    advance = (*fcb)(active, active->fstart, active->rp - active->fstart, arg);
    if (advance < 1) {
        mgr->fbs.advance_no++;
        mlog(FBUF_DBG, "recv: id=%s, active=%p, got=%zd, no-advance", mgr->id,
             mgr->active, got);
    } else {
        mlog(FBUF_DBG, "recv: id=%s, active=%p, got=%zd, advance=%zd", mgr->id,
             mgr->active, got, advance);
        if (advance > active->rp - active->fstart) { /* shouldn't happen */
            mlog_abort(FBUF_CRIT, "recv: id=%s, advance %zd too big, abort!",
                       mgr->id, advance);
        }
        active->fstart += advance;
        if (active->fstart == active->rp)
            mgr->fbs.advance_all++;
        else
            mgr->fbs.advance_some++;
        if (active->end - active->rp < mgr->min_avail) {
            fbufmgr_retire_active(mgr);
        }
    }
    return(got);
}

/*
 * loan out space in a fbuf.   used by fbuf_cb_t callback function
 * to place a hold on reusing the fbuf's buffer space while it is
 * being processed.
 */
void fbuf_loan(struct fbuf *fbuf) {
    struct fbufmgr *mgr = fbuf->mgr;

    pthread_mutex_lock(&mgr->lck);
    fbuf->loancnt++;
    mlog(FBUF_DBG, "loan: id=%s, fbuf=%p, new loancnt=%d", mgr->id,
         fbuf, fbuf->loancnt);
    pthread_mutex_unlock(&mgr->lck);
}

/*
 * return fbuf loan to its manager.  used by external processing
 * routines started by the fbuf_cb_t callback to indicate that we
 * no longer need this hold on the buffer.
 */
void fbuf_return(struct fbuf *fbuf) {
    struct fbufmgr *mgr = fbuf->mgr;

    pthread_mutex_lock(&mgr->lck);
    if (fbuf->loancnt > 0)
        fbuf->loancnt--;
    mlog(FBUF_DBG, "return: id=%s, fbuf=%p (a=%d), new loancnt=%d", mgr->id,
         fbuf, fbuf == mgr->active, fbuf->loancnt);

    /* we can free the fbuf if it isn't active and last loan was dropped */
    if (fbuf->loancnt == 0 && fbuf != mgr->active) {
        TAILQ_REMOVE(&mgr->loaned, fbuf, l);
        TAILQ_INSERT_HEAD(&mgr->free, fbuf, l);
    }
    pthread_mutex_unlock(&mgr->lck);
}

/*
 * copy out fbufmgr stats.   assumes we own the fbufmgr
 * (so it will not change while we copy data).
*/
void fbufmgr_stats(struct fbufmgr *mgr, struct fbuf_stats *fbs) {
    *fbs = mgr->fbs;               /* structure copy */
    fbs->maxfbufs = mgr->nfbufs;   /* update with current nfbufs */
}

/*
 * finalize a fbufmgr.  the caller must own the fbufmgr and there
 * must not be any outstanding loans.   if there are no outstanding
 * loans, there is no danger of other thread trying to access the
 * fbufmgr (to return loans) and no locking is required.
 */
void fbufmgr_finalize(struct fbufmgr *mgr) {
    struct fbuf *fb;

    if (mgr->active && mgr->active->loancnt) {
        mlog_abort(FBUF_CRIT, "finalize %s: active has loans, abort", mgr->id);
        /*NOTREACHED*/
    }
    if (TAILQ_FIRST(&mgr->loaned)) {
        mlog_abort(FBUF_CRIT, "finalize %s: loaned not empty, abort", mgr->id);
        /*NOTREACHED*/
    }

    if (mgr->active) {
         if (mgr->active->fbufbuf)
             free(mgr->active->fbufbuf);
         free(mgr->active);
         mgr->active = NULL;
    }

    while ((fb = TAILQ_FIRST(&mgr->free)) != NULL) {
        TAILQ_REMOVE(&mgr->free, fb, l);
        free(fb->fbufbuf);
        free(fb);
    }
    /* already checked mgr->loaned and it is empty */

    mlog(FBUF_NOTE, "finalize %s: done", mgr->id);
}
