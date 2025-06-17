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
 * mvp_queuing.c  packet data buffering and queue management for mvpnet
 * 02-May-2025  chuck@ece.cmu.edu
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fdio_thread.h"          /* for FDIO_NOTE_QUEUE */
#include "mvp_mlog.h"
#include "mvp_queuing.h"

/*
 * static helper functions
 */

/*
 * allocate a sendq_entry from free list (or malloc if none free).
 * returns sqe for use, or NULL if memory allocation failure.
 * caller must be holding the qlock.
 */
static struct sendq_entry *mvp_sqe_alloc(struct mvp_queuing *mq) {
    struct sendq_entry *sqe;

    sqe = TAILQ_FIRST(&mq->mpisndfree);
    if (sqe) {
        TAILQ_REMOVE(&mq->mpisndfree, sqe, sq);
    }

    if (!sqe) {
        if ((sqe = malloc(sizeof(*sqe))) == NULL)
            return(NULL);
        mq->nsqe++;
        mlog(QUE_DBG, "malloc new sqe %p, nsqe=%d", sqe, mq->nsqe);
    }
    sqe->owner = NULL;    /* to be safe */
    return(sqe);

}

/*
 * map frame size to receive buffer pool free list bin
 */
static int mvp_bpool_size2bin(int size, int *bufsize) {
    int bin, max;

    if (size == 0) {
        *bufsize = 0;
        return(0);
    }
    if (size > MVP_BPOOLMAX) {
        mlog(QUE_ERR, "size2bin: size %d > BPOOLMAX %d", size, MVP_BPOOLMAX);
        return(-1);
    }

    for (bin = 1, max = MVP_BPOOLMIN ; size > max ; bin++, max <<= 1)
        /*nop*/;
    *bufsize = max;

    return(bin);

}

/*
 * init the receive queue buffer pool.  does not allocate any
 * recvq_entry structs.  return 0 on succes, -1 on error.
 */
static int mvp_rq_bpool_init(struct recvq_bpool *p, int minsz, int maxsz,
                             int (*sz2bin)(int sz, int *bufsize)) {
    int lcv, bs;
 
    if (minsz >= maxsz) {
        mlog(QUE_ERR, "bpool_init: minsz %d >= maxsz %d", minsz, maxsz);
        return(-1);
    }
    p->minsize = minsz;
    p->maxsize = maxsz;
    p->nbins = (*sz2bin)(maxsz, &bs) + 1;
    p->sz2bin = sz2bin;
    if (p->nbins < 2) {
        mlog(QUE_ERR, "bpool_init: nbins %d < 2", p->nbins);
        return(-1);
    }

    p->freebins = malloc(p->nbins * sizeof(p->freebins[0]));
    p->bininfo = malloc(p->nbins * sizeof(p->bininfo[0]));
    if (!p->freebins || !p->bininfo) {
        if (p->freebins) {
            free(p->freebins);
            p->freebins = NULL;
        } 
        if (p->bininfo) {
            free(p->bininfo);
            p->bininfo = NULL;
        }
        mlog(QUE_CRIT, "bpool_init: freebins malloc %d failed", p->nbins);
        return(-1);
    }
    for (lcv = 0 ; lcv < p->nbins ; lcv++) {
        TAILQ_INIT(&p->freebins[lcv]);
        p->bininfo[lcv].cnt = p->bininfo[lcv].size = 0;
    }
    return(0);
}

/*
 * finalize receive queue bpool.  bpool be out of service at this
 * point, so no need to qlock it.
 */
static void mvp_rq_bpool_finalize(struct recvq_bpool *p) {
    int lcv, b0, bn;
    struct recvq_entry *rqe;

    if (p->freebins == NULL)
        return;

    for (lcv = 0, b0 = 0, bn = 0 ; lcv < p->nbins ; lcv++) {
        while ((rqe = TAILQ_FIRST(&p->freebins[lcv])) != NULL) {
            TAILQ_REMOVE(&p->freebins[lcv], rqe, rq);
            if (lcv == 0) {
                b0++;         /* bin[0] is for external data */
            } else {
                if (rqe->frame) free(rqe->frame);
                bn++;
            }
            free(rqe);
        }
    }
    free(p->freebins);
    p->freebins = NULL;   /* to be safe */
    free(p->bininfo);
    p->bininfo = NULL;
    mlog(QUE_INFO, "bpool_fin: done.  rqe counts: bin[0]=%d, rest=%d", b0, bn);
}

