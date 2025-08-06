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
 * mpi_thread.c  mpi i/o thread
 * 09-May-2025  chuck@ece.cmu.edu
 */

#include <err.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>      /* sched_yield() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mpi_thread.h"
#include "mvp_mlog.h"
#include "mvp_stats.h"

/*
 * manage combined state for MPI_Testsome() calls.
 */
struct mpimtsmgr {
    /* slot allocation counters */
    int nalloc;                 /* number of test slots allocated */
    int nused;                  /* number of test slots in use (<=nalloc) */

    /* stat counters */
    uint64_t ntestsome;         /* number of MPI_Testsome() calls */
    uint64_t got1;              /* #times testsome returned 1 entry */
    uint64_t gotmore;           /* #times testsome return >1 entry */
    uint64_t gotmax;            /* max# entries testsome returned */

    /* arrays (all contain nalloc entries) */
    struct sendq_entry **sqes;  /* array of sqes (in sync with reqs[]) */
    MPI_Request *reqs;          /* array of requests we are testing */
    int *idxout;                /* array of completed index vals */
    MPI_Status *statout;        /* array of output status vals */
};

#define MPIMTSMGR_INIT                                                         \
    (struct mpimtsmgr) {                                                       \
        .nalloc = 0, .nused = 0, .ntestsome = 0, .got1 = 0, .gotmore = 0,      \
        .gotmax = 0, .sqes = NULL, .reqs = NULL, .idxout = NULL,               \
        .statout = NULL                                                        \
    }

/*
 * ensure there are at least 'n' slots in the mpimtsmgr.
 * realloc arrays if they are too small.   return 0 on
 * success, -1 on memory allocation failure.
 */
static int mpimts_setslots(struct mpimtsmgr *mts, int n) {
    void *tmp;

    if (n <= mts->nalloc)    /* already large enough? */
        return(0);

    tmp = realloc(mts->sqes, n * sizeof(mts->sqes[0]));
    if (!tmp)
        return(-1);
    mts->sqes = tmp;

    tmp = realloc(mts->reqs, n * sizeof(mts->reqs[0]));
    if (!tmp)
        return(-1);
    mts->reqs = tmp;

    tmp = realloc(mts->idxout, n * sizeof(mts->idxout[0]));
    if (!tmp)
        return(-1);
    mts->idxout = tmp;

    tmp = realloc(mts->statout, n * sizeof(mts->statout[0]));
    if (!tmp)
        return(-1);
    mts->statout = tmp;

    mts->nalloc = n;
    return(0);
}

/*
 * free arrays and reset.  caller should empty out pending
 * requests before calling this.
 */
static void mpimts_afree(struct mpimtsmgr *mts) {
    if (mts->sqes)
        free(mts->sqes);
    if (mts->reqs)
        free(mts->reqs);
    if (mts->idxout)
        free(mts->idxout);
    if (mts->statout)
        free(mts->statout);
    *mts = MPIMTSMGR_INIT;   /* reset */
}

/*
 * init mpimtsmgr.   return 0 on succes, -1 on allocation failure.
 */
static int mpimts_init(struct mpimtsmgr *mts, int n) {
    int ret;

    *mts = MPIMTSMGR_INIT;
    ret = mpimts_setslots(mts, n);
    if (ret == -1) {
        mpimts_afree(mts);
        return(-1);
    }
    mlog(MPI_INFO, "mts init to %d testsome slots", n);
    return(0);
}

/*
 * add sqe and request to test arrays.   return 0 on success,
 * -1 on failure.
 */
static int mpimts_add(struct mpimtsmgr *mts, struct sendq_entry *sqe,
                      MPI_Request req) {
    int slot;

    /* if all array slots are in use, grow mgr */
    if (mts->nused == mts->nalloc) {
        if (mpimts_setslots(mts, mts->nalloc * 2) == -1)
            return(-1);
        mlog(MPI_INFO, "mts grew testsome slot count to %d", mts->nalloc);
    }

    /* put new entry at the end of the arrays (same slot#) */
    slot = mts->nused;
    mts->sqes[slot] = sqe;
    mts->reqs[slot] = req;
    mts->nused++;
    mlog(MPI_DBG, "sqe %p added to mts slot %d.  nused now %d.",
         sqe, slot, mts->nused);
   return(0);
}

/*
 * sqe send is complete.  drop its fbuf loan and release it to
 * mvq_queuing for reuse.
 */
static void mpimts_done(struct mvp_queuing *mq, struct sendq_entry *sqe) {
    if (sqe->owner)      /* currently should always be set */
        fbuf_return(sqe->owner);
    mvp_sq_release(mq, sqe);
}

