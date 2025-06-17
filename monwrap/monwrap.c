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
 * monwrap.c  monitor/wrapper to shutdown a program on EOF on stdin
 * 09-Apr-2025  chuck@ece.cmu.edu
 */

/*
 * monwrap wraps an application program with additional shutdown
 * logic based on pipes and signals.  without monwrap, the application
 * setup is:
 *
 *                    +-------------+
 *  --stdin---------->|             |
 *  <--stdout/stderr--| application |
 *                    |             |
 *                    +-------------+
 *
 * when but when you run with monwrap, the application setup is
 * modified to look like this:
 * 
 *                    +-------------+             +-------------+
 *  --stdin---------->|             |---inchain-->|             |
 *  <--stdout/stderr  |   monwrap   |<--monitor---| application |
 *                ^   |             |             |             |
 *                |   +-------------+             +-------------+
 *                |                              /
 *                \-----------------------------/
 *
 * monwrap copies data from it's stdin pipe to the application's
 * stdin (which is connected to the 'inchain' pipe) until:
 *   [1] it gets an EOF on its stdin pipe (whatever started monwrap exited)
 *   [2] it gets an EOF on the monitor pipe (because the app exited)
 *   [3] it gets a SIGTERM/SIGINT/SIGHUP signal
 *
 * when one of those events happens, it "finalizes" (shuts down)
 * the application using the following steps:
 *   - if the app processes has already exited, then monwrap exits
 *   - if (eoftime)
 *        send EOF to app via inchain
 *        wait eoftime secs for app to exit
 *        if the app exits in time, then monwrap exits
 *   - if (termtime)
 *        send SIGTERM to app
 *        wait termtime secs for app to exit
 *        if the app exits in time, then monwrap exits
 *   - send SIGKILL to app
 *   - wait for app to exit from SIGKILL and then monwrap exits
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/select.h>
#include <sys/wait.h>

#include "utilfns.h"

/*
 * global state (global so it can be accessed by signal handlers).
 */
struct monwrap_global {
    int inchain[2];      /* inchain pipe to child's stdin, parent writes [1] */
    int eoftime;         /* termination time for EOF */
    int termtime;        /* termination time for SIGTERM */
    pid_t cpid;          /* child process id (or 0 if no child) */
};

/*
 * default defines for monwrap_global
 */
#define DEF_EOFTIME  0    /* seconds */
#define DEF_TERMTIME 60   /* seconds */

static struct monwrap_global mg = { {-1,-1}, DEF_EOFTIME, DEF_TERMTIME, 0 };

/* 
 * optional log file.  once we close stdout/stderr, this is only
 * place we can print diagnostic log messages.
 */
static FILE *monlog = NULL;

/*
 * exit_waitstatus: exit using a status value from wait call.
 * if we don't have a status, we exit with -1.
 */
void exit_waitstatus(pid_t waitrv, int status) {
    if (waitrv < 1) {
        if (monlog)
            fprintf(monlog, "exit_waitstatus: no pid, waitrv=%d\n", waitrv);
        exit(-1);
    }
    if (WIFEXITED(status)) {
        if (monlog)
            fprintf(monlog, "exit_waitstatus: pid %d, EXIT=%d\n",
                    waitrv, WEXITSTATUS(status));
        exit( WEXITSTATUS(status) );
    }
    if (WIFSIGNALED(status)) {
        if (monlog)
            fprintf(monlog, "exit_waitstatus: pid %d, SIGNAL=%d\n",
                    waitrv, WTERMSIG(status));
        exit(-1); 
    }
    if (monlog)
        fprintf(monlog, "exit_waitstatus: pid %d, status=%d\n",
                waitrv, status);
    exit(-1); 
    /*NOTREACHED*/
}

/*
 * finalize_child: terminate the child if it is still running
 * and exit (with the wait status, if possible).
 */