/*
 * allocate a recvq_entry from the bin free list for the given size.
 * caller must be holding the qlock.  return rqe or NULL on failure.
 */
static struct recvq_entry *mvp_rqe_alloc(struct mvp_queuing *mq, int sz) {
    int bin, bufsize;
    struct recvq_entry *rqe;

    bin = mq->rbpool.sz2bin(sz, &bufsize);
    if (bin < 0 || bin >= mq->rbpool.nbins) {
        mlog(QUE_ERR, "req_alloc: sz2bin returns error (%d)", bin);
        return(NULL);
    }

    rqe = TAILQ_FIRST(&mq->rbpool.freebins[bin]);
    if (rqe) {
        TAILQ_REMOVE(&mq->rbpool.freebins[bin], rqe, rq);
        return(rqe);
    }

    /* need to allocate a new rqe */
    rqe = malloc(sizeof(*rqe));
    if (!rqe) {
        return(NULL);
    }
    if (bufsize == 0) {     /* bin 0 uses external bufs, no need to alloc */
        rqe->frame = NULL;
    } else {
        if (mq->rbpool.bininfo[bin].size &&
            bufsize != mq->rbpool.bininfo[bin].size) {
            mlog_abort(QUE_CRIT, "bpool bin %d size mismatch: %d != %d",
                       bin, bufsize, mq->rbpool.bininfo[bin].size);
        }
        rqe->frame = malloc(bufsize);
        if (!rqe->frame) {
            free(rqe);
            return(NULL);
        }
        if (mq->rbpool.bininfo[bin].size == 0)
            mq->rbpool.bininfo[bin].size = bufsize;
        mq->rbpool.bininfo[bin].cnt++;
    }
    rqe->flen = 0;
    rqe->bin = bin;
    rqe->owner = NULL;
    mq->nrqe++;
    mlog(QUE_DBG, "malloc new rqe %p, bin=%d, bufsize=%d, nrqe=%d",
         rqe, bin, bufsize, mq->nrqe);
    return(rqe);
}

/*
 * add rqe to the mpi recv queue.   caller must be holding qlock.
 * return 1 if the queue was previously empty (notification required).
 */
static int mvp_rqe_queue(struct mvp_queuing *mq, struct recvq_entry *rqe) {
    struct recvq_entry *head;

    head = TAILQ_FIRST(&mq->mpirecvq);
    TAILQ_INSERT_TAIL(&mq->mpirecvq, rqe, rq);
    mq->mpirqlen++;
    if (mq->mpirqlen > mq->mpimaxrqlen)
        mq->mpimaxrqlen = mq->mpirqlen;

    return( (head == NULL) ? 1 : 0);
}

/*
 * public api functions
 */

/*
 * fdio thread notification: send a notification message to the fdio thread.
 *  we asssume the thread is running.   return 0 on success, -1 on on error.
 */
int mvp_notify_fdio(struct mvp_queuing *mq, char note) {
    mlog(QUE_DBG, "notify-fdio %d", note);
    if (write(mq->fdio_notify[NOTE_WR], &note, sizeof(note)) == sizeof(note))
        return(0);
    mlog(QUE_ERR, "notify-fdio %d FAILED", note);
    return(-1);
}

/*
 * allocate a sendq_entry, fill it in, and add to send queue
 * for the MPI thread to process.  no need to notify the MPI
 * thread, since it polls this queue.  the caller must establish
 * the fbuf loan on the ethernet frame's buffer before calling us.
 * returns sqe on success, NULL on failure (to allocate sqe).
 */