/*
 * do a MPI_Testsome now.   return 0 on success, -1 if we get
 * a general error.
 */
static int mpimts_test(struct mpimtsmgr *mts, struct mpi_args *ma) {
    int rv, got, lcv, firsthole, i, err, new_nused, pullfrom;
    struct sendq_entry *sqe;

    if (mts->nused == 0)     /* nothing to test? */
        return(0);

    rv = MPI_Testsome(mts->nused, mts->reqs, &got, mts->idxout, mts->statout);
    mts->ntestsome++;

    if (rv == MPI_UNDEFINED) {
        /* this should never happen */
        mlog(MPI_ERR, "MPI_Testsome returned MPI_UNDEFINED, reset!");
        mts->nused = 0;
        return(0);
    }

    if (rv != MPI_SUCCESS && rv != MPI_ERR_IN_STATUS) {
        mlog(MPI_CRIT, "MPI_Testsome returned unexpected val %d", rv);
        return(-1);    /* this shouldn't happen either... */
    }

    if (got == 0)     /* if nothing finished, we are done */
        return(0);

     /* update stats */
    if (got == 1)
        mts->got1++;
    else if (got > 1)
        mts->gotmore++;
    if (got > mts->gotmax)
        mts->gotmax = got;

    /* release complete sqes, finding firsthole in the process */
    for (lcv = 0, firsthole = mts->idxout[0] ; lcv < got ; lcv++) {
        i = mts->idxout[lcv];
        if (i < firsthole)
            firsthole = i;
        sqe = mts->sqes[i];
        mts->sqes[i] = NULL;   /* create corrsponding hole in sqes[] */
        err = (rv == MPI_SUCCESS) ? MPI_SUCCESS : mts->statout[i].MPI_ERROR;
        if (err != MPI_SUCCESS) {
            mlog(MPI_ERR, "mts Isend sqe %p %d->%d failed (mpierr=%d)",
                 sqe, ma->mi.rank, sqe->dest, err);
        }
        mlog(MPI_DBG, "mts sqe done: err=%d, sqe=%p, owner=%p", err,
             sqe, sqe->owner);
        mpimts_done(ma->a->mq, sqe);
    }

    if (got >= mts->nused) {    /* if everything is done, just reset */
        mts->nused = 0;
        mlog(MPI_DBG, "mts update: nused=0 (got=%d)", got);
        return(0);
    }

    /*
     * we need to compact out all the holes in mts->sqes[] while
     * keeping mts->reqs[] in sync.   we know where the first hole
     * is and we know the new smaller size of the array.
     */
    new_nused = mts->nused - got;
    pullfrom = new_nused;  /* use ones from here to fill holes */
    for (lcv = firsthole ; lcv < new_nused ; lcv++) {

        if (mts->sqes[lcv] != NULL)  /* already full, skip to next one */
            continue;

         /* scan past end of new array for a non-null entry we can pull */
         while (pullfrom < mts->nused && mts->sqes[pullfrom] == NULL)
             pullfrom++;
        if (pullfrom >= mts->nused) {
             mlog(MPI_ERR, "mts: pull overflow - should not happen!");
             new_nused = 0;  /* reset, may leak memory */
             break;
        }

        /* fill the hole, keeping the slot# in sync */
        mts->sqes[lcv] = mts->sqes[pullfrom];
        mts->reqs[lcv] = mts->reqs[pullfrom];
        pullfrom++;      /* consumed, advance to next one */
    }
    mts->nused = new_nused;
    mlog(MPI_DBG, "mts update: nused=%d (got=%d)", mts->nused, got);

    return(0);
}

/*
 * finalize mpimts before exiting.   the mts is no longer active.
 */
static void mpimts_finalize(struct mpimtsmgr *mts, struct mpi_args *ma) {
    int lcv;

    for (lcv = 0 ; lcv < mts->nused ; lcv++) {
        if (mts->reqs[lcv] != MPI_REQUEST_NULL)
            MPI_Request_free(&mts->reqs[lcv]);  /* ignore failures */
        if (mts->sqes[lcv]) {
            mlog(MPI_DBG, "mts sqe finalize-drop: sqe=%p, owner=%p",
                     mts->sqes[lcv], mts->sqes[lcv]->owner);
            mpimts_done(ma->a->mq, mts->sqes[lcv]);
        }
    }
    mts->nused = 0;
    mpimts_afree(mts);
}

