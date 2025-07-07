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
 * mvp_stats.h  mvpnet stats
 * 10-Jun-2025  chuck@ece.cmu.edu
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mpi.h>

#include "mvp_mlog.h"
#include "mvp_stats.h"


/*
 * total/min/max (tmm) information for a stat.   a simple function
 * for rank 0 to consolidate stat data it has collected from all
 * the ranks with MPI_Gather().
 */
struct tmminfo {
    uint64_t total;
    uint64_t minval;
    int minrank;
    uint64_t maxval;
    int maxrank;
};

/*
 * compute total/min/max across an array of records.  we handle
 * int and uint64_t for input and return the result in uint64_t.
 * base is the base of the array, recsize is the size of a record
 * (e.g. sizeof(struct mvp_stats)), rcount is the number of records
 * (e.g. the mpi world size), offset is the offset (in bytes) of
 * the value of interest, and is64 sets the type (0=int,!0=uint64_t).
 * result comes back in tmi.
 */
static void tmm(void *base, int recsize, int rcount, int offset, int is64,
                struct tmminfo *tmi) {
    char *cur = base;
    int lcv;
    uint64_t val;

    tmi->total = 0;
    tmi->minrank = tmi->maxrank = -1;

    for (lcv = 0 ; lcv < rcount ; lcv++, cur += recsize) {
        if (is64)
            val = *((uint64_t *)&cur[offset]);
        else
            val = *((int *)&cur[offset]);
        tmi->total += val;
        if (tmi->minrank == -1 || val < tmi->minval) {
            tmi->minrank = lcv;
            tmi->minval = val;
        }
        if (tmi->maxrank == -1 || val > tmi->maxval) {
            tmi->maxrank = lcv;
            tmi->maxval = val;
        }
    }
}

/*
 * add stats to the local log
 */
static void log_stats(struct mpi_args *ma, struct mvp_stats *sts, int nbins,
                      struct rbp_bininfo *bininfo) {
    struct fdio_stats *f = &sts->fs;
    struct mpi_stats *m = &sts->ms;
    struct que_stats *q = &sts->qs;
    int lcv;

    mlog(FDIO_INFO, "qemusend (cnt/bytes/blocked): %d %" PRIu64 " %d",
         f->qemusend_cnt, f->qemusend_bytes, f->qemusend_blocked);
    mlog(FDIO_INFO, "runts=%d", f->runt_cnt);
    mlog(FDIO_INFO, "unicast (cnt/bytes/bad-dst): %d %" PRIu64 " %d",
         f->unicast_cnt, f->unicast_bytes, f->unicast_baddst);
    mlog(FDIO_INFO, "arpreq (cnt/badreq): %d %d", f->arpreq_cnt,
         f->arpreq_badreq);
    mlog(FDIO_INFO, "bcast-start (cnt/bytes): %d %" PRIu64, f->bcast_st_cnt,
         f->bcast_st_bytes);
    mlog(FDIO_INFO, "stdin (cnt/bytes): %d %" PRIu64 ", console (cnt/bytes): "
                    "%d %" PRIu64, f->stdin_cnt, f->stdin_bytes,
                    f->console_cnt, f->console_bytes);
    mlog(FDIO_INFO, "note-queue: %d, accept: %d", f->notequeue_cnt,
         f->accept_cnt);
    mlog(FDIO_INFO, "bcast-in (cnt/bytes/badsrc): %d %" PRIu64 " %d",
         f->bcastin_cnt, f->bcastin_bytes, f->bcastin_badsrc);

    mlog(MPI_INFO, "testsome (initslots/maxslots/calls): "
         "%d %d %" PRIu64, m->mts_initsz, m->mts_maxslots, m->mts_ntestsome);
    mlog(MPI_INFO, "testsome got (1/>1/max): %" PRIu64 
                   " %" PRIu64 " %" PRIu64, m->mts_got1, m->mts_gotmore,
                   m->mts_gotmax);
    mlog(MPI_INFO, "isend (cnt/bytes): %" PRIu64 " %" PRIu64, m->isend_cnt,
         m->isend_bytes);
    mlog(MPI_INFO, "iprobe (calls): %" PRIu64, m->iprobe_cnt);
    mlog(MPI_INFO, "recv (cnt/bytes): %" PRIu64 " %" PRIu64, m->recv_cnt,
         m->recv_bytes);

    mlog(QUE_INFO, "fb-main: maxfbuf=%d",q->fbsmain.maxfbufs);
    mlog(QUE_INFO, "fb-main: retire (empty/compact/loaned): %d %d %d",
         q->fbsmain.retire_nol_empty, q->fbsmain.retire_nol_compact, 
         q->fbsmain.retire_loaned);
    mlog(QUE_INFO, "fb-main: sockrcv (cnt/nodata/bytes): %d %d %" PRIu64,
         q->fbsmain.sockrcv_cnt, q->fbsmain.sockrcv_nodata,
         q->fbsmain.sockrcv_bytes);
    mlog(QUE_INFO, "fb-main: advance (no/some/all): %d %d %d",
         q->fbsmain.advance_no, q->fbsmain.advance_some,
         q->fbsmain.advance_all);

    mlog(QUE_INFO, "fb-bcast: maxfbuf=%d",q->fbsbcast.maxfbufs);
    mlog(QUE_INFO, "fb-bcast: retire (empty/compact/loaned): %d %d %d",
         q->fbsbcast.retire_nol_empty, q->fbsbcast.retire_nol_compact, 
         q->fbsbcast.retire_loaned);
    mlog(QUE_INFO, "fb-bcast: sockrcv (cnt/nodata/bytes): %d %d %" PRIu64,
         q->fbsbcast.sockrcv_cnt, q->fbsbcast.sockrcv_nodata,
         q->fbsbcast.sockrcv_bytes);
    mlog(QUE_INFO, "fb-bcast: advance (no/some/all): %d %d %d",
         q->fbsbcast.advance_no, q->fbsbcast.advance_some,
         q->fbsbcast.advance_all);

    mlog(QUE_INFO, "send-queue-entry (n, maxqlen): %d %d",
        q->nsqe, q->mpimaxsqlen);
    mlog(QUE_INFO, "recv-queue-entry (n, maxqlen): %d %d",
        q->nrqe, q->mpimaxrqlen);

    for (lcv = 0 ; lcv < nbins ; lcv++) {
        if (bininfo[lcv].cnt == 0)
            continue;
        mlog(QUE_INFO, "recv-bin[%d] (size/count): %d %d", lcv,
             bininfo[lcv].size, bininfo[lcv].cnt);
    }
}