struct sendq_entry *mvp_sq_queue(struct mvp_queuing *mq, int dest,
                                 char *frame, int len, struct fbuf *owner) {
    struct sendq_entry *sqe;
    pthread_mutex_lock(&mq->qlock);
    sqe = mvp_sqe_alloc(mq);
    if (sqe) {
        sqe->dest = dest;
        sqe->frame = frame;
        sqe->flen = len;
        sqe->owner = owner;
        TAILQ_INSERT_TAIL(&mq->mpisendq, sqe, sq);
        mq->mpisqlen++;
        if (mq->mpisqlen > mq->mpimaxsqlen)
            mq->mpimaxsqlen = mq->mpisqlen;
        mlog(QUE_DBG, "sq_queue sqe=%p, dest=%d, len=%d, fbuf=%p, qlen=%d",
             sqe, dest, len, owner, mq->mpisqlen);
    } else {
        mlog(QUE_ERR, "sq_queue sqe=FAILED, dest=%d, len=%d, fbuf=%p",
             dest, len, owner);
    }
    pthread_mutex_unlock(&mq->qlock);

    return(sqe);
}

/*
 * get and return the first sendq_entry on the send queue, if any.
 * returns NULL if send queue was empty.
 */
struct sendq_entry *mvp_sq_dequeue(struct mvp_queuing *mq) {
    struct sendq_entry *sqe;

    pthread_mutex_lock(&mq->qlock);
    sqe = TAILQ_FIRST(&mq->mpisendq);
    if (sqe) {
        TAILQ_REMOVE(&mq->mpisendq, sqe, sq);
        mq->mpisqlen--;
        mlog(QUE_DBG, "sq_dequeue sqe=%p", sqe);
    }
    pthread_mutex_unlock(&mq->qlock);

    return(sqe);
}

/*
 * release a sendq_entry to free list for reuse.
 */
void mvp_sq_release(struct mvp_queuing *mq, struct sendq_entry *sqe) {
    mlog(QUE_DBG, "sq_release sqe=%p", sqe);
    pthread_mutex_lock(&mq->qlock);
    TAILQ_INSERT_HEAD(&mq->mpisndfree, sqe, sq);
    pthread_mutex_unlock(&mq->qlock);
}

/*
 * allocate a recvq_entry with a buffer large enough to hold
 * data of the given size.   typically done by the MPI thread
 * for use with MPI_Recv().   returns the rqe or NULL on allocation
 * error.
 */
struct recvq_entry *mvp_rq_alloc(struct mvp_queuing *mq, int sz) {
    struct recvq_entry *rqe;

    /* we can just call mvp_rqe_alloc() after taking the qlock */
    pthread_mutex_lock(&mq->qlock);
    rqe = mvp_rqe_alloc(mq, sz);
    pthread_mutex_unlock(&mq->qlock);

    mlog(QUE_DBG, "rq_alloc rqe=%p", rqe);
    return(rqe);
}

/*
 * put a previously allocated rqe that we've filled with data
 * on the MPI receive queue for the fdio thread to process.
 * we will notify fdio if the queue was previously empty.
 */
void mvp_rq_queue(struct mvp_queuing *mq, struct recvq_entry *rqe) {
    int notify, ql;

    pthread_mutex_lock(&mq->qlock);
    notify = mvp_rqe_queue(mq, rqe);
    ql = mq->mpirqlen;
    pthread_mutex_unlock(&mq->qlock);

    mlog(QUE_DBG, "rq_queue rqe=%p, notify=%d, qlen=%d", rqe, notify, ql);
    if (notify && mvp_notify_fdio(mq, FDIO_NOTE_QUEUE) != 0)
        mlog(QUE_DBG, "rq_queue: notify failed?");
}

/*
 * allocate a rqe for a fbuf buffer (that we have a loan for),
 * install the fbuf in the rqe, and put it on the MPI receive queue
 * for the fdio thread to process.   optionally do a notification
 * if the queue was empty (if we are fdio thread we don't need
 * to notify ourselves).   return rqe on success, NULL on failure.
 */
