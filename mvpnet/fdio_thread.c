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
 * fdio_thread.c  file descriptor i/o thread
 * 29-Apr-2025  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "fbufmgr.h"
#include "fdio_thread.h"
#include "mvp_mlog.h"
#include "mvpnet.h"
#include "pktfmt.h"

/*
 * set up fd map for poll()
 */
#define PF_STDIN         0   /* mvpnet stdin (low or no traffic) */
#define PF_QEMUOUT       1   /* qemu stdout/stderr (console output) */
#define PF_NOTIFY        2   /* fdio notification pipe (read side) */
#define PF_ST_LISTEN     3   /* stream: listening socket fd */
#define PF_ST_CONN       4   /* stream: connected socket (if connected) */
#define PF_DG_QOUT       3   /* dgram: qemu pkt output (we recv()) */
#define PF_DG_QIN        4   /* dgram: qemu pkt input (we send()) */
#define PF_NFDS          5   /* size of pfd[] array needed */

/* struct with qemu sender state */
struct qemusender {
    struct recvq_entry *rqe; /* currently being sent to qemu */
    int nsent;               /* number of bytes sent so far */
    int blocked;             /* got EWOULDBLOCK on last send, wait to clear */
    int pending;             /* !=0  if we know more rqes are pending */
};

/*
 * helper functions
 */

/* update fdio thread state */
static void fdio_set_state(struct fdio_args *a, int new_state) {
    pthread_mutex_lock(&a->flk);
    a->fdio_state = new_state;
    mlog(FDIO_DBG, "fdio new-state: %s", fdio_statestr(new_state));
    pthread_cond_broadcast(&a->fcond);
    pthread_mutex_unlock(&a->flk);
}

/* release qemusender rqe back to mvp queuing */
static void fdio_qemusend_done(struct fdio_args *a, struct qemusender *curq) {
    if (curq->rqe) {
        mlog(FDIO_DBG, "qemusend_done: rqe %p, fbufmgr=%p",
             curq->rqe, (curq->rqe->bin == 0) ? curq->rqe->owner : NULL);
        if (curq->rqe->bin == 0 && curq->rqe->owner) {
            fbuf_return(curq->rqe->owner);   /* drop loan if from fbuf */
        }
        mvp_rq_release(a->mq, curq->rqe);
        curq->rqe = NULL;
        a->fst.qemusend_cnt++;
    }
    curq->nsent = 0;
    return;
}

/*
 * routing function for a simple binary broadcast tree on top of
 * MPI ranks.   each rank has 0, 1, or 2 next hops depending on
 * the size of the tree and their location in it.  this function
 * returns the number of children this rank has in the tree.
 * if there are children (i.e. our return value is > 0), their
 * rank numbers are returned in the children[] array.
 */
static int fdio_binary_router(int root_rank, int world_size,
                              int my_rank, int children[2]) {
    int my_adjusted_rank, adjusted_child0;

    /* shift all ranks so that root_rank is rank 0 in the adjusted world */
    my_adjusted_rank = my_rank - root_rank;
    if (my_adjusted_rank < 0)
        my_adjusted_rank += world_size;

    adjusted_child0 = (my_adjusted_rank * 2) + 1;
    if (adjusted_child0 >= world_size) {
        mlog(FDIO_DBG, "binary_router: root=%d, me=%d, children=[]",
             root_rank, my_rank);
        return(0);    /* no children */
    }
    children[0] = (adjusted_child0 + root_rank) % world_size;
    if (adjusted_child0 == world_size - 1) {
        mlog(FDIO_DBG, "binary_router: root=%d, me=%d, children=[%d]",
             root_rank, my_rank, children[0]);
        return(1);    /* one child */
    }
    children[1] = (children[0] + 1) % world_size;
    mlog(FDIO_DBG, "binary_router: root=%d, me=%d, children=[%d,%d]",
         root_rank, my_rank, children[0], children[1]);

    return(2);        /* two children */
}

/*
 * "broadcast" a frame down our broadcast tree.  the ranks of our
 * children in in the broadcast tree are in children[].  caller
 * should establish "nchild" loans on the fbuf _before_ calling us.
 * we drop loans on queuing error (otherwise they will get dropped
 * in the normal way after the MPI thread sends the data).
 */
static void fdio_broadcast(struct fbuf *fbuf, char *fstart, int nbytes,
                           struct fdio_args *a, char *type,
                           int nchild, int *children) {
    int lcv;
    struct sendq_entry *sqe;

    for (lcv = 0 ; lcv < nchild ; lcv++) {
        sqe = mvp_sq_queue(a->mq, children[lcv], fstart, nbytes, fbuf);

        if (sqe == NULL) {
            if (!mvp_sq_draining(a->mq))      /* null is ok if draining */
                mlog(FDIO_CRIT, "broadcast-%s: drop %d (no space)", type, lcv);
            fbuf_return(fbuf);  /* failed, drop the loan */
        } else {
            mlog(FDIO_DBG, "broadcast-%s: qok %d, dst=%d, sqe=%p, fb=%p",
                 type, lcv, children[lcv], sqe, fbuf);
        }
    }
}