/*
 * write global stats log file (on rank 0) from data rank 0 collected
 * with MPI_Gather().
 *
 * XXX: to be safe, write it as key/value labled texted rather than
 * doing a binary dump.  consider revising format later if needed.
 */
static void log_gstats(struct mpi_args *ma, struct mvp_stats *asts, int nbins,
                       struct rbp_bininfo *abins) {
    FILE *g;
    time_t now = time(0);
    int r, lcv;

    if ((g = fopen(ma->gstats_log, "a")) == NULL) {
        mlog(MVP_CRIT, "skipping gstats, log open err: %s", ma->gstats_log);
        return;
    }

    fprintf(g, "BEGIN gstats %s", ctime(&now));
    for (r = 0 ; r < ma->mi.wsize ; r++) {
        struct fdio_stats *f = &asts[r].fs;
        struct mpi_stats *m = &asts[r].ms;
        struct que_stats *q = &asts[r].qs;
        struct rbp_bininfo *bininfo = &abins[r*nbins];

        fprintf(g, "%d.qemusend.cnt=%d\n", r, f->qemusend_cnt); 
        fprintf(g, "%d.qemusend.bytes=%" PRIu64 "\n", r, f->qemusend_bytes); 
        fprintf(g, "%d.qemusend.blocked=%d\n", r, f->qemusend_blocked); 
        fprintf(g, "%d.runts=%d\n", r, f->runt_cnt); 
        fprintf(g, "%d.unicast.cnt=%d\n", r, f->unicast_cnt); 
        fprintf(g, "%d.unicast.bytes=%" PRIu64 "\n", r, f->unicast_bytes); 
        fprintf(g, "%d.unicast.baddst=%d\n", r, f->unicast_baddst); 
        fprintf(g, "%d.arpreq.cnt=%d\n", r, f->arpreq_cnt); 
        fprintf(g, "%d.arpreq.badreq=%d\n", r, f->arpreq_badreq); 
        fprintf(g, "%d.bcaststart.cnt=%d\n", r, f->bcast_st_cnt); 
        fprintf(g, "%d.bcaststart.bytes=%" PRIu64 "\n", r, f->bcast_st_bytes); 
        fprintf(g, "%d.stdin.cnt=%d\n", r, f->stdin_cnt); 
        fprintf(g, "%d.stdin.bytes=%" PRIu64 "\n", r, f->stdin_bytes); 
        fprintf(g, "%d.console.cnt=%d\n", r, f->console_cnt); 
        fprintf(g, "%d.console.bytes=%" PRIu64 "\n", r, f->console_bytes); 
        fprintf(g, "%d.notequeue.cnt: %d\n", r, f->notequeue_cnt);
        fprintf(g, "%d.accept.cnt: %d\n", r, f->accept_cnt);
        fprintf(g, "%d.bcastin.cnt=%d\n", r, f->bcastin_cnt); 
        fprintf(g, "%d.bcastin.bytes=%" PRIu64 "\n", r, f->bcastin_bytes); 
        fprintf(g, "%d.bcastin.badsrc=%d\n", r, f->bcastin_badsrc); 
        fprintf(g, "%d.testsome.initslots=%d\n", r, m->mts_initsz); 
        fprintf(g, "%d.testsome.maxslots=%d\n", r, m->mts_maxslots); 
        fprintf(g, "%d.testsome.cnt=%" PRIu64 "\n", r, m->mts_ntestsome); 
        fprintf(g, "%d.testsome.got1=%" PRIu64 "\n", r, m->mts_got1); 
        fprintf(g, "%d.testsome.gotmore=%" PRIu64 "\n", r, m->mts_gotmore); 
        fprintf(g, "%d.testsome.gotmax=%" PRIu64 "\n", r, m->mts_gotmax); 
        fprintf(g, "%d.isend.cnt=%" PRIu64 "\n", r, m->isend_cnt); 
        fprintf(g, "%d.isend.bytes=%" PRIu64 "\n", r, m->isend_bytes); 
        fprintf(g, "%d.iprobe.cnt=%" PRIu64 "\n", r, m->iprobe_cnt); 
        fprintf(g, "%d.recv.cnt=%" PRIu64 "\n", r, m->recv_cnt); 
        fprintf(g, "%d.recv.bytes=%" PRIu64 "\n", r, m->recv_bytes); 
        fprintf(g, "%d.fbmain.maxfbuf=%d\n", r, q->fbsmain.maxfbufs);
        fprintf(g, "%d.fbmain.retire.empty=%d\n", r,
                q->fbsmain.retire_nol_empty);
        fprintf(g, "%d.fbmain.retire.compact=%d\n", r,
                q->fbsmain.retire_nol_compact);
        fprintf(g, "%d.fbmain.retire.loaned=%d\n", r, q->fbsmain.retire_loaned);
        fprintf(g, "%d.fbmain.sockrcv.cnt=%d\n", r, q->fbsmain.sockrcv_cnt);
        fprintf(g, "%d.fbmain.sockrcv.nodata=%d\n", r,
                q->fbsmain.sockrcv_nodata);
        fprintf(g, "%d.fbmain.sockrcv.bytes=%" PRId64 "\n", r,
                q->fbsmain.sockrcv_bytes);
        fprintf(g, "%d.fbmain.advance.no=%d\n", r, q->fbsmain.advance_no);
        fprintf(g, "%d.fbmain.advance.some=%d\n", r, q->fbsmain.advance_some);
        fprintf(g, "%d.fbmain.advance.all=%d\n", r, q->fbsmain.advance_all);
        fprintf(g, "%d.fbbcast.maxfbuf=%d\n", r, q->fbsbcast.maxfbufs);
        fprintf(g, "%d.fbbcast.retire.empty=%d\n", r,
                q->fbsbcast.retire_nol_empty);
        fprintf(g, "%d.fbbcast.retire.compact=%d\n", r,
                q->fbsbcast.retire_nol_compact);
        fprintf(g, "%d.fbbcast.retire.loaned=%d\n", r,
                q->fbsbcast.retire_loaned);
        fprintf(g, "%d.fbbcast.sockrcv.cnt=%d\n", r, q->fbsbcast.sockrcv_cnt);
        fprintf(g, "%d.fbbcast.sockrcv.nodata=%d\n", r,
                q->fbsbcast.sockrcv_nodata);
        fprintf(g, "%d.fbbcast.sockrcv.bytes=%" PRId64 "\n", r,
                q->fbsbcast.sockrcv_bytes);
        fprintf(g, "%d.fbbcast.advance.no=%d\n", r, q->fbsbcast.advance_no);
        fprintf(g, "%d.fbbcast.advance.some=%d\n", r, q->fbsbcast.advance_some);
        fprintf(g, "%d.fbbcast.advance.all=%d\n", r, q->fbsbcast.advance_all);
        fprintf(g, "%d.sqentry.n=%d\n", r, q->nsqe);
        fprintf(g, "%d.sqentry.maxqlen=%d\n", r, q->mpimaxsqlen);
        fprintf(g, "%d.rqentry.n=%d\n", r, q->nrqe);
        fprintf(g, "%d.rqentry.maxqlen=%d\n", r, q->mpimaxrqlen);
        for (lcv = 0 ; lcv < nbins ; lcv++) {
            if (bininfo[lcv].cnt == 0)
                continue;
            fprintf(g, "%d.recvbin.%d.size=%d\n", r, lcv, bininfo[lcv].size);
            fprintf(g, "%d.recvbin.%d.cnt=%d\n", r, lcv, bininfo[lcv].cnt);
        }
    }
    
    fprintf(g, "END gstats %s", ctime(&now));
    fclose(g);
}