struct recvq_entry *mvp_rq_queue_fbuf(struct mvp_queuing *mq, char *extbuf,
                                      int extlen, struct fbuf *owner,
                                      int donotify) {
    struct recvq_entry *rqe;
    int notify;

    pthread_mutex_lock(&mq->qlock);
    rqe = mvp_rqe_alloc(mq, 0);    /* len==0 => bin[0] external buf */
    if (rqe) {
       rqe->frame = extbuf;
       rqe->flen = extlen;
       rqe->owner = owner;
       notify = mvp_rqe_queue(mq, rqe);
       mlog(QUE_DBG, "rq_queue_fbuf rqe=%p, notify=%d, owner=%p, qlen=%d",
            rqe, notify, owner->mgr, mq->mpirqlen);
    } else {
       notify = 0;
       mlog(QUE_CRIT, "rq_queue_fbuf rqe=FAILED, owner=%p", owner->mgr);
    }
    pthread_mutex_unlock(&mq->qlock);

    if (donotify && notify && mvp_notify_fdio(mq, FDIO_NOTE_QUEUE) != 0)
        mlog(QUE_DBG, "rq_queue_fbuf: notify failed?");

    return(rqe);
}

/*
 * get the first recvq_entry on the recv queue, if any.
 * if morep is not null, set to 1 if there are still rqes
 * on the queue after we've removed the one we are returning.
 * returns NULL if recv queue was empty.
 */
struct recvq_entry *mvp_rq_dequeue(struct mvp_queuing *mq, int *morep) {
    struct recvq_entry *rqe;
    int more = 0;

    pthread_mutex_lock(&mq->qlock);
    rqe = TAILQ_FIRST(&mq->mpirecvq);
    if (rqe) {
        TAILQ_REMOVE(&mq->mpirecvq, rqe, rq);
        mq->mpirqlen--;
        if (TAILQ_FIRST(&mq->mpirecvq))         /* did not empty queue? */
            more++;
        mlog(QUE_DBG, "rq_dequeue rqe=%p, more=%d", rqe, more);
    }
    pthread_mutex_unlock(&mq->qlock);

    if (morep)
        *morep = more;
    return(rqe);
}

/*
 * release a recvq_entry to its bin's free list for reuse.
 */
void mvp_rq_release(struct mvp_queuing *mq, struct recvq_entry *rqe) {
    if (rqe->bin < 0 || rqe->bin >= mq->rbpool.nbins) {
        mlog_abort(QUE_CRIT, "mvp_rq_release: correct rqe=%p bin=%d n=%d",
                   rqe, rqe->bin, mq->rbpool.nbins);
        /*NOTREACHED*/
    }
    if (rqe->bin == 0) {    /* to be safe, clear old external info */
        rqe->frame = NULL;
        rqe->owner = NULL;
    }
    mlog(QUE_DBG, "rq_release: rqe=%p (bin=%d)", rqe, rqe->bin);
    pthread_mutex_lock(&mq->qlock);
    TAILQ_INSERT_HEAD(&mq->rbpool.freebins[rqe->bin], rqe, rq);
    pthread_mutex_unlock(&mq->qlock);
}

/*
 * mvp_queuing init.   setup queuing data structures before putting
 * them in service.  return 0 on success, -1 on error.
 */
int mvp_queuing_init(struct mvp_queuing *mq) {

    /* make it safe to call finalize on mq (if we get an error) */
    memset(mq, 0, sizeof(*mq));
    mq->fdio_notify[0] = mq->fdio_notify[1] = -1;
    pthread_mutex_init(&mq->qlock, NULL);
    TAILQ_INIT(&mq->mpisendq);
    TAILQ_INIT(&mq->mpisndfree);
    TAILQ_INIT(&mq->mpirecvq);

    if (pipe(mq->fdio_notify) < 0) {
        mlog(QUE_ERR, "pipe() failed: %s", strerror(errno));
        return(-1);
    }

    if (fbufmgr_init(&mq->fbm_main, "main", MVP_FBUFSIZE, MVP_FBUFMIN) != 0 ||
        fbufmgr_init(&mq->fbm_bcast, "bcst", MVP_FBUFSIZE, MVP_FBUFMIN) != 0) {
        mlog(QUE_ERR, "fbufmgr init failed!");
        goto err;
    }

    if (mvp_rq_bpool_init(&mq->rbpool, MVP_BPOOLMIN,
                          MVP_BPOOLMAX, mvp_bpool_size2bin) != 0) {
        mlog(QUE_ERR, "bpool init failed!");
        goto err;
    }

    mlog(QUE_DBG, "mvp_queuing_init: complete");
    return(0);

err:
    mvp_queuing_finalize(mq);
    return(-1);
}