/* ssh probe thread main */
static void *fdio_sshprobe(void *arg) {
    struct fdio_args *a = arg;
    char note;
    int ms_left, s, ret;
    struct sockaddr_in sin;
    struct timeval tbase;
    struct pollfd pf;
    char line[128];

    note = FDIO_NOTE_NOSSHD;               /* default is to fail... */
    ms_left = a->sshprobe_timeout * 1000;  /* timeout (cvt from secs) */

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(a->localsshport);

    /* loop until ms_left == 0 or state change (no need to lock here) */
    for (/*nop*/ ; ms_left > 0 && (a->fdio_state == FDIO_BOOT_QN ||
                                   a->fdio_state == FDIO_BOOT_Q) ;
         ms_left -= ms_time(&tbase, 0)) {

        ms_time(&tbase, 1);    /* set time */

        if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0)
            goto done;
        if (fcntl(s, F_SETFL, O_NONBLOCK) < 0) { /* no block in connect */
            close(s);
            goto done;
        }

        if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0 &&
            errno != EINPROGRESS) {
            close(s);
            NSLEEP(0, 500000000);   /* pause 1/2 a sec and try again */
            continue;
        }

        /* wait for connect to complete */
        pf.fd = s;
        pf.events = POLLOUT;
        pf.revents = 0;
        if (poll(&pf, 1, ms_left) < 1) {
            close(s);
            continue;
        }

        /* reset timer after poll and keep going */
        ms_left -= ms_time(&tbase, 0);
        ms_time(&tbase, 1);

        /* connected, now wait for data */
        pf.events = POLLIN;
        if (poll(&pf, 1, ms_left) < 1) {
            close(s);
            continue;
        }

        /* either we got an error or we got data. */
        ret = read(s, line, sizeof(line) - 1);
        close(s);           /* close socket either way */
        if (ret < 0) {
            NSLEEP(0, 500000000);   /* pause 1/2 a sec and try again */
            continue;
        }
        line[ret] = '\0';   /* null terminate */

        /* check for ssh banner */
        if (strncmp(line, "SSH-", 4) == 0) {   /* success? */
            note = FDIO_NOTE_SSHD;
            break;
        }
        NSLEEP(1, 0);     /* pause a second and try again */
    }

done:
    /* notify parent thread we are done and our status */
    if (write(a->mq->fdio_notify[NOTE_WR], &note, 1) < 0)
        fprintf(stderr, "write: NOTE_WR: %s\n", strerror(errno));
    return(NULL);
}

/*
 * process a complete ethernet frame that we've received from the
 * qemu sockets.   if we do not do anything with the frame, it will
 * be dropped.
 */
