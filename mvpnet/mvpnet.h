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
 * mvpnet.c  interfaces shared across mvpnet modules
 * 14-Apr-2025  chuck@ece.cmu.edu
 */
#ifndef MVP_MVPNET_H
#define MVP_MVPNET_H

#include <pthread.h>

#include <mpi.h>

#include "utilfns.h"       /* for strvec */

/*
 * structure to store mvpnet command line options
 */
struct mvpopts {
    int bufsz_ml;          /* mlog message buffer size (bytes) */
    char *cloudinit;       /* cloud-init info (image file or dir) */
    int conlog;            /* enable console log file? */
    char *defpri_ml;       /* default mlog priority */
    struct strvec domain;  /* domain(s) for resolver */
    int gstats;            /* do global stat dump at rank 0 */
    struct strvec image;   /* images to load */
    char *jobname;         /* batch job name/id (if any) */
    int kvm;               /* use kvm when running qemu */
    char *logdir;          /* directory put log files */
    int logfile_ml;        /* enable mlog log file */
    int mem_mb;            /* #mb of memory for guest VM */
    char *monwrap;         /* monwrap program */
    int nettype;           /* type of local socket to use for qemu network */
    char *processor;       /* processor type to use w/qemu */
    char *qemucmd;         /* qemu command */
    char *rundir;          /* runtime directory to copy image to */
    char *sockdir;         /* directory to put sockets in */
    int sshprobe_timeout;  /* ssh probe timeout (secs) */
    char *stderrpri_ml;    /* stderr mlog priority */
    char *tftpdir;         /* tftp dir (enables tftpd) */
    char *username;        /* username to login to on guest */
    int wraplog;           /* enable monwrap log file */
    char *xmask_ml;        /* mlog mask */
};

/* non-NULL defaults */
#define MVPOPTS_INIT                                                           \
    (struct mvpopts) {                                                         \
        .bufsz_ml = 4096, .defpri_ml = "WARN", .domain = STRVEC_INIT,          \
        .image = STRVEC_INIT, .kvm = 1, .logdir = "/tmp", .mem_mb = 4096,      \
        .monwrap = "monwrap", .nettype = SOCK_STREAM,                          \
        .processor = "host", .qemucmd = "qemu-system-x86_64",                  \
        .sockdir = "/tmp", .sshprobe_timeout = 90, .stderrpri_ml = "CRIT"      \
    }

/* struct to track if a pthread is running */
struct runthread {
    int can_join;            /* pth is valid, can be joined */
    pthread_t pth;           /* the thread */
};

/* mpi info for our proc */
struct mpiinfo {
    int rank;                /* my rank */
    int wsize;               /* world size */
    int maxlocalsize;        /* mpi max local size (if > 0) */
    MPI_Comm comm;           /* our top-level comm */
};

#endif /* MVP_MVPNET_H */