/*
 * produce short log report at rank 0 at exit time.
 */
static void log_report(int rcount, struct mvp_stats *asts,
                  struct rbp_bininfo *abins, int nbins) {
    int statsz = sizeof(struct mvp_stats);
    struct tmminfo tmi[2];

#define LL (MVP_NOTE|MLOG_STDOUT)
#define roff(S,F) ( ((char *)(&(S)->F)) - ((char *)(S)) )

    mlog(LL, "shutdown report");
    memset(tmi, 0, sizeof(tmi));
    tmm(asts, statsz, rcount, roff(asts,qs.mpimaxsqlen), 0, &tmi[0]);
    mlog(LL, "send-q: avg maxlen=%.1lf entries (min=r%d@%d, max=r%d@%d)",
         tmi[0].total / (double) rcount, tmi[0].minrank, (int)tmi[0].minval,
         tmi[0].maxrank, (int)tmi[0].maxval);
    tmm(asts, statsz, rcount, roff(asts,qs.mpimaxrqlen), 0, &tmi[0]);
    mlog(LL, "recv-q: avg maxlen=%.1lf entries (min=r%d@%d, max=r%d@%d)",
         tmi[0].total / (double) rcount, tmi[0].minrank, (int)tmi[0].minval,
         tmi[0].maxrank, (int)tmi[0].maxval);
    tmm(asts, statsz, rcount, roff(asts,qs.fbsmain.maxfbufs), 0, &tmi[0]);
    mlog(LL, "fbuf-main: avg-max=%.1lf entries (min=r%d@%d, max=r%d@%d)",
         tmi[0].total / (double) rcount, tmi[0].minrank, (int)tmi[0].minval,
         tmi[0].maxrank, (int)tmi[0].maxval);
    tmm(asts, statsz, rcount, roff(asts,qs.fbsbcast.maxfbufs), 0, &tmi[0]);
    mlog(LL, "fbuf-bcast: avg-max=%.1lf entries (min=r%d@%d, max=r%d@%d)",
         tmi[0].total / (double) rcount, tmi[0].minrank, (int)tmi[0].minval,
         tmi[0].maxrank, (int)tmi[0].maxval);
    tmm(asts, statsz, rcount, roff(asts,ms.mts_maxslots), 0, &tmi[0]);
    mlog(LL, "MPI_testsome: avg-max=%.1lf slots (min=r%d@%d, max=r%d@%d)",
         tmi[0].total / (double) rcount, tmi[0].minrank, (int)tmi[0].minval,
         tmi[0].maxrank, (int)tmi[0].maxval);
    tmm(asts, statsz, rcount, roff(asts,ms.isend_cnt), 1, &tmi[0]);
    tmm(asts, statsz, rcount, roff(asts,ms.isend_bytes), 1, &tmi[1]);
    mlog(LL, "MPI_Isend: total=%" PRIu64 " ops, %" PRIu64 " bytes",
         tmi[0].total, tmi[1].total);
    tmm(asts, statsz, rcount, roff(asts,ms.recv_cnt), 1, &tmi[0]);
    tmm(asts, statsz, rcount, roff(asts,ms.recv_bytes), 1, &tmi[1]);
    mlog(LL, "MPI_Recv: total=%" PRIu64 " ops, %" PRIu64 " bytes",
         tmi[0].total, tmi[1].total);
    tmm(asts, statsz, rcount, roff(asts,fs.runt_cnt), 0, &tmi[0]);
    if (tmi[0].total) {
        mlog(LL, "runts: total=%" PRIu64 " runts (min=r%d@%d, max=r%d@%d)",
             tmi[0].total, tmi[0].minrank, (int)tmi[0].minval,
             tmi[0].maxrank, (int)tmi[0].maxval);
    }
    tmm(asts, statsz, rcount, roff(asts,fs.unicast_baddst), 0, &tmi[0]);
    if (tmi[0].total) {
        mlog(LL, "unicast: baddst=%" PRIu64 " pkts (min=r%d@%d, max=r%d@%d)",
             tmi[0].total, tmi[0].minrank, (int)tmi[0].minval,
             tmi[0].maxrank, (int)tmi[0].maxval);
    }
    tmm(asts, statsz, rcount, roff(asts,fs.bcastin_badsrc), 0, &tmi[0]);
    if (tmi[0].total) {
        mlog(LL, "broadcast: badsrc=%" PRIu64 " pkts (min=r%d@%d, max=r%d@%d)",
             tmi[0].total, tmi[0].minrank, (int)tmi[0].minval,
             tmi[0].maxrank, (int)tmi[0].maxval);
    }
    tmm(asts, statsz, rcount, roff(asts,fs.accept_cnt), 0, &tmi[0]);
    if (tmi[0].total > rcount) {
        mlog(LL, "accept: %" PRIu64 " ops (min=r%d@%d, max=r%d@%d)",
             tmi[0].total, tmi[0].minrank, (int)tmi[0].minval,
             tmi[0].maxrank, (int)tmi[0].maxval);
    }
    tmm(asts, statsz, rcount, roff(asts,fs.stdin_cnt), 0, &tmi[0]);
    if (tmi[0].total > rcount) {
        mlog(LL, "stdin: %" PRIu64 " readcnt (min=r%d@%d, max=r%d@%d)",
             tmi[0].total, tmi[0].minrank, (int)tmi[0].minval,
             tmi[0].maxrank, (int)tmi[0].maxval);
    }
    tmm(asts, statsz, rcount, roff(asts,fs.console_bytes), 1, &tmi[0]);
    mlog(LL, "consoleout: %.1lf avg-bytes (min=r%d@%" PRIu64 
             ", max=r%d@%" PRIu64 ")",
         tmi[0].total / (double) rcount, tmi[0].minrank, tmi[0].minval,
         tmi[0].maxrank, tmi[0].maxval);


#undef LL
#undef roff
}