static void fdio_process_qframe(struct fbuf *fbuf, char *fstart, int nbytes,
                                struct fdio_args *a) {
    int xtrahdrsz = (a->nettype == SOCK_STREAM) ? 4 : 0;
    uint8_t *efrm;
    struct sendq_entry *sqe;
    int arp_qrank;
    struct recvq_entry *rqe;

    if (nbytes < xtrahdrsz + ETH_HDRSIZE) {
        a->fst.runt_cnt++;
        mlog(FDIO_WARN, "pqframe: runt packet droped (%d)", nbytes);
        return;
    }
    efrm = ((uint8_t *) fstart) + xtrahdrsz;

    /*
     * case 1: qemu sent a unicast ethernet frame.  extract dst
     * MPI rank from the dst ethernet address.  drop frame if
     * it not a valid rank to send to.   otherwise, queue it for
     * the MPI thread to send.
     */
    if ((efrm[ETH_DSTOFF] & 0x1) == 0) {  /* bit set only on bcast/mcast */
        int dst_rank;

        dst_rank = (efrm[ETH_DSTOFF+2] << 24) | (efrm[ETH_DSTOFF+3] << 16) |
                   (efrm[ETH_DSTOFF+4] << 8)  | efrm[ETH_DSTOFF+5];
        mlog(FDIO_DBG, "pqframe: unicast: %d => %d, fbuf=%p",
             a->mi.rank, dst_rank, fbuf);

        /* validate rank */
        if (dst_rank != a->mi.rank && dst_rank >= 0 &&
            dst_rank <= a->mi.wsize) {
            fbuf_loan(fbuf, 1);     /* establish loan */
            sqe = mvp_sq_queue(a->mq, dst_rank, fstart, nbytes, fbuf);
            if (sqe == NULL) {
                if (!mvp_sq_draining(a->mq))   /* ok if draining */
                    mlog(FDIO_CRIT, "pqframe: sqe/c1 alloc failed - dropping");
                fbuf_return(fbuf);  /* failed, drop the loan */
            } else {
                a->fst.unicast_cnt++;
                a->fst.unicast_bytes += nbytes;
                mlog(FDIO_DBG, "pqframe: unicast queued dst=%d sqe=%p fb=%p",
                     dst_rank, sqe, fbuf);
            }
        } else {
            a->fst.unicast_baddst++;
            mlog(FDIO_DBG, "pqframe: unicast bad rank: %d, drop!", dst_rank);
            /* bad rank in ETH_DSTOFF, silently drop frame */
        }
        return;
    }

    /*
     * case 2: broadcast ARP request.  we can directly answer these
     * since we know the mapping from MPI rank to IP and ethernet addrs.
     */
    if (pktfmt_arp_req_qrank(efrm, nbytes - xtrahdrsz, &arp_qrank) != -1) {
        void *rep_fstart;
        struct fbuf *rep_fbuf;
        uint8_t *rep_frm;

        /* shutdown broadcast?  honor (unless ignore flag set) */
        if (arp_qrank == MVPNET_SDOWNRANK && a->ign_shutdown_ip == 0) {
            mlog(FDIO_INFO, "pqframe: got shutdown broadcast");
            if (mvp_notify_fdio(a->mq, FDIO_NOTE_SNDSHUT) < 0)
                mlog(FDIO_ERR, "pqframe: shutdown trigger note failed");
            return;  /* drop the req */
        }

        if (arp_qrank < 0 || arp_qrank >= a->mi.wsize ||
            arp_qrank == a->mi.rank) {
            a->fst.arpreq_badreq++;
            mlog(FDIO_INFO, "pqframe: bad bcast-ARP for %d, drop!", arp_qrank);
            return;  /* drop req if unknown rank or for self */
        }

        mlog(FDIO_DBG, "pqframe: bcast-ARP: %d for %d, fbuf=%p",
             a->mi.rank, arp_qrank, fbuf);

        /* first allocate space for the reply in fbm_bcast */
        if (fbufmgr_loan_newframe(&a->mq->fbm_bcast, nbytes, 1,
                                  &rep_fstart, &rep_fbuf) != 0) {
            mlog(FDIO_CRIT, "pqframe: bcast-ARP: no space for reply %d - drop",
                 arp_qrank);
            return;
        }
        rep_frm = ((uint8_t *) rep_fstart) + xtrahdrsz;

        /* generate reply - size does not change */
        if (xtrahdrsz > 0)
            memcpy(rep_fstart, fstart, xtrahdrsz);
        pktfmt_arp_mkreply(efrm, arp_qrank, rep_frm);

        /*
         * add to recvq.  we do not need to notify ourselves since
         * the fdio thread progresses the rq after handling poll()
         * I/O (i.e. what we are doing now) so we'll catch it there
         * before the next call to poll().
         */
        rqe = mvp_rq_queue_fbuf(a->mq, rep_fstart, nbytes, rep_fbuf, 0);
        if (rqe == NULL) {
            mlog(FDIO_CRIT, "pqframe: rqe/c2 alloc fail %d, drop", arp_qrank);
            fbuf_return(rep_fbuf);  /* failed, drop the loan */
            return;
        }

        a->fst.arpreq_cnt++;
        mlog(FDIO_DBG, "pqframe: bcast-ARP reply queued rank=%d rqe=%p fb=%p",
             arp_qrank, rqe, rep_fbuf);
        return;
    }

    /*
     * case 3: generic broadcast/multicast.   qemu is starting a new
     * broadcast-like operation with our rank as the root.  use the
     * binary router to determine the next hops in the binary broadcast
     * tree and send the data there.  we will have 0, 1, or 2 next hops.
     *
     * if qemu is trying to directly send a shutdown broadcast, route
     * it through FDIO_NOTE_SNDSHUT instead of sending it here.  this
     * unifies shutdown broadcast handling in one central place where
     * we have everything we need to process it (e.g. qemu_stdin).
     */
    if (pktfmt_is_shutdown(efrm, nbytes - xtrahdrsz) != 0) {

        if (mvp_notify_fdio(a->mq, FDIO_NOTE_SNDSHUT) < 0)
            mlog(FDIO_CRIT, "pqframe: broadcast shutdown notify failed!");
        else
            mlog(FDIO_NOTE, "pqframe: broadcast shutdown trigger from qemu!");

    } else {

        int nc, children[2];       /* in broadcast tree */

        a->fst.bcast_st_cnt++;
        a->fst.bcast_st_bytes += nbytes;
        nc = fdio_binary_router(a->mi.rank, a->mi.wsize, a->mi.rank, children);
        mlog(FDIO_DBG, "pqframe: bcast start@%d: nchild=%d", a->mi.rank, nc);
        if (nc > 0) {
            fbuf_loan(fbuf, nc);
            fdio_broadcast(fbuf, fstart, nbytes, a, "start", nc, children);
        }

        return;
    }

}

/*
 * fbufmgr_recv callback function for streaming sockets.
 * returns number of bytes to advance fstart.
 */
static size_t fdio_fbstream_cb(struct fbuf *fbuf, char *fstart,
                               size_t nbytes, void *arg) {
    struct fdio_args *a = arg;
    char *ptr = fstart;
    size_t nleft = nbytes;
    uint32_t framelen;

    mlog(FDIO_DBG, "streamcb: add %zd bytes", nbytes);
    while (1) {                 /* decode as many frames as possible */

        if (nleft < 4) {        /* not enough data to decode frame length? */
            if (nleft)
                mlog(FDIO_DBG, "streamcb: partial-len: %zd, stop", nleft);
            break;
        }

        memcpy(&framelen, ptr, sizeof(framelen));
        framelen = ntohl(framelen) + sizeof(framelen);

        if (framelen > MVP_FBUFMIN)     /* sanity check */
            mlog_abort(FDIO_CRIT, "streamcb: BAD framlen %" PRId32, framelen);

        if (nleft < framelen) {  /* incomplete frame, save for later */
            mlog(FDIO_DBG, "streamcb: partial-frame: %zd < %" PRId32 ", stop",
                 nleft, framelen);
            break;
        }

        mlog(FDIO_DBG, "streamcb: got frame, len=%" PRId32, framelen);
        fdio_process_qframe(fbuf, ptr, framelen, a);

        nleft -= framelen;
        ptr += framelen;
    }

    mlog(FDIO_DBG, "streamcb: advance %zd bytes", ptr - fstart);
    return(ptr - fstart);
}

