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
 * mvpnet.c  run qemu-based VM in MPI-based virtual private network
 * 14-Apr-2025  chuck@ece.cmu.edu
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/signal.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <mpi.h>

#define MKMLOGHDR_DEF_FNAMES   /* to configure mlog */

#include "fdio_thread.h"
#include "mpi_thread.h"
#include "mvp_mlog.h"
#include "mvp_queuing.h"
#include "mvpnet.h"
#include "qemucli.h"
#include "utilfns.h"

/*
 * global info (e.g. for atexit()).
 */
struct mvpnet_global {
    pid_t mainpid;       /* pid of main process */
    struct strvec tmps;  /* temporary files to remove at exit */
};

struct mvpnet_global g = { .mainpid = -1, .tmps = STRVEC_INIT };

/*
 * atexit handler
 */
static void atexit_handler() {
    if (getpid() != g.mainpid)    /* only the mainpid does cleanup */
        return;
    if (g.tmps.base) {            /* remove temporary files */
        for (int lcv = 0 ; lcv < g.tmps.nused - 1 ; lcv++) {
            mlog(MVP_INFO, "atexit unlink(%s)", g.tmps.base[lcv]);
            unlink(g.tmps.base[lcv]);
        }
    }
}

/*
 * rank/match error macro (variadic macro).  only have rank 0 print
 * the error if possible.
 */
#define rmerror(RNK,MTCH,...) do {                                             \
    if ((RNK) == 0 || (MTCH)) {                                                \
        if ((MTCH)) fprintf(stderr, "[%d] ", (RNK));                           \
        errx(1, __VA_ARGS__);                                                  \
    }                                                                          \
    exit(1);                                                                   \
} while (0)

/*
 * is local tcp port currently available for qemu sshd forwarding?
 * best effort only.  another proc can grab it before we start qemu.
 * ret 0 if available, -1 if not.
 */
static int ltcp_port_available(int tryport) {
    int s, rv;
    struct sockaddr_in sin;

    rv = -1;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(tryport);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    s = socket(PF_INET, SOCK_STREAM, 0);
    if (s != -1) {
        if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == -1 &&
            errno == ECONNREFUSED)
            rv = 0;
        close(s);
    }
    return(rv);
}

/*
 * return the max number of MPI ranks running on a comm in the job.
 * (i.e. the max local size for that comm.)   this is a collective
 * call on the given comm (typically MPI_COMM_WORLD).  ret -1 on error.
 */
static int mpimaxlocalsize(MPI_Comm comm) {
    MPI_Comm localcomm;
    int my_local_size, maxlocal;

    /* create a local-only comm, get its size, and free it */
    if (MPI_Comm_split_type(comm, MPI_COMM_TYPE_SHARED, 0,
                            MPI_INFO_NULL, &localcomm) != MPI_SUCCESS)
        return(-1);
    if (MPI_Comm_size(localcomm, &my_local_size) != MPI_SUCCESS)
        return(-1);
    MPI_Comm_free(&localcomm);   /* don't care if it fails */

    /* use collective allreduce to get max of localsize across comm */
    if (MPI_Allreduce(&my_local_size, &maxlocal, 1, MPI_INT,
                       MPI_MAX, comm) != MPI_SUCCESS)
        return(-1);

    return(maxlocal);
}

/*
 * check string going into qemu command line to make sure does not
 * have any unusual chars in it that might confuse the qmeu command
 * line parser (or us!).   ret 0 if ok, -1 if not ok.
 */
static int stringcheck(char *in, int xtra) {
    for (char *cp = in ; *cp ; cp++) {
        if (isalnum(*cp) || *cp == '.' || *cp == '-' || *cp == '_')
            continue;
        if (xtra && *cp == xtra)
            continue;
        return(-1);
    }
    return(0);
}

/*
 * usage msg
 */