void finalize_child(pid_t cpid, int termtime) {
    sigset_t termset;
    int status, rv;
    pid_t wpid;
    struct timeval tt;

    /*
     * we are called by both the main code and the sigterminate()
     * signal handler.  we block the set of terminate signals here to
     * prevent additional signals handled by sigterminate() from
     * calling us when we are already running.  there is no need
     * to unblock signals when we finish since finalize_child()
     * always exits when it finishes (rather than return to the caller).
     */
    sigemptyset(&termset);
    sigaddset(&termset, SIGTERM);
    sigaddset(&termset, SIGHUP);
    sigaddset(&termset, SIGINT);
    if (sigprocmask(SIG_BLOCK, &termset, NULL) == -1)
        err(1, "finalize_child: sigprocmask block");   /* unlikely */

    /* maybe the child already exited?   poll for it w/nohang */
    wpid = waitpid(cpid, &status, WNOHANG);
    if (wpid) {
        if (monlog)
            fprintf(monlog, "finalize_child: child %d already exited\n", cpid);
        exit_waitstatus(wpid, status);
    }

    /* eoftime set?  send EOF to child's stdin and wait eoftime for exit. */
    if (mg.eoftime && mg.inchain[1] >= 0) {
        close(mg.inchain[1]);   /* EOF */
        mg.inchain[1] = -1;
        if (monlog)
            fprintf(monlog, "finalize_child: EOF child stdin, eoftime=%d\n",
                   mg.eoftime);
        tt.tv_sec = mg.eoftime;
        tt.tv_usec = 0;
        wpid = twaitpid(&tt, cpid, &status);
        if (wpid) {
            if (monlog)
                fprintf(monlog, "finalize_child: exit after EOF\n");
            exit_waitstatus(wpid, status);
        }
        if (monlog)
            fprintf(monlog, "finalize_child: child unresponsive to EOF\n");
    }

    /* termtime set?  send SIGTERM to child and wait termtime for exit. */
    if (termtime) {
        rv = kill(cpid, SIGTERM);
        if (monlog)
            fprintf(monlog, "finalize_child: SIGTERM child/%d, termtime=%d\n",
                    rv, termtime);

        if (rv == 0 || (rv == -1 && errno == ESRCH)) {
            tt.tv_sec = termtime;
            tt.tv_usec = 0;
            wpid = twaitpid(&tt, cpid, &status);
            if (wpid) {
                if (monlog)
                    fprintf(monlog, "finalize_child: exit after SIGTERM\n");
                exit_waitstatus(wpid, status);
            }
        }
        if (monlog)
            fprintf(monlog, "finalize_child: child unresponsive to SIGTERM\n");
    }

    /* last resort is to SIGKILL the child */
    rv = kill(cpid, SIGKILL);
    if (monlog)
        fprintf(monlog, "finalize_child: SIGKILL child/%d\n", rv);

    if (rv == 0 || (rv == -1 && errno == ESRCH)) {
        if (monlog)
            fprintf(monlog, "finalize_child: blocking wait after SIGKILL\n");
        /* this should never block, since we did SIGKILL */
        wpid = waitpid(cpid, &status, 0);
        if (wpid) {
            if (monlog)
                fprintf(monlog, "finalize_child: exit after SIGKILL\n");
            exit_waitstatus(wpid, status);
        }
    }

    if (monlog)
        fprintf(monlog, "finalize_child: %d: failed.  exiting anyway\n", cpid);
    exit(-1);
    /*NOTREACHED*/
}

/*
 * xwrite: loop doing write to pipe/socket until done or error.
 */
int xwrite(int d, char *buf, size_t nb) {
    char *p;
    int resid, cnt, rv;

    for (p = buf, resid = nb, rv = 0 ; resid > 0 ; resid -= cnt, p += cnt) {
        cnt = write(d, p, resid);
        if (cnt < 1)      /* abort if error or no progress */
            return(-1);
        rv += cnt;
    }
    return(rv);
}


/*
 * sigterminate: handler for SIGTERM, SIGHUP, SIGINT
 */
static void sigterminate(int sig) {
    if (monlog)
        fprintf(monlog, "sigterminate: got %d (cpid=%d)\n", sig, mg.cpid);
    if (mg.cpid > 0)
        finalize_child(mg.cpid, mg.termtime);
     exit(0);
     /*NOTREACHED*/
}

/*
 * usage msg
 */
void usage(char *prog) {
    fprintf(stderr, "usage: %s [flags] cmd args ...\n", prog);
    fprintf(stderr, "flags are:\n");
    fprintf(stderr, "\t-e [secs]   EOF timeout before SIGTERM (def=%d)\n",
            DEF_EOFTIME);
    fprintf(stderr, "\t-l [file]   logfile (def=no log file)\n");
    fprintf(stderr, "\t-t [secs]   SIGTERM timeout before SIGKILL (def=%d)\n",
            DEF_TERMTIME);
    exit(1);
}

/*
 * monwrap: monitor/wrapper to shutdown a program on EOF on stdin.
 */