/*
 * fbufmgr_recv callback function for dgram sockets.
 * returns number of bytes to advance fstart.
 */
static size_t fdio_fbdgram_cb(struct fbuf *fbuf, char *fstart,
                              size_t nbytes, void *arg) {
    struct fdio_args *a = arg;

    /* dgram always receives a complete single frame */
    mlog(FDIO_DBG, "dgramcb: got frame, len=%zd", nbytes);
    fdio_process_qframe(fbuf, fstart, nbytes, a);
    return(nbytes);

}

/*
 * I/O helper functions called when poll indicates we have work to do.
 * will set final_state to indicate we should exit the poll loop.
 */

/*
 * PF_STDIN is readable.  this indicates our process has data on stdin
 * (typically from MPI).  if we get EOF on stdin we shut down.
 * if we get data on stdin, we make a best effort to relay it to
 * qemu stdin (the console input).   we don't expect much (if any)
 * data on this path.  we ignore errors writing to qemu (note
 * that qemu stdin is set to O_NONBLOCK as we don't want to block
 * writing it it).   if qemu has exited, we'll handle that later
 * as an EOF on qemu console output.
 */
static void fdio_read_stdin(struct fdio_args *a, struct pollfd *pf,
                            int *finalstate, int qstdin) {
    char buf[BUFSIZ];
    ssize_t got;

    got = read(pf->fd, buf, sizeof(buf));
    if (got < 1) {
        mlog(FDIO_DBG, "stdin: read rv=%zd, set DONE", got);
        *finalstate = FDIO_DONE;         /* EOF/error: exit poll loop */
        return;
    }
    if (qstdin >= 0) {                   /* only relay if qstdin is open */
        a->fst.stdin_cnt++;
        a->fst.stdin_bytes += got;
        got = write(qstdin, buf, got);   /* best effort, ignore errors */
    }
}

/*
 * PF_QEMUOUT is readable.   this indicates we got console output
 * from qemu.   if we read an EOF from the qemu console we assume
 * that qemu has exited (closing the pipe we are reading) and that
 * we should exit too.   if we get console output we either write
 * it to the console log file (if enabled) or discard if (if no
 * log file).   note that qemu stdout and stderr have been combined
 * onto the PF_QEMUOUT file descriptor.
 */
static void fdio_read_qconsole(struct fdio_args *a, struct pollfd *pf,
                               int *finalstate) {
    char buf[BUFSIZ];
    ssize_t got;

    got = read(pf->fd, buf, sizeof(buf));
    if (got < 1) {
        mlog(FDIO_DBG, "qconsole: read rv=%zd, set DONE", got);
        *finalstate = FDIO_DONE;    /* EOF/error: exit poll loop */
    } else {
        a->fst.console_cnt++;
        a->fst.console_bytes += got;
        if (a->confp)
            fwrite(buf, got, 1, a->confp);  /* ignore file write errors */
    }
}

/*
 * PF_NOTIFY is readable.   this means that the pfio thread is receiving
 * a notification from some other thread.   we read the notification and
 * act on it.   we should never get an EOF or error on read here, since
 * everything (including our thread) is part of the same process.
 */
