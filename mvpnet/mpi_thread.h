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
 * mpi_thread.h  mpi i/o thread interface
 * 09-May-2025  chuck@ece.cmu.edu
 */
#ifndef MVP_MPI_THREAD_H
#define MVP_MPI_THREAD_H

#include <inttypes.h>
#include <mpi.h>

#include "fdio_thread.h"
#include "mvpnet.h"

/*
 * stats collected by mpi thread
 */
struct mpi_stats {
    int mts_initsz;            /* init slot size (copy from mpi_args) */
    int mts_maxslots;          /* max number of mpi testsome slots */
    uint64_t mts_ntestsome;    /* number of MPI_Testsome() calls */
    uint64_t mts_got1;         /* #times testsome returned 1 entry */
    uint64_t mts_gotmore;      /* #times testsome return >1 entry */
    uint64_t mts_gotmax;       /* max# entries testsome returned */
    uint64_t isend_cnt;        /* number of MPI_Isend calls */
    uint64_t isend_bytes;      /* number of bytes send with MPI_Isend */
    uint64_t iprobe_cnt;       /* number of MPI_Iprobe calls */
    uint64_t recv_cnt;         /* number of MPI_Recv calls */
    uint64_t recv_bytes;       /* number of bytes recv'd with MPI_Recv */
};

/*
 * startup args for mpi main thread
 */
struct mpi_args {
    /* our config */
    struct mpiinfo mi;         /* copy of mpi info for this proc */
    int mts_initsz;            /* init# of slots to allocate for testsome */
    char *gstats_log;          /* gstats log file, if enabled (rank0 only) */

    /* we take over ownership of fdio */
    struct runthread *fdio;    /* handle for fdio thread */
    struct fdio_args *a;       /* args we sent to fdio thread */
};

int mpi_main(struct mpi_args *ma);

#endif /* MVP_MPI_THREAD_H */