/*
 * MPI_Iprobe says we have data that can be received.
 * allocate space from the bpool, recv the data into it,
 * and queue result for processing by the fdio thread.
 * return number of bytes we recv or 0 if we got an error.
 */
static int mpibpoolrecv(struct mpi_args *ma, MPI_Status *status) {
    int count, ret;
    struct recvq_entry *rqe;

    if (MPI_Get_count(status, MPI_BYTE, &count) != MPI_SUCCESS)
        mlog_exit(1, MPI_CRIT, "MPI_Get_count: unexpectedly failed!?!");
    if (count < 1)
        mlog_exit(1, MPI_CRIT, "MPI_Get_count: impossible count %d", count);

    /* allocate recv buffer of proper size ... */
    rqe = mvp_rq_alloc(ma->a->mq, count);
    if (!rqe) {
        char dummy[128];
        mlog(MPI_CRIT, "rqe alloc failed!  Dropping message");
        /* attempt to throw it away, ignore possible truncate error */
        ret = MPI_Recv(dummy, sizeof(dummy), MPI_BYTE, status->MPI_SOURCE,
                      status->MPI_TAG, ma->mi.comm, MPI_STATUS_IGNORE);
        if (ret != MPI_SUCCESS && ret != MPI_ERR_TRUNCATE)
            mlog_exit(1, MPI_CRIT, "mpibpoolrecv: MPI_Recv failed %d", ret);
        return(0);
    }
    mlog(MPI_DBG, "recv alloc rqe %p for %d bytes", rqe, count);

    /* receive data into our buffer and queue it for processing */
    rqe->flen = count;
    ret = MPI_Recv(rqe->frame, rqe->flen, MPI_BYTE, status->MPI_SOURCE,
                   status->MPI_TAG, ma->mi.comm, MPI_STATUS_IGNORE);
    if (ret != MPI_SUCCESS) {
        mlog(MPI_CRIT, "MPI_Recv error %d, rqe=%p/%d: drop/release rqe",
             ret, rqe, count);
        mvp_rq_release(ma->a->mq, rqe);
        return(0);
    }
    mlog(MPI_DBG, "MPI_Recv: rqe=%p, success.  queue for fdio", rqe);
    mvp_rq_queue(ma->a->mq, rqe);   /* will notify if needed */
    return(count);
}

/*
 * main routine for the mpi thread.  the main mvpnet thread becomes
 * the mpi thread after init (so we take over managment of the fdio
 * thread from the main thread).
 */