static void fdio_read_notifications(struct fdio_args *a, struct pollfd *pf,
                                    int *finalstate, struct runthread *sshp,
                                    int *qstdinp) {
    char buf;
    ssize_t got;
    int ret, nchild, children[2], ndraining;
    struct sockaddr_un sun;
    void *sbc_pkt;             /* shutdown broadcast pkg buffer */
    struct fbuf *sbc_fbuf;

    got = read(pf->fd, &buf, sizeof(buf));   /* get the note */
    if (got != 1) {
        mlog(FDIO_ERR, "readnote: got %zd e=%d, set DONE", got,
             (got < 0) ? errno : 0);
        *finalstate = FDIO_ERROR;  /* something is seriously wrong */
        return;
    }
    switch (buf) {
    case FDIO_NOTE_STOP:           /* we've been asked to clean up and exit */
        mlog(FDIO_DBG, "readnote: STOP");
        *finalstate = FDIO_DONE;
        break;
    case FDIO_NOTE_SSHD:               /* sshd probe completed */
    case FDIO_NOTE_NOSSHD:
        mlog(FDIO_DBG, "readnote: SSHD (ok=%d)", buf == FDIO_NOTE_SSHD);
        pthread_join(sshp->pth, NULL);  /* finalize thread */
        sshp->can_join = 0;
        if (buf == FDIO_NOTE_NOSSHD) {  /* guest failed to start sshd */
            *finalstate = FDIO_ERROR;
            break;
        }
        if (a->nettype == SOCK_STREAM) {
            /* we can advance state, we are no longer waiting on Qemu */
            fdio_set_state(a, (a->fdio_state == FDIO_BOOT_QN) ?
                           FDIO_BOOT_N : FDIO_RUN);
        } else {
            /*
             * qemu is up, we should be able to connect() to the dgram
             * unix domain socket it created to recv packets from us on.
             */
            ret = strlen(a->socknames[1]) + 1;
            if (ret > sizeof(sun.sun_path))
                mlog_abort(FDIO_CRIT, "readnote: dgram sockname too long!");
            memset(&sun, 0, sizeof(sun));
            sun.sun_family = AF_LOCAL;
            strncpy(sun.sun_path, a->socknames[1], ret);
            ret = connect(a->sockfds[1], (struct sockaddr *)&sun, sizeof(sun));
            if (ret != 0) {
                mlog(FDIO_CRIT, "readnote: dgram connect fail (%s)",
                     strerror(errno));
                *finalstate = FDIO_ERROR;
                break;
            }
            if (fcntl(a->sockfds[1], F_SETFL, O_NONBLOCK) < 0)
                mlog_exit(1, FDIO_CRIT, "readnote: dg: fcntl: %d failed: %s",
                          a->sockfds[1], strerror(errno));


            /* both Q and N are now up */
            fdio_set_state(a, FDIO_RUN);
        }
        break;
    case FDIO_NOTE_QUEUE:
        a->fst.notequeue_cnt++;
        mlog(FDIO_DBG, "readnote: QUEUE");
        /*
         * MPI thread added a new packet to a->mq->mpirecvq and that queue
         * was previously empty.   MPI thread sent us a QUEUE notification
         * to wake us up in case we were blocked in poll().   we will check
         * and process the mpirecvq before calling poll again, so no
         * further action is required here.
         */
        break;
    case FDIO_NOTE_DRAINED:      /* mpisendq drain complete */
        mlog(FDIO_DBG, "readnote: DRAINED");
        if (*qstdinp >= 0) {     /* send EOF to qemu by closing its stdin */
            close(*qstdinp);
            *qstdinp = -1;
        }
        break;
    case FDIO_NOTE_SNDSHUT:
        if (a->mi.rank != 0) {
            mlog(FDIO_WARN, "readnote: SNDSHUT ignored (rank=%d)", a->mi.rank);
            break;
        }
        if (mvp_sq_draining(a->mq)) {
            mlog(FDIO_WARN, "readnote: SNDSHUT ignored (is draining)");
            break;
        }

        /* broadcast shutdown packet if needed */
        nchild = fdio_binary_router(a->mi.rank, a->mi.wsize, a->mi.rank,
                                    children);

        if (nchild > 0) {
            if (fbufmgr_loan_newframe(&a->mq->fbm_bcast, PKTFMT_SHUTDOWN_LEN,
                                      nchild, &sbc_pkt, &sbc_fbuf) != 0) {
                /* we can still shutdown, but we cannot relay the msg */
                mlog(FDIO_CRIT, "readnote: SNDSHUT no bcast buffer space!");
            } else {
                pktfmt_load_shutdown(sbc_pkt); /* len PKTFMT_SHUTDOWN_LEN */
                fdio_broadcast(sbc_fbuf, sbc_pkt, PKTFMT_SHUTDOWN_LEN,
                               a, "shutdown", nchild, children);
            }
        }

        /* start draindown (if not already running) */
        ndraining = mvp_sq_draindown(a->mq);
        if (ndraining == 0 && *qstdinp >= 0) {   /* close qemu stdin now */
            close(*qstdinp);
            *qstdinp = -1;
        }
        mlog(FDIO_DBG, "readnote: SNDSHUT relay (nchild=%d, draining=%d)",
             nchild, ndraining);

        break;
    default:
        mlog_exit(1, FDIO_CRIT, "readnote: unknown note %d!", buf);
    }
}

/*
 * PF_ST_LISTEN is readable.  this means something is trying to
 * connect to our listening socket.  our qemu child will only
 * connect to us once (when it is starting up).   we accept that
 * connection and then refuse any connections beyond that.
 */
static void fdio_read_listensock(struct fdio_args *a, struct pollfd *pf,
                                 struct pollfd *confd, int *finalstate) {
    int newsock;

    newsock = accept(pf->fd, NULL, 0);
    if (newsock < 0) {
        mlog(FDIO_ERR, "listensock: accept failed: %s", strerror(errno));
        *finalstate = FDIO_ERROR; /* shouldn't happen */
        return;
    }
    a->fst.accept_cnt++;

    if (confd->fd != -1) {        /* already have connected socket? */
        close(newsock);
        mlog(FDIO_WARN, "listensock: discarding redundant connection");
        return;
    }

    /* put the socket in O_NONBLOCK mode - should never fail */
    if (fcntl(newsock, F_SETFL, O_NONBLOCK) < 0)
        mlog_exit(1, FDIO_ERR, "listensock: set nbio on %d failed: %s",
                  newsock, strerror(errno));

    confd->fd = newsock;
    confd->events = POLLIN;    /* set events/revents to be safe */
    confd->revents = 0;
    fdio_set_state(a, (a->fdio_state == FDIO_BOOT_QN) ?
                   FDIO_BOOT_Q : FDIO_RUN);
}

/*
 * PF_ST_CONN is readable.  qemu is likely trying to send us a frame.
 * we send the inbound data to a->mq->fbm_main for framing.   if we
 * get EOF then we've lost our connection to qemu (it likely terminated)
 * and we exit.
 */
static void fdio_read_stconn(struct fdio_args *a, struct pollfd *pf,
                             int *finalstate) {
    ssize_t got;

    got = fbufmgr_recv(&a->mq->fbm_main, pf->fd, fdio_fbstream_cb, a);
    if (got < 1) {
        *finalstate = FDIO_DONE;
        return;
    }
}