int main(int argc, char **argv) {
    char *prog = argv[0];
    char *logfile = NULL;  /* logfile off by default */
    int ch, devnull;
    int monpipe[2];  /* read side = [0], write side = [1] */
    sigset_t termset, oldset;
    fd_set rfds;
    int mx;


    /*
     * XXX: need '+' in optstring with glibc so that it does not permute
     * the args.   we want to stop parsing opts when we hit a non-option
     * so that we can pass that the rest to the child process as its
     * command line.  the '+' should be harmless on non-glibc getopts.
     * see discussion of POSIXLY_CORRECT in glib getopt man page.
     */
    while ((ch = getopt(argc, argv, "+e:l:t:")) != -1) {
        switch (ch) {
        case 'e':
            mg.eoftime = atoi(optarg);
            break;
        case 'l':
            logfile = optarg;
            break;
        case 't':
            mg.termtime = atoi(optarg);
            break;
        default:
            usage(prog);
        }
    }
    argc -= optind;
    argv += optind;
    
    if (argc == 0)
        usage(prog);

    /* sanity check args and open log file (if requested) */
    if (mg.eoftime < 0)
        errx(1, "bad -e eoftime value, must be >= 0");
    if (mg.termtime < 0)
        errx(1, "bad -t termtime value, must be >= 0");
    if (logfile) {
        monlog = fopen(logfile, "a");
        if (!monlog)
            err(1, "unable to open logfile %s", logfile);
        setlinebuf(monlog);
    }

    /*
     * ensure we can open /dev/null before we get too far along.
     * after we fork we need to drop our reference to our current
     * stdout/stderr fds (rather than continue to share them with
     * the child process).   we want to replace the stdout/stderr
     * fds with valid fds (rather than just closing them) so that
     * writes to stdout/stderr do not generate errors (and reset
     * errno to EBADF - this can confuse error handling).  we use
     * an open fd to /dev/null for this.
     */
    if ((devnull = open("/dev/null", O_WRONLY)) == -1) {
        err(1, "/dev/null open failed");
    }

    /*
     * set up pipes and signals.
     *
     * - pipe inchain  connected to child's stdin.  anything we receive
     *                 on our stdin we'll write to this pipe.  we do
     *                 not expect much (if any) data from stdin, so
     *                 we do not worry about blocking writing to inchain
     *                 (could happen if child does not read/drain its
     *                 stdin).
     *
     * - pipe monpipe  we keep the read side and the child gets the
     *                 write side (but shouldn't be aware of it since
     *                 it inherits it from us rather than opens it
     *                 itself).  if the child exits or dies by a
     *                 signal the write side of this pipe gets closed
     *                 and we'll see an EOF on the read side.  this
     *                 tells us the child is done and we should finalize
     *                 it.   (we assume the child does not attempt
     *                 to close file descriptors that it did not open.) 
     *
     * - SIGPIPE       we don't want SIGPIPE to kill us.   we can handle
     *                 getting a EPIPE error from an I/O call.
     *
     * - SIGTERM       finalize child and exit (also SIGHUP, SIGINT).
     *                 we block these during the fork.
     */
    if (pipe(mg.inchain) == -1 || pipe(monpipe) == -1)
        err(1, "pipe");
    signal(SIGPIPE, SIG_IGN);    /* don't let SIGPIPE kill us */
    sigemptyset(&termset);
    sigaddset(&termset, SIGTERM);
    sigaddset(&termset, SIGHUP);
    sigaddset(&termset, SIGINT);
    if (sigprocmask(SIG_BLOCK, &termset, &oldset) == -1) /* block for fork */
        err(1, "sigprocmask block");

    /*
     * we can now fork off the child process.
     */
    mg.cpid = fork();
    if (mg.cpid < 0) {
        err(1, "fork");
    }

    if (mg.cpid == 0) {
        /*
         * child process.  drop devnull and monlog (if it is open).
         * then we need to unblock termination signals, finish setting
         * up our file descriptors, and exec the app we are monitoring.
         */
        close(devnull);
        if (monlog) {
            fclose(monlog);
            monlog = NULL;
        }
        if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1) /* unblock */
            err(1, "sigprocmask unblock");

        /* close parent's side of inchain and plug our side into stdin */
        close(mg.inchain[1]);
        if (dup2(mg.inchain[0], STDIN_FILENO) == -1) {
            err(1, "dup2");
        }
        close(mg.inchain[0]);  /* now duped to stdin */

        /* close parent's side of monpipe and let our side just hang out */
        close(monpipe[0]);

        /* it is now safe to exec the user's application */
        execvp(argv[0], argv);
        fprintf(monlog, "execvp: %s: %s\n", argv[0], strerror(errno));
        exit(1);
        /*NOTREACHED*/
    }

    /*
     * parent process.  finish setting up our side of the file descriptors.
     * then set up terminate signal handlers and unblock and
     * enter the main loop...
     */
    /* close child's side of inchain.  we write our stdin to inchain[1] */
    close(mg.inchain[0]);

    /* 
     * close child's side of monpipe.  we look for EOF on our side
     * of the monpipe and finalize the child if we get it.
     */
    close(monpipe[1]);

    /*
     * we are currently sharing stdout/stderr with the child.
     * we replace our copy of stdout/stderr so that the child is
     * no longer sharing with us (if monlog is enabled, we will
     * write diag info there).
     */
    if (dup2(devnull, STDOUT_FILENO) < 0) {
        warn("dup2 devnull out");
        finalize_child(mg.cpid, mg.termtime);
    }
    if (monlog) {
        /* if log is enabled, send stderr there */
        if (dup2(fileno(monlog), STDERR_FILENO) < 0) {
            warn("dup2 devnull errmon");
            finalize_child(mg.cpid, mg.termtime);
        }
    } else {
        if (dup2(devnull, STDERR_FILENO) < 0) {
            warn("dup2 devnull err");
            finalize_child(mg.cpid, mg.termtime);
        }
    }
    close(devnull);   /* done with it */

    signal(SIGTERM, sigterminate);
    signal(SIGHUP, sigterminate);
    signal(SIGINT, sigterminate);
    if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1) {/* unblock */
        warn("sigprocmask unblock");
        finalize_child(mg.cpid, mg.termtime);
    }

    /*
     * main I/O loop
     */
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    FD_SET(monpipe[0], &rfds);
    mx = monpipe[0] + 1;

    if (monlog)
        fprintf(monlog, "monwrap: entering main I/O loop\n");

    while (1) {
        int rv, sent;
        char buf[512];

        rv = select(mx, &rfds, NULL, NULL, NULL);
        if (rv < 0) {
            if (errno == EINTR) continue;
            if (monlog)
                fprintf(monlog, "monwrap: select: %s\n", strerror(errno));
            finalize_child(mg.cpid, mg.termtime);  /* shouldn't ever happen */
        }

        /*
         * input from remote to our stdin?  we relay it to app
         * through the inchain[] pipe.   if we get EOF from the
         * remote then we finalize the child and exit.
         */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            rv = read(STDIN_FILENO, buf, sizeof(buf));
            if (rv < 1) {
                if (monlog)
                    fprintf(monlog, "monwrap: read stdin: %d %s\n",
                            rv, (rv == 0) ? "EOF" : strerror(errno));
                finalize_child(mg.cpid, mg.termtime);
            }
            /*
             * we do not expect to move much data this way.  but when
             * we do, we expect the app to keep up so we don't block
             * in the following write call.
             */
            sent = xwrite(mg.inchain[1], buf, rv);
            if (sent != rv) {
                if (monlog)
                    fprintf(monlog, "monwrap: write inchain: %d %s\n",
                            rv, strerror(errno));
                finalize_child(mg.cpid, mg.termtime);
            }
        } else {
            FD_SET(STDIN_FILENO, &rfds);   /* reset for next select call */
        }

        /*
         * input from the child app via monpipe?   if we get EOF,
         * we assume the child has exited and so we finalize and
         * exit too.   in the unlikely event that we get data
         * from the child app we discard it.  (since the child
         * app inherited the monpipe from us and did not open it
         * itself, it should be unaware of it and never write to it.)
         */
        if (FD_ISSET(monpipe[0], &rfds)) {
            rv = read(monpipe[0], buf, sizeof(buf));
            if (rv < 1) {
                if (monlog)
                    fprintf(monlog, "monwrap: read monpipe: %d %s\n",
                            rv, (rv == 0) ? "EOF" : strerror(errno));
                finalize_child(mg.cpid, mg.termtime);
            }
            /* discard the data in buf, we don't expect or need it */
        } else {
            FD_SET(monpipe[0], &rfds);
        }

    }   /* while (1) */
    /*NOTREACHED*/
}