int mpi_main(struct mpi_args *ma) {
    struct fdio_args *a = ma->a;
    int old_fdio_state = FDIO_NONE;    /* inital state */
    int fdio_run, total, ret, pflag, nbins;
    struct mpimtsmgr mts = MPIMTSMGR_INIT;
    struct mvp_stats stats;
    struct sendq_entry *sqe;
    MPI_Request newreq;
    MPI_Status status;
    struct rbp_bininfo *bininfo;

    /*
     * first zero stats and wait for the fdio thread to boot up (or fail to).
     */
    memset(&stats, 0, sizeof(stats));
    mlog(MPI_NOTE, "waiting for local fdio startup");
    pthread_mutex_lock(&a->flk);
    while (old_fdio_state < FDIO_RUN) {
        pthread_cond_wait(&a->fcond, &a->flk);
        if (a->fdio_state == old_fdio_state)
            continue;
        mlog(MPI_INFO, "fdio state %s => %s", fdio_statestr(old_fdio_state),
             fdio_statestr(a->fdio_state));
        old_fdio_state = a->fdio_state;
    }
    pthread_mutex_unlock(&a->flk);

    /*
     * our fdio thread is either running or stopped with an error.
     * now sync up with other ranks to see how their startup went.
     * we continue if all ranks report a successful startup!
     */
    fdio_run = (old_fdio_state == FDIO_RUN) ? 1 : 0;
    mlog(MPI_NOTE, "collecting global fdio startup results");
    ret = MPI_Allreduce(&fdio_run, &total, 1, MPI_INT,
                        MPI_SUM, ma->mi.comm);  /* collective call! */
    if (ret != MPI_SUCCESS) {
        mlog(MPI_CRIT, "MPI_Allreduce failed (%d)", ret);
        goto done;
    }
    if (total < ma->mi.wsize) {
        if (ma->mi.rank == 0)
            mlog(MPI_CRIT, "%d of %d ranks failed to boot - exiting!",
                 ma->mi.wsize - total, ma->mi.wsize);
        ret = 1;
        goto done;
    }
    mlog(MPI_NOTE, "fdio globally running (wsize=%d)", ma->mi.wsize);

    /*
     * allocate state for MPI_Testsome management.   note that mts
     * tracks its own stats.  we will copy them out to stats.ms.* for
     * collection after the fdio thread terminates.
     */
    stats.ms.mts_initsz = (ma->mts_initsz > 0) ? ma->mts_initsz : 16;
    if (mpimts_init(&mts, stats.ms.mts_initsz) != 0) {
        /* slot memory allocation failure */
        mlog(MPI_CRIT, "mpimts_init failed (initsz=%d)", ma->mts_initsz);
        goto done;
    }

    /*
     * we are ready to init and start the MPI networking loop
     */
    while (a->fdio_state == FDIO_RUN) {

        /*
         * send side: check for complete async MPI_Isend ops using mts.
         * we retire all completed ops (no further processing required).
         */
        if (mpimts_test(&mts, ma) != 0) {
            mlog(MPI_ERR, "mpi_main: mpimts_test failed!  exit.");
            goto done;
        }

        /*
         * send side: check for pending sends and start w/MPI_Isend.
         */
        if ((sqe = mvp_sq_dequeue(a->mq)) != NULL) {

            mlog(MPI_DBG, "deque sqe %p: MPI_Isend %d bytes %d->%d",
                 sqe, sqe->flen, ma->mi.rank, sqe->dest);
            ret = MPI_Isend(sqe->frame, sqe->flen, MPI_BYTE, sqe->dest,
                            0, ma->mi.comm, &newreq);

            if (ret != MPI_SUCCESS) {

                mlog(MPI_ERR, "mts sqe Isend-drop: err=%d, sqe=%p, owner=%p",
                     ret, sqe, sqe->owner);
                mpimts_done(ma->a->mq, sqe);

            } else {

                stats.ms.isend_cnt++;
                stats.ms.isend_bytes += sqe->flen;
                if (mpimts_add(&mts, sqe, newreq) == -1) {
                    mlog(MPI_ERR, "mts sqe add-drop: sqe=%p, owner=%p",
                         sqe, sqe->owner);
                    MPI_Request_free(&newreq);
                    mpimts_done(ma->a->mq, sqe);
                } else {
                    /* mpimts_test will release when complete */
                }

            }

        }

        /*
         * recv side: probe to see if data can be received.
         */
        ret = MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, ma->mi.comm,
                         &pflag, &status);
        stats.ms.iprobe_cnt++;

        if (ret != MPI_SUCCESS) {
            mlog(MPI_ERR, "mpi_main: MPI_Iprobe failed %d!  exit.", ret);
            goto done;
        }
        if (pflag) {
            ret = mpibpoolrecv(ma, &status);  /* recv into bpool and queue */
            if (ret > 0) {
                stats.ms.recv_cnt++;
                stats.ms.recv_bytes += ret;
            }
        }

        /*
         * since there's no way to properly block and release
         * the CPU waiting for MPI work to do, we just spin.
         * to minimize the impact of the spin on other threads
         * that might have useful work to do, we put a scheduler
         * yield in the loop.
         */
        sched_yield();
    }

    mlog(MPI_NOTE, "exited main loop, fdio state=%s",
         fdio_statestr(a->fdio_state));

done:
    /*
     * done!
     */
    if (ma->fdio->can_join) {
        mlog(MPI_INFO, "joining fdio pthread");
        (void) mvp_notify_fdio(a->mq, FDIO_NOTE_STOP);
        pthread_join(ma->fdio->pth, NULL);
        mlog(MPI_INFO, "fdio thread join complete!");
        ma->fdio->can_join = 0;
    }

    /* copy mts stats out into stats.ms so it is fully sync'd */
    stats.ms.mts_maxslots = mts.nalloc;
    stats.ms.mts_ntestsome = mts.ntestsome;
    stats.ms.mts_got1 = mts.got1;
    stats.ms.mts_gotmore = mts.gotmore;
    stats.ms.mts_gotmax = mts.gotmax;

    /* copy in the rest of the stats */
    stats.fs = a->fst;    /* structure copy */
    mvp_queuing_stats(a->mq, &stats.qs);
    mvp_queuing_bininfo(a->mq, &nbins, &bininfo);

    /* process the stats */
     mvp_stats_proc(ma, &stats, nbins, bininfo);

    mpimts_finalize(&mts, ma);
    mvp_queuing_finalize(a->mq);
    mlog(MPI_NOTE, "finalize and exit(%d)", ret);
    MPI_Finalize();
    return(ret);
}