/*
 * PF_DG_QUOT is readable.  qemu is likely trying to send us a frame.
 * we send the inbound data to a->mq->fbm_main for framing.   if we
 * get err/EOF then we've lost our connection to qemu (it likely terminated)
 * and we exit.
 */
static void fdio_read_dgqout(struct fdio_args *a, struct pollfd *pf,
                             int *finalstate) {
    ssize_t got;

    got = fbufmgr_recv(&a->mq->fbm_main, pf->fd, fdio_fbdgram_cb, a);
    if (got < 1) {
        *finalstate = FDIO_DONE;
        return;
    }
}

/*
 * check to see if there is a packet from MPI that we can start
 * processing.  if so, load it into the current qemusender (so
 * we will start writing it to the qemu socket) and also handle
 * any broadcast-related processing.
 */
static void fdio_load_next_rqe(struct fdio_args *a, struct qemusender *curq,
                               int *qstdinp) {
    int xtrahdrsz = (a->nettype == SOCK_STREAM) ? 4 : 0;
    uint8_t *efrm;
    int src_rank, children[2], nchild, ndraining;
    void *copy;
    struct fbuf *copy_fbuf;

    curq->rqe = mvp_rq_dequeue(a->mq, &curq->pending);

    if (curq->rqe == NULL)      /* return if nothing to do */
        return;

    mlog(FDIO_DBG, "loadnext: loaded new rqe %p", curq->rqe);
    curq->nsent = 0;           /* reset to start of frame */

     /*
      * if it is a broadcast/multicast frame, we make copies
      * and queue the copies for sending to the next hop.
      */
    if (curq->rqe->flen < xtrahdrsz + ETH_HDRSIZE)  /* to be safe */
        return;
    efrm = ((uint8_t *) curq->rqe->frame) + xtrahdrsz;
    if ((efrm[0] & 0x1) == 0)
        return;                  /* done if it is a unicast */

    /* recover source rank (i.e. root of bcast tree) from ETH_SRC */
    src_rank = (efrm[ETH_SRCOFF+2] << 24) | (efrm[ETH_SRCOFF+3] << 16) |
               (efrm[ETH_SRCOFF+4] << 8)  | efrm[ETH_SRCOFF+5];

    /* ignore pkts with an invalid src rank */
    if (src_rank < 0 || src_rank >= a->mi.wsize || src_rank == a->mi.rank) {
        a->fst.bcastin_badsrc++;
        mlog(FDIO_WARN, "loadnext: bcast rqe w/bad src_rank=%d", src_rank);
        return;
    }

    /* compute children in bcast tree */
    nchild = fdio_binary_router(src_rank, a->mi.wsize, a->mi.rank, children);
    mlog(FDIO_DBG, "loadnext: bcast from %d: nchild=%d", src_rank, nchild);

    /* send copies to any children we have */
    a->fst.bcastin_cnt++;
    a->fst.bcastin_bytes += curq->rqe->flen;

    /* if we have children in bcast tree: copy to fbm_bcast and forward */
    if (nchild > 0) {
        if (fbufmgr_loan_newframe(&a->mq->fbm_bcast, curq->rqe->flen, nchild,
                                  &copy, &copy_fbuf) != 0) {
            mlog(FDIO_CRIT, "loadnext: bcast from %d, newframe failed, drop",
                 src_rank);
            return;
        }
        memcpy(copy, curq->rqe->frame, curq->rqe->flen);
        fdio_broadcast(copy_fbuf, copy, curq->rqe->flen, a, "relay",
                       nchild, children);
    }

    /* check for and handle shudown frames */
    if (pktfmt_is_shutdown(efrm, curq->rqe->flen)) {
        fdio_qemusend_done(a, curq);  /* drop, no need to send to qemu */
        ndraining = mvp_sq_draindown(a->mq);
        if (ndraining == 0 && qstdinp >= 0) {
            close(*qstdinp);
            *qstdinp = -1;
        }
        mlog(FDIO_NOTE, "loadnext: recv shutdown (ndraining=%d)", ndraining);
    }
}

/*
 * we have data to write to PF_ST_CONN.   we are sending a frame to
 * qemu.  write what we can.  we are either going to write everything
 * (and finish the pending rqe) or get EWOULDBLOCK.
 */
static void fdio_write_sttoqemu(struct fdio_args *a, struct pollfd *pf,
                                struct qemusender *curq, int *finalstate) {
    int ret = 0;

    /* write as much as possible */
    while (curq->nsent < curq->rqe->flen) {
        ret = send(pf->fd, curq->rqe->frame + curq->nsent,
                   curq->rqe->flen - curq->nsent, 0);
        if (ret < 1)
            break;
        curq->nsent += ret;
        a->fst.qemusend_bytes += ret;
    }

    if (curq->nsent == curq->rqe->flen) {
        mlog(FDIO_DBG, "sttoqemu: done sending rqe %p", curq->rqe);
        fdio_qemusend_done(a, curq);
        return;
    }

    if (ret < 0 && errno == EWOULDBLOCK) {
        a->fst.qemusend_blocked++;
        mlog(FDIO_DBG, "sttoqemu: EWOULDBLOCK on rqe=%p, pause", curq->rqe);
        /* socket buffer is full.  block until there is space */
        curq->blocked = 1;
        pf->events |= POLLOUT;
        return;
    }

    /* unexpected error... exit fdio */
    mlog(FDIO_ERR, "sttoqemu: send error: %s", strerror(errno));
    *finalstate = FDIO_DONE;
    return;
}