/*
 * process mvp stats.   this is a collective call.
 */
void mvp_stats_proc(struct mpi_args *ma, struct mvp_stats *sts, int nbins,
                    struct rbp_bininfo *bininfo) {
    int ret;
    struct mvp_stats *asts = NULL;
    struct rbp_bininfo *abins = NULL;

    log_stats(ma, sts, nbins, bininfo);   /* log stats locally first */

    if (ma->mi.rank == 0) {
        asts = malloc(sizeof(asts[0]) * ma->mi.wsize);
        abins = malloc(sizeof(abins[0]) * nbins * ma->mi.wsize);
        if (asts == NULL || abins == NULL)
            mlog_abort(MVP_CRIT, "stats malloc on rank 0 failed");
    }

    mlog(MVP_DBG, "mvp_stats_proc: MPI gathering stats at rank 0");
    ret = MPI_Gather(sts, sizeof(*sts), MPI_BYTE,
                     asts, sizeof(*asts), MPI_BYTE, 0, ma->mi.comm);
    if (ret != MPI_SUCCESS)
        mlog_exit(1, MVP_CRIT, "MPI_Gather stats failed %d", ret);

    ret = MPI_Gather(bininfo, sizeof(bininfo[0])*nbins, MPI_BYTE,
                     abins, sizeof(abins[0])*nbins, MPI_BYTE, 0, ma->mi.comm);
    if (ret != MPI_SUCCESS)
        mlog_exit(1, MVP_CRIT, "MPI_Gather bins failed %d", ret);

    if (ma->mi.rank != 0)   /* rank 0 has all the data and will report */
        return;

    /* write gstats_log if enabled */
    if (ma->gstats_log)
        log_gstats(ma, asts, nbins, abins);

    log_report(ma->mi.wsize, asts, abins, nbins);

    free(asts);
    free(abins);
}