void usage(char *prog, int rank) {
    struct mvpopts defs = MVPOPTS_INIT;

    /* avoid output jumble by only having rank 0 print usage */
    if (rank != 0)
        exit(1);

    fprintf(stderr, "usage: %s [flags] application-script\n", prog);
    fprintf(stderr, "flags are:\n");
    fprintf(stderr, "\t-B [sz]    *mlog msgbuf size (bytes, def=%d)\n",
            defs.bufsz_ml);
    fprintf(stderr, "\t-c [val]   *console log on (1) or off (0) (def=%d)\n",
            defs.conlog);
    fprintf(stderr, "\t-d [dom]    domain (def=load from resolv.conf)\n");
    fprintf(stderr, "\t-D [pri]   *mlog default priority (def=%s)\n",
            defs.defpri_ml);
    fprintf(stderr, "\t-g          have rank0 dump global stats to file\n");
    fprintf(stderr, "\t-i [img]   *image spec to load\n");
    fprintf(stderr, "\t-j [id]     job id/name (added to log/socket names)\n");
    fprintf(stderr, "\t-k [val]    kvm on (1) or off (0) (def=%d)\n", defs.kvm);
    fprintf(stderr, "\t-l [dir]    log file directory (def=%s)\n", defs.logdir);
    fprintf(stderr, "\t-L [val]   *mpv mlog on (1) or off (0) (def=%d)\n",
            defs.logfile_ml);
    fprintf(stderr, "\t-m [mb]    *guest VM memory size in mb (def=%d)\n",
            defs.mem_mb);
    fprintf(stderr, "\t-M [wrap]   monwrap prog name (def=%s)\n", defs.monwrap);
    fprintf(stderr, "\t-n [nett]   net type (stream or dgram) (def=%s)\n",
            (defs.nettype == SOCK_STREAM) ? "stream" : "dgram");
    fprintf(stderr, "\t-q [qemu]   qemu command (def=%s)\n", defs.qemucmd);
    fprintf(stderr, "\t-r [dir]    run dir to copy image to (def=none)\n");
    fprintf(stderr, "\t-s [dir]    socket directory (def=%s)\n", defs.sockdir);
    fprintf(stderr, "\t-S [pri]    mlog stderr priority (def=%s)\n",
            defs.stderrpri_ml);
    fprintf(stderr, "\t-t [dir]    tftp dir (enables tftpd)\n");
    fprintf(stderr, "\t-u [usr]    username on guest (for ssh)\n");
    fprintf(stderr, "\t-w [val]   *wrapper log on (1) or off (0) (def=%d)\n",
            defs.wraplog);
    fprintf(stderr, "\t-X [mask]  *mlog mask to set after defaults\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "note: options marked with '*' can be prefixed with\n");
    fprintf(stderr, "      a rank spec (e.g. '-D 0,4-9:ERR') to limit\n");
    fprintf(stderr, "      the ranks the option is applied to\n");
    fprintf(stderr, "note: by default we run '-i' images directly\n");
    fprintf(stderr, "      unless '-r' is set.  if '-r' is set we copy\n");
    fprintf(stderr, "      images to rundir and run from there.\n");
    fprintf(stderr, "note: use additional '-i' flags to add more images\n");
    fprintf(stderr, "note: use additional '-d' flags to add more domains\n");
    fprintf(stderr, "note: '-i' uses qemu '-drive' cmd format but takes an\n");
    fprintf(stderr, "      an additional mvpctl= set of flag values:\n");
    fprintf(stderr, "\tc - copy image to rundir (-r must be set)\n");
    fprintf(stderr, "\tj - add job to filename in rundir\n");
    fprintf(stderr, "\tr - add rank to filename in rundir\n");
    fprintf(stderr, "\td - remove image from rundir at exit\n");
    fprintf(stderr, "\tJ - add job to image filename (-i)\n");
    fprintf(stderr, "\tR - add rank to image filename (-i)\n");
    fprintf(stderr, "\tD - remove image (-i) at exit\n");
    fprintf(stderr, "\tp - write-protect image file (read-only=on)\n");
    fprintf(stderr, "\ts - open file in snapshot mode (snapshot=on)\n");
    fprintf(stderr, "note: default mvpctl is 'cjrd' if '-r' is set\n");
    fprintf(stderr, "       otherwise the default is '' if '-r' is not set\n");
    fprintf(stderr, "example: '-i foo.img,mvpctl=s' run direct w/snapshot\n");
    exit(1);
}

/*
 * main routine
 */
int main(int argc, char **argv) {
    char *prog = argv[0];
    char *cp, *optrest;
    struct mpiinfo mii;
    int ch, match, lcv;
    struct mvpopts mopt = MVPOPTS_INIT;

    /* tags, filenames, and command line related stuff */
    char *jobtag, ranktag[32], *mlog_log, *console_log, *gstats_log,
         *wrapper_log;
    char *socknames[2];
    int stride, localport;
    struct qemucli_args qcliargs = { 0 };
    struct strvec qemuvec = STRVEC_INIT;  /* qemu execp() args */

    /* resources we use for open/allocate/fork/create */
    FILE *conlog_fp = NULL;
    int sockfds[2], ret;
    struct mvp_queuing mvpq;
    struct fdio_args fdioargs = FDIO_ARGS_INIT;
    struct runthread fdio_thread = { 0 };
    struct mpi_args mpiargs;

    /* limit prog to its basename, advance past any path before that */
    if ((cp = strrchr(prog, '/')) != NULL)
        prog = cp + 1;

    /* bring up MPI first and determine our rank and number of procs */
    mii.comm = MPI_COMM_WORLD;
    if (MPI_Init(&argc, &argv))
        errx(1, "MPI_Init unexpectedly failed");
    if (MPI_Comm_rank(mii.comm, &mii.rank) ||
        MPI_Comm_size(mii.comm, &mii.wsize))
        errx(1, "MPI unable to find my place in the world");
    mii.maxlocalsize = 0;   /* updated below */

    /* parse our command line options */
    while ((ch = getopt(argc, argv,
                 "B:c:d:D:ghi:j:k:l:L:m:M:n:q:r:s:S:t:u:w:X:")) != -1) {
        switch (ch) {
        case 'B':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                mopt.bufsz_ml = atoi(optrest);
                if (mopt.bufsz_ml < 1)
                    rmerror(mii.rank, match, "bad mlog bufsize %d",
                            mopt.bufsz_ml);
            }
            break;
        case 'c':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                if (strcmp(optrest, "1") == 0 ||
                    strcasecmp(optrest, "on") == 0) {
                    mopt.conlog = 1;
                } else if (strcmp(optrest, "0") == 0 ||
                           strcasecmp(optrest, "off") == 0) {
                    mopt.conlog = 0;
                } else {
                    rmerror(mii.rank, match,
                        "console log: invalid value (use 0 or 1): %s", optrest);
                }
            }
            break;
        case 'd':
            if (stringcheck(optarg, 0) != 0)
                rmerror(mii.rank, 0, "bad domain: %s", optarg);
            if (strvec_append(&mopt.domain, optarg, NULL) != 0)
                errx(1, "bad domain append: %s", optarg);
            break;
        case 'D':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                if (mlog_str2pri(optrest) == -1)
                    rmerror(mii.rank, match, "bad def priority %s", optrest);
                mopt.defpri_ml = optrest;
            }
            break;
        case 'g':
            mopt.gstats = 1;
            break;
        case 'i':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                if (strvec_append(&mopt.image, optrest, NULL) == -1)
                    rmerror(mii.rank, match, "unable to append imagename");
            }
            break;
        case 'j':
            mopt.jobname = optarg;
            if (strchr(mopt.jobname, '/'))
                rmerror(mii.rank, 0, "jobname: '/' not allowed: %s",
                        mopt.jobname);
            break;
        case 'k':
            if (strcmp(optarg, "1") == 0 || strcasecmp(optarg, "on") == 0) {
                mopt.kvm = 1;
            } else if (strcmp(optarg, "0") == 0 ||
                       strcasecmp(optarg, "off") == 0) {
                mopt.kvm = 0;
            } else {
                rmerror(mii.rank, 0, "kvm: bad val (use 0 or 1): %s", optarg);
            }
            break;
        case 'l':
            mopt.logdir = optarg;
            if (dirok(mopt.logdir) < 0)
                err(1, "logdir: %s", mopt.logdir);
            break;
        case 'L':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                if (strcmp(optrest, "1") == 0 ||
                    strcasecmp(optrest, "on") == 0) {
                    mopt.logfile_ml = 1;
                } else if (strcmp(optrest, "0") == 0 ||
                           strcasecmp(optrest, "off") == 0) {
                    mopt.logfile_ml = 0;
                } else {
                    rmerror(mii.rank, match,
                        "mlog log: invalid value (use 0 or 1): %s", optrest);
                }
            }
            break;
        case 'm':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                mopt.mem_mb = atoi(optrest);
                if (mopt.mem_mb < 1)
                    rmerror(mii.rank, match, "mem_mb: %s: need >= 1MB",
                            optrest);
            }
            break;
        case 'M':
            mopt.monwrap = optarg;
            break;
        case 'n':
            if (strcasecmp("stream", optarg) == 0) {
                mopt.nettype = SOCK_STREAM;
            } else if (strcasecmp("dgram", optarg) == 0) {
                mopt.nettype = SOCK_DGRAM;
            } else {
                rmerror(mii.rank, 0, "bad nettype: %s (stream or dgram)",
                        optarg);
            }
            break;
        case 'q':
            mopt.qemucmd = optarg;
            break;
        case 'r':
            mopt.rundir = optarg;
            if (dirok(mopt.rundir) < 0)
                err(1, "rundir: %s", mopt.rundir);
            break;
        case 's':
            mopt.sockdir = optarg;
            if (dirok(mopt.sockdir) < 0)
                err(1, "sockdir: %s", mopt.sockdir);
            break;
        case 'S':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                if (mlog_str2pri(optrest) == -1)
                    rmerror(mii.rank, match, "bad stderr priority %s",
                            optrest);
                mopt.stderrpri_ml = optrest;
            }
            break;
        case 't':
            mopt.tftpdir = optarg;
            if (dirok(mopt.tftpdir) < 0)
                err(1, "tftpdir: %s", mopt.tftpdir);
            if (stringcheck(mopt.tftpdir, '/') != 0)
                rmerror(mii.rank, 0, "tftpdir: %s: bad stringcheck",
                        mopt.tftpdir);
            break;
        case 'u':
            mopt.username = optarg;
            for (cp = optarg ; *cp ; cp++) {
                if (!isalnum(*cp))
                    errx(1, "bad char in username: %s", optarg);
            }
            break;
        case 'w':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                if (strcmp(optrest, "1") == 0 ||
                    strcasecmp(optrest, "on") == 0) {
                    mopt.wraplog = 1;
                } else if (strcmp(optrest, "0") == 0 ||
                           strcasecmp(optrest, "off") == 0) {
                    mopt.wraplog = 0;
                } else {
                    rmerror(mii.rank, match,
                        "wrap log: invalid value (use 0 or 1): %s", optrest);
                }
            }
            break;
        case 'X':
            match = prefix_num_match(optarg, ':', mii.rank, &optrest);
            if (match >= 0) {
                mopt.xmask_ml = optrest;
            }
            break;
        case 'h':
        default:
            usage(prog, mii.rank);
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1)
        usage(prog, mii.rank);

    if (mopt.image.nused == 0)
        rmerror(mii.rank, 1, "images must be specified with -i");
    if (cmdpathok(mopt.monwrap) < 0)
        errx(1, "bad monwarp prog: %s", mopt.monwrap);
    if (cmdpathok(mopt.qemucmd) < 0)
        errx(1, "bad qemu command: %s", mopt.qemucmd);

    /* setup globals and exit handler so we remove tmp files */
    g.mainpid = getpid();
    if (atexit(atexit_handler) == -1)
        err(1, "atexit");
    signal(SIGPIPE, SIG_IGN);   /* ignore sigpipe, we'll handle EPIPE errs */

    /*
     * now we need to start building filenames and command vectors
     * based on the command line flags we got.
     */

    /* tags */
    if (mopt.jobname == NULL) {
        jobtag = "";
    } else if (strgen(&jobtag, ".", mopt.jobname, NULL) < 1) {
        errx(1, "strgen: jobtag");
    }
    snprintf(ranktag, sizeof(ranktag), ".%d", mii.rank);

    /* open mlog */
    if (mopt.logfile_ml == 0) {
        mlog_log = NULL;
    } else if (strgen(&mlog_log, mopt.logdir, "/mvpnet", jobtag,
                      ranktag, ".log", NULL) < 1) {
        errx(1, "strgen: mlog_log");
    }
    for (lcv = 0 ; mkmloghdr_facdef[lcv].sname != NULL ; lcv++) {
        /*null*/;
    }
    if (mlog_open(prog, lcv, mlog_str2pri(mopt.defpri_ml),
                  mlog_str2pri(mopt.stderrpri_ml), mlog_log,
                  mopt.bufsz_ml, MLOG_LOGPID|MLOG_OUTTTY, 0) != 0)
        errx(1, "unable to open log");
    for (lcv = 0 ; mkmloghdr_facdef[lcv].sname != NULL ; lcv++) {
        if (mlog_namefacility(lcv, mkmloghdr_facdef[lcv].sname,
                              mkmloghdr_facdef[lcv].lname) != 0)
            errx(1, "unable to name mlog facility!");
    }
    if (mopt.xmask_ml)
        mlog_setmasks(mopt.xmask_ml, -1);

    /* mlog() calls are OK from this point on */
    mlog(MVP_INFO, "mlog opened");

    /* console, gstats, and monwrap logs */
    if (mopt.conlog == 0) {
        console_log = NULL;
    } else if (strgen(&console_log, mopt.logdir, "/con", jobtag,
                      ranktag, ".log", NULL) < 1) {
        mlog_exit(1, MVP_CRIT, "strgen: console_log");
    }
    if (mii.rank != 0 || mopt.gstats == 0) {
        gstats_log = NULL;
    } else if (strgen(&gstats_log, mopt.logdir, "/gstats", jobtag,
                      ranktag, ".log", NULL) < 1) {
        mlog_exit(1, MVP_CRIT, "strgen: gstats_log");
    }
    if (mopt.wraplog == 0) {
        wrapper_log = NULL;
    } else if (strgen(&wrapper_log, mopt.logdir, "/wrap", jobtag,
                      ranktag, ".log", NULL) < 1) {
        mlog_exit(1, MVP_CRIT, "strgen: wrap_log");
    }

    /* socket file names */
    if (mopt.nettype == SOCK_STREAM) {
        /* [0] = socket (we create, qemu makes 2 way stream connect on) */
        /* [1] = NULL (not used) */
        if (strgen(&socknames[0], mopt.sockdir, "/mvp", jobtag, ranktag,
                   ".sock", NULL) < 1)
            mlog_exit(1, MVP_CRIT, "strgen: sockdir");
        socknames[1] = NULL;
    } else {
        /* [0] = qo-rsock: qemu output "remote" (we create and recv on) */
        /* [1] = qi-lsock: qemu input "local" (qemu creates, we send on) */
        if (strgen(&socknames[0], mopt.sockdir, "/mvp", jobtag, ranktag,
                   ".qo-rsock", NULL) < 1  ||
             strgen(&socknames[1], mopt.sockdir, "/mvp", jobtag, ranktag,
                   ".qi-lsock", NULL) < 1)
            mlog_exit(1, MVP_CRIT, "strgen: sockdir");
    }

    /* determine max local size to help pick ssh forward port */
    if ((stride = mpimaxlocalsize(MPI_COMM_WORLD)) == -1)   /* collective! */
        errx(1, "mpimaxlocalsize failed");
    mii.maxlocalsize = stride;
    mlog(MVP_INFO, "MPI max local size: %d", stride);
    stride = ((stride + 99) / 100) * 100;  /* round up to mult of 100 */

    /* localhost port for ssh forwarding, encode rank in it */
    for (localport = 2200+mii.rank ; localport <= UINT16_MAX ;
         localport += stride) {
        if (ltcp_port_available(localport) == 0)  /* best effort, not held */
            break;
    }
    if (localport > UINT16_MAX)
        mlog_exit(1, MVP_ERR, "no local ports (wsize=%d, stride=%d)",
                  mii.wsize, stride);

    /* now we can build our qemu command line */
    qcliargs.mopt = &mopt;
    qcliargs.mi = mii;        /* struct copy */
    qcliargs.jobtag = jobtag;
    qcliargs.ranktag = ranktag;
    qcliargs.localport = localport;
    qcliargs.wraplog = wrapper_log;
    qcliargs.socknames = socknames;
    qemucli_gen(&qcliargs, &g.tmps, &qemuvec);

    /* log the config */
    mlog(MVP_NOTE, "config for rank %d:", mii.rank);
    mlog(MVP_NOTE, "tags: job=%s, rank=%s", jobtag, ranktag);
    if (mopt.conlog)
        mlog(MVP_NOTE, "console_log: %s", console_log);
    if (mopt.wraplog)
        mlog(MVP_NOTE, "wrap_log: %s", wrapper_log);
    mlog(MVP_NOTE, "socknames[0]: %s", socknames[0]);
    if (socknames[1])
        mlog(MVP_NOTE, "socknames[1]: %s", socknames[1]);
    if (mopt.rundir)
        mlog(MVP_NOTE, "rundir: %s", mopt.rundir);
    mlog(MVP_NOTE, "localport: %d", localport);
    if (qemuvec.base == NULL) {
        mlog(MVP_NOTE, "qemuvec: <null>");
    } else {
        mlog(MVP_NOTE, "qemuvec:");
        for (lcv = 0 ; lcv < qemuvec.nused - 1 ; lcv++) {
            mlog(MVP_NOTE, "qemuvec[%d]: %s", lcv, qemuvec.base[lcv]);
        }
        mlog(MVP_NOTE, "qemuvec[%d]: NULL", lcv);
    }

    /* create console log if enabled */
    if (console_log) {
        if ((conlog_fp = logfopen(console_log, "a")) == NULL)
            mlog_exit(1, MVP_ERR, "unable to open console log %s: %s",
                      console_log, strerror(errno));
    }

    /* create sockets */
    if (mopt.nettype == SOCK_STREAM) {
        sockfds[0] = mksock_un(SOCK_STREAM, socknames[0]);
        if (sockfds[0] < 0)
            mlog_exit(1, MVP_ERR, "mksock_un: %s: %s", socknames[0],
                      strerror(errno));
        sockfds[1] = -1;
    } else {
        sockfds[0] = mksock_un(SOCK_DGRAM, socknames[0]);
        if (sockfds[0] < 0)
            mlog_exit(1, MVP_ERR, "mksock_un: %s: %s", socknames[0],
                      strerror(errno));
        /* socknames[1] will be created by qemu at startup time */
        unlink(socknames[1]);    /* in case there was an old one */
        sockfds[1] = mksock_un(SOCK_DGRAM, NULL);
        if (sockfds[1] < 0)
            mlog_exit(1, MVP_ERR, "mksock_un: <no-name>: %s", strerror(errno));
    }
    if (strvec_append(&g.tmps, socknames[0], NULL) != 0)
        mlog_exit(1, MVP_CRIT, "g.tmps sockname[0] failed");

    /* init mvp_queuing subsystem */
    if (mvp_queuing_init(&mvpq) != 0)
        mlog_exit(1, MVP_CRIT, "mvp_queuing_init failed");

    /* setup fdio args */
    fdioargs.qvec = &qemuvec;
    fdioargs.mi = mii;     /* struct copy */
    fdioargs.localsshport = localport;
    fdioargs.nettype = mopt.nettype;
    fdioargs.socknames = socknames;
    fdioargs.sockfds = sockfds;
    fdioargs.confp = conlog_fp;
    fdioargs.mq = &mvpq;
    /* XXXCDC: anything else we need to pass to it?  app script? */

    /* now we can launch the fdio thread... */
    ret = pthread_create(&fdio_thread.pth, NULL, fdio_main, &fdioargs);
    if (ret != 0)
        mlog_exit(1, MVP_CRIT, "fdio pthread_create: %s", strerror(ret));
    fdio_thread.can_join = 1;

    /*
     * the main thread now takes on the role of the MPI thread.
     * the mpi thread handles calling MPI_Finalize().
     */
    mpiargs.mi = mii;          /* struct copy */
    mpiargs.mts_initsz = 16;   /* hardwired here for now */
    mpiargs.gstats_log = gstats_log;
    mpiargs.fdio = &fdio_thread;
    mpiargs.a = &fdioargs;
    mlog(MVP_INFO, "init complete!  transitioning to main MPI thread");
    exit(mpi_main(&mpiargs));
    /*NOTREACHED*/
}