/*
 * mvp_queuing rqe bin info.   returns a pointer to current
 * (live) bininfo array.  typically called during shutdown
 * for stat collection.  caller should finish using the return
 * info before finalizing the mq.
 */
void mvp_queuing_bininfo(struct mvp_queuing *mq, int *nbinp,
                         struct rbp_bininfo **bininfop) {
    *nbinp = mq->rbpool.nbins;
    *bininfop = mq->rbpool.bininfo;
}

/*
 * mvp_queuing stats.  load stats into provided que_stats structure.
 * this does all stats except for bininfo.
 */
void mvp_queuing_stats(struct mvp_queuing *mq, struct que_stats *qs) {
    fbufmgr_stats(&mq->fbm_main, &qs->fbsmain);
    fbufmgr_stats(&mq->fbm_bcast, &qs->fbsbcast);
    qs->nsqe = mq->nsqe;
    qs->mpimaxsqlen = mq->mpimaxsqlen;
    qs->nrqe = mq->nrqe;
    qs->mpimaxrqlen = mq->mpimaxrqlen;
}

/*
 * mvp_queueing finalize.  make sure all processing has stopped
 * before trying to finalize the queues (i.e. locking no longer
 * needed).
 */
void mvp_queuing_finalize(struct mvp_queuing *mq) {
    struct sendq_entry *sqe;
    struct recvq_entry *rqe;

    /* close notification fds */
    if (mq->fdio_notify[0] != -1)
        close(mq->fdio_notify[0]);    
    if (mq->fdio_notify[1] != -1)
        close(mq->fdio_notify[1]);    

    /* drop/free sqes waiting for isend, then clear the free list */
    while ((sqe = TAILQ_FIRST(&mq->mpisendq)) != NULL) {
        mlog(QUE_DBG, "finalize drop sqe %p", sqe);
        TAILQ_REMOVE(&mq->mpisendq, sqe, sq);
        if (sqe->owner)
            fbuf_return(sqe->owner);        /* drop loan */
        free(sqe);
    }
    while ((sqe = TAILQ_FIRST(&mq->mpisndfree)) != NULL) {
        TAILQ_REMOVE(&mq->mpisndfree, sqe, sq);
        free(sqe);
    }

    /* drop/free rqes waiting for fdio to handle them */
    while ((rqe = TAILQ_FIRST(&mq->mpirecvq)) != NULL) {
        mlog(QUE_DBG, "finalize drop rqe %p", rqe);
        TAILQ_REMOVE(&mq->mpirecvq, rqe, rq);
        if (rqe->bin == 0) {
            if (rqe->owner)
                fbuf_return(rqe->owner);   /* drop loan if from fbuf */
        } else {
            if (rqe->frame)
                free(rqe->frame);          /* free buffer space */
        }
        free(rqe);
    }
    if (mq->rbpool.freebins)               /* clear the free lists */
        mvp_rq_bpool_finalize(&mq->rbpool);

    /* all loans should be released now, so safe to finialize fbufmgrs */
    if (mq->fbm_main.active)
        fbufmgr_finalize(&mq->fbm_main);
    if (mq->fbm_bcast.active)
        fbufmgr_finalize(&mq->fbm_bcast);
    mlog(QUE_INFO, "mvp_queuing_finalize complete with nsqe=%d, nrqe=%d",
         mq->nsqe, mq->nrqe);
}