/*
 * we have data to write to PF_DQ_QIN.   we are sending a frame to
 * qemu.  we are either going to write everything (and finish the
 * pending rqe) or get EWOULDBLOCK.
 */
static void fdio_write_dgtoqemu(struct fdio_args *a, struct pollfd *pf,
                                struct qemusender *curq, int *finalstate) {
    int ret;

    ret = send(pf->fd, curq->rqe->frame, curq->rqe->flen, 0);

    if (ret > 0) {                             /* sent something */
        a->fst.qemusend_bytes += ret;
        if (ret != curq->rqe->flen) {
            mlog(FDIO_WARN, "dgtoqemu: partial send? (r=%d, l=%d)", ret,
                 curq->rqe->flen);
        }
        mlog(FDIO_DBG, "dgtoqemu: done sending rqe %p", curq->rqe);
        fdio_qemusend_done(a, curq);
        return;
    }

    if (ret < 0 && errno == EWOULDBLOCK) {
        /* blocked in send... can this happen with a unix domain dgram? */
        a->fst.qemusend_blocked++;
        mlog(FDIO_DBG, "dgtoqemu: EWOULDBLOCK on rqe=%p, pause", curq->rqe);
        curq->blocked = 1;
        pf->events |= POLLOUT;
        return;
    }

    /* unexpected error... exit fdio */
    mlog(FDIO_ERR, "dgtoqemu: send error: %s", strerror(errno));
    *finalstate = FDIO_DONE;
    return;
}

/*
 * convert state to string (for debug prints)
 */
char *fdio_statestr(int state) {
    switch (state) {
        case FDIO_NONE:    return("NONE");
        case FDIO_PREP:    return("PREP");
        case FDIO_BOOT_QN: return("BOOT_QN");
        case FDIO_BOOT_Q:  return("BOOT_Q");
        case FDIO_BOOT_N:  return("BOOT_N");
        case FDIO_RUN:     return("RUN");
        case FDIO_DONE:    return("DONE");
        case FDIO_ERROR:   return("ERROR");
        default: break;
    }
    return("???");
}

/*
 * main routine for fdio thread
 */
