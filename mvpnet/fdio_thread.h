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
 * fdio_thread.h  file descriptor i/o thread interface
 * 29-Apr-2025  chuck@ece.cmu.edu
 */
#ifndef MVP_FDIO_THREAD_H
#define MVP_FDIO_THREAD_H

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>

#include "utilfns.h"

#include "mvp_queuing.h"
#include "mvpnet.h"

/* fdio thread states */
#define FDIO_NONE         0  /* fdio thread has not been started */
#define FDIO_PREP         1  /* fdio preparing to start qemu */
#define FDIO_BOOT_QN      2  /* booting, waiting on qemu and network */
#define FDIO_BOOT_Q       3  /* booting, waiting on qemu (net up!) */
#define FDIO_BOOT_N       4  /* booting, waiting on network (qemu up!) */
#define FDIO_RUN          5  /* qemu is up and running */
/* XXX: states about user app script? */
#define FDIO_DONE         6  /* qemu (and fdio) is done, ok to join thread */
#define FDIO_ERROR        7  /* fdio got an error, ok to join */

/* fdio thread notifications */
#define FDIO_NOTE_STOP    1  /* ask fdio thread to stop */
#define FDIO_NOTE_SSHD    2  /* running sshd detected */
#define FDIO_NOTE_NOSSHD  3  /* sshd failed to start before timeout */
#define FDIO_NOTE_QUEUE   4  /* check MPI queue for input data */
#define FDIO_NOTE_DRAINED 5  /* mpisendq drain complete */

/*
 * stats collected by fdio thread
 */
struct fdio_stats {
    int qemusend_cnt;        /* number of frames sent to qemu socket */
    uint64_t qemusend_bytes; /* number of bytes sent to qemu socket */
    int qemusend_blocked;    /* number of times qemu socket blocked */
    int runt_cnt;            /* number of runt frames from qemu socket */
    int unicast_baddst;      /* number of unicast w/bad dst from qemu */
    int unicast_cnt;         /* number of unicast frames from qemu */
    uint64_t unicast_bytes;  /* number of unicast bytes from qemu */
    int arpreq_badreq;       /* number of discarded bad arp requests */
    int arpreq_cnt;          /* number of good arp requests processed */
    int bcast_st_cnt;        /* number of bcasts our qemu started */
    uint64_t bcast_st_bytes; /* nubmer of bcast start bytes our qemu sent */
    int stdin_cnt;           /* number of times we got stdin (mpi) input */
    uint64_t stdin_bytes;    /* number of bytes we got on stdin */
    int console_cnt;         /* number of times we got qemu console output */
    uint64_t console_bytes;  /* number of qemu console bytes we got */
    int notequeue_cnt;       /* number of QUEUE notes we got */
    int accept_cnt;          /* number of accepts we processed */
    int bcastin_cnt;         /* number of bcasts we received from MPI */
    uint64_t bcastin_bytes;  /* number of bcasts bytes from MPI */
    int bcastin_badsrc;      /* number of bcasts from MPI with bad src rank */
};

/*
 * startup args for fdio main thread
 */
struct fdio_args {
    /* config data that is constant throughout the life of fdio_thread */
    struct strvec *qvec;     /* qemu command line for exec */
    struct mpiinfo mi;       /* copy of mpi info for this proc */
    int localsshport;        /* local port to reach guest sshd on */
    int sshprobe_timeout;    /* timeout for ssh probe (secs) */
    int nettype;             /* network type (SOCK_STREAM or SOCK_DGRAM) */
    char **socknames;        /* unix domain socket filenames */
    int *sockfds;            /* socket file descriptors */
    FILE *confp;             /* if not NULL, print console output here */

    /* mvp queuing area (w/notify pipe) ... shared with MPI thread */
    struct mvp_queuing *mq;  /* see mvp_queuing for locking info */

    /* fdio state, only fdio can change it (others can wait for a change) */
    pthread_mutex_t flk;     /* lock on fdio_state */
    pthread_cond_t fcond;    /* block here to wait for fdio state change */
    int fdio_state;          /* fdio thread state (single writer) */

    /* fdio stats - only fdio thread writes here */
    struct fdio_stats fst;   /* fdio stats */
#if 0
    // need user application script name
#endif
};

/* non-NULL defaults for fdio_args */
#define FDIO_ARGS_INIT                                                         \
    (struct fdio_args) {                                                       \
        .flk = PTHREAD_MUTEX_INITIALIZER, .fcond = PTHREAD_COND_INITIALIZER,   \
        .fdio_state = FDIO_NONE, .nettype = -1,                                \
    }

void *fdio_main(void *arg);
char *fdio_statestr(int state);

#endif /* MVP_FDIO_THREAD_H */