void *fdio_main(void *arg) {
    struct fdio_args *a = arg;
    int final_state = FDIO_NONE;

    struct pollfd pfd[PF_NFDS];
    pid_t qemupid;
    int qemu_stdin, ret;
    struct runthread sshprobe = {0};
    struct qemusender cur_qsend = { NULL };
    int ptimeout;

    fdio_set_state(a, FDIO_PREP);    /* prepare to start qemu */

    /* use our fd map to setup the pollfd array for poll() */
    memset(pfd, 0, sizeof(pfd));

    pfd[PF_STDIN].fd = fileno(stdin);   /* look for data/EOF on our stdin */
    pfd[PF_STDIN].events = POLLIN;
    pfd[PF_QEMUOUT].fd = -1;            /* console output/EOF from qemu */
    pfd[PF_QEMUOUT].events = POLLIN;    /* will set fd when we fdforkprog() */
    pfd[PF_NOTIFY].fd = a->mq->fdio_notify[NOTE_RD];  /* internal notify */
    pfd[PF_NOTIFY].events = POLLIN;
    if (a->nettype == SOCK_STREAM) {
        pfd[PF_ST_LISTEN].fd = a->sockfds[0];   /* listening socket */
        pfd[PF_ST_LISTEN].events = POLLIN;
        pfd[PF_ST_CONN].fd = -1;        /* will set connected fd here */
        pfd[PF_ST_CONN].events = POLLIN;
    } else {
        pfd[PF_DG_QOUT].fd = a->sockfds[0];  /* recv dgrams here */
        pfd[PF_DG_QOUT].events = POLLIN;
        pfd[PF_DG_QIN].fd = a->sockfds[1];   /* send dgrams, after connect */
        pfd[PF_DG_QIN].events = 0;
    }

    /* now we can finally start qemu! */
    qemupid = fdforkprog(a->qvec->base[0], a->qvec->base, FDFPROG_FIOD,
                         &qemu_stdin, &pfd[PF_QEMUOUT].fd, NULL);
    if (qemupid < 0) {
        mlog(FDIO_CRIT, "main: qemu fdforkprog failed!");
        final_state = FDIO_ERROR;
        goto done;
    }
    fcntl(qemu_stdin, F_SETFL, O_NONBLOCK);  /* best effort, ignore errors */

    /*
     * qemu is now running.   we create a thread to connect to the
     * guest's sshd via usernet (using the localhost port forwarding).
     * if we can connect to the guest's sshd and get a sshd banner, then
     * we consider the guest fully booted.
     */
    mlog(FDIO_INFO, "launching sshprobe thread (localport=%d, timeout=%d)",
        a->localsshport, a->sshprobe_timeout);
    ret = pthread_create(&sshprobe.pth, NULL, fdio_sshprobe, a);
    if (ret != 0) {
        mlog(FDIO_CRIT, "main: pthread_create sshprobe failed (%d)", ret);
        final_state = FDIO_ERROR;
        goto done;
    }
    sshprobe.can_join = 1;
    fdio_set_state(a, FDIO_BOOT_QN);

    /*
     * main poll loop... loop until we are done.
     */
    mlog(FDIO_DBG, "main: entering main poll loop");
    while (final_state == FDIO_NONE) {

        /* determine poll timeout */
        if (cur_qsend.blocked == 0 &&
            (cur_qsend.rqe != NULL || cur_qsend.pending != 0)) {
            /* do not block in poll if we can progess cur_qsend */
            ptimeout = 0;
        } else {
            /* ok to indefinitely block in poll, no rqe work possible */
            ptimeout = -1;
        }

        ret = poll(&pfd[0], PF_NFDS, ptimeout);   /* may block here */
        if (ret == -1) {
            if (errno == EINTR)
                continue;            /* retry */
            mlog(FDIO_ERR, "main: poll error: %s", strerror(errno));
            final_state = FDIO_ERROR;
            goto done;
        }

        /*
         * process poll results, if there are any
         */
        if (ret > 0) {
            if (pfd[PF_STDIN].revents & (POLLIN|POLLHUP)) {
                fdio_read_stdin(a, &pfd[PF_STDIN], &final_state, qemu_stdin);
                if (final_state != FDIO_NONE)
                    break;
            }
            if (pfd[PF_QEMUOUT].revents & (POLLIN|POLLHUP)) {
                fdio_read_qconsole(a, &pfd[PF_QEMUOUT], &final_state);
                if (final_state != FDIO_NONE)
                    break;
            }
            if (pfd[PF_NOTIFY].revents & (POLLIN|POLLHUP)) {
                fdio_read_notifications(a, &pfd[PF_NOTIFY], &final_state,
                                        &sshprobe, &qemu_stdin);
                if (final_state != FDIO_NONE)
                    break;
            }
            if (a->nettype == SOCK_STREAM) {

                /* stream case has listen socket and connected socket */
                if (pfd[PF_ST_LISTEN].revents & (POLLIN|POLLHUP)) {
                    fdio_read_listensock(a, &pfd[PF_ST_LISTEN],
                                         &pfd[PF_ST_CONN], &final_state);
                    if (final_state != FDIO_NONE)
                        break;
                }

                if (pfd[PF_ST_CONN].revents & (POLLIN|POLLHUP)) {
                    fdio_read_stconn(a, &pfd[PF_ST_CONN], &final_state);
                    if (final_state != FDIO_NONE)
                        break;
                }

                /* asked for POLLOUT and got it, just clear blocked flag */
                if (pfd[PF_ST_CONN].revents & POLLOUT) {
                    pfd[PF_ST_CONN].events &= ~POLLOUT;
                    cur_qsend.blocked = 0;
                }

            } else {

                /* we recv frames from qemu on the PF_DG_QOUT socket */
                if (pfd[PF_DG_QOUT].revents & (POLLIN|POLLHUP)) {
                    fdio_read_dgqout(a, &pfd[PF_DG_QOUT], &final_state);
                    if (final_state != FDIO_NONE)
                        break;
                }

                /* asked for POLLOUT and got it, just clear blocked flag */
                if (pfd[PF_DG_QIN].revents & POLLOUT) {
                    pfd[PF_DG_QIN].events &= ~POLLOUT;
                    cur_qsend.blocked = 0;
                }

            }
        }   /* ret > 0 */

        /*
         * if cur_qsend is not blocked attempt to make rqe progress
         */
        if (cur_qsend.blocked == 0) {

            /* load a new rqe if none is currently active */
            if (cur_qsend.rqe == NULL) {

                /* reload cur_qsend.rqe */
                fdio_load_next_rqe(a, &cur_qsend, &qemu_stdin);

            }

            if (cur_qsend.rqe) {    /* previously blocked or new rqe */

                if (a->nettype == SOCK_STREAM) {
                    fdio_write_sttoqemu(a, &pfd[PF_ST_CONN],
                                        &cur_qsend, &final_state);
                    if (final_state != FDIO_NONE)
                        break;
                } else {
                    fdio_write_dgtoqemu(a, &pfd[PF_DG_QIN],
                                        &cur_qsend, &final_state);
                    if (final_state != FDIO_NONE)
                        break;
                }
            }
        }

    }  /* end of main loop */

done:   /* clean up and terminate the fdio thread */
    mlog(FDIO_DBG, "main: exited main poll loop - start cleanup");

    /* dispose of any rqes we are sending */
    fdio_qemusend_done(a, &cur_qsend);

    /* collect sshprobe if it still needs to be joined */
    if (sshprobe.can_join) {
        pthread_cancel(sshprobe.pth);
        pthread_join(sshprobe.pth, NULL);
    }

    /* wait for qemu and kill if it does not appear to be wrapping up */
    if (qemupid > 0) {
        struct timeval tv;
        if (qemu_stdin >= 0)
            close(qemu_stdin);   /* EOF tells wrapper to shutdown+exit */
        /* backstop the wrapper shutdown with our own timeout */
        tv.tv_sec = 120;
        tv.tv_usec = 0;
        if (twaitpid(&tv, qemupid, NULL) == 0) {
            warnx("qemu did not exit on EOF in time!  sending SIGKILL");
            kill(qemupid, SIGKILL);
            waitpid(qemupid, NULL, 0);
        }
    }

    /* done! */
    fdio_set_state(a, final_state);
    mlog(FDIO_DBG, "main: cleanup done, exiting");
    return(NULL);
}
