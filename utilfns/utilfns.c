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
 * utilfns.c  some helpful util functions
 * 02-Apr-2025  chuck@ece.cmu.edu
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "utilfns.h"

/*
 * selmax... keep track of max fd value for select.
 */
void selmax(int *current, int count, ...) {
    int lcv, val;
    va_list ap;

    va_start(ap, count);
    for (lcv = 0 ; lcv < count ; lcv++) {
        val = va_arg(ap, int);
        val++;                 /* adjust, since fds start at zero */
        if (val > *current)
            *current = val;
    }
    va_end(ap);
}

/*
 * read fd in fd_set and return 1 if it is ON, 0 if it is OFF.
 * also, if it is off, turn it back on for the next select.
 */
int fdreadreset(int select_rv, fd_set *fds, int fd) {

    if (select_rv <= 0 || !FD_ISSET(fd, fds)) {
        FD_SET(fd, fds);     /* turn back on for next select */
        return(0);
    }

    return(1);               /* already on, leave it be */
}

/*
 * set/clear fd's close on exec flag.   return 0 on success, -1 on fail.
 */
int fdcloexec(int fd, int state) {
    int flags;

    flags = fcntl(fd, F_GETFD);           /* get fd's current flags */
    if (flags == -1) {
        warn("fcntl: F_GETFD");
        return(-1);
    }

    if (state)                            /* edit flag bits */
        flags |= FD_CLOEXEC;
    else
        flags &= ~FD_CLOEXEC;

    if (fcntl(fd, F_SETFD, flags) < 0) {  /* install new flags */
        warn("fcntl: F_SETFD");
        return(-1);
    }

    return(0);
}

/*
 * open log file with close-on-exec and linebuf mode
 */
FILE *logfopen(const char *path, const char *mode) {
    FILE *fp;

    if ((fp = fopen(path, mode)) == NULL)
        return(NULL);
    setlinebuf(fp);
    if (fdcloexec(fileno(fp), 1) < 0) {
        fclose(fp);
        return(NULL);
    }

    return(fp);
}

/*
 * create PF_LOCAL socket and return close-on-exec socket fd.
 * if a path is given, we create that too.
 * return 0 on success, -1 on error.
 */
int mksock_un(int type, const char *path) {
    size_t plen;
    int sfd;
    struct sockaddr_un unaddr;

    /* null terminated path must fit in sun_path */
    plen = (path == NULL) ? 0 : strlen(path)+1;   /* includes null */
    if (plen > sizeof(unaddr.sun_path)) {
        warnx("mksock_un: %s: path too long", path);
        return(-1);
    }

    /* remove any stray sockets lying around */
    if (plen && access(path, F_OK) == 0 && unlink(path) < 0) {
        warn("mksock_un: old socket unlink(%s)", path);
        return(-1);
    }

    /* create new socket */
    sfd = socket(PF_LOCAL, type, 0);
    if (sfd < 0) {
        warn("mksock_un: socket");
        return(-1);
    }

    /* set close-on-exec */
    if (fdcloexec(sfd, 1) < 0) {
        close(sfd);
        return(-1);
    }

    if (plen) {
        /* setup address for bind */
        memset(&unaddr, 0, sizeof(unaddr));
        unaddr.sun_family = AF_LOCAL;
        strncpy(unaddr.sun_path, path, plen);

        /* bind to path in the sockaddr */
        if (bind(sfd, (struct sockaddr *)&unaddr, sizeof(unaddr)) < 0) {
            warn("mksock_un: bind");
            close(sfd);
            return(-1);
        }

        /* set to listen mode if stream socket */
        if (type == SOCK_STREAM && listen(sfd, 5) < 0) {
            warn("mksock_un: listen");
            close(sfd);
            unlink(path);
            return(-1);
        }
    }

    return(sfd);
}

/*
 * fdforkprog: fork program in a child process with stdio redirected
 * as specified.  for any pipes we create, we set the caller side
 * file descriptors to close-on-exec so that child only has the side
 * of the pipe it uses open.   return child pid on success, -1 on
 * failure.
 */
pid_t fdforkprog(const char *prog, char *const argv[], int flags,
                 int *stdinfd, int *stdoutfd, int *stderrfd) {
    int lcv, pipefds[7];   /* [0,1]=in, [2,3]=out, [4,5]=err, 6=xtra err */
    char *emsg;
    pid_t child;
    int errfd;

    /*
     * sanity check args.  if a flag bit is NOT set, then its
     * corresponding fd pointer must be NULL.  thus, if a fd
     * pointer is NOT NULL, then its flag bit must be set.
     * also, we do not allow both FERR and EDUPOUT to be set
     * at the same time.
     */
    if ( ((flags & FDFPROG_FIN) == 0  && stdinfd) ||
         ((flags & FDFPROG_FOUT) == 0 && stdoutfd) ||
         ((flags & FDFPROG_FERR) == 0 && stderrfd) ||
         ((flags & FDFPROG_FERR) && (flags & FDFPROG_EDUPOUT)) ) {
        warnx("fdforkprog: usage error (%#x,%p,%p,%p)", flags, stdinfd,
              stdoutfd, stderrfd);
        return(-1);
    }

    /* convert non-null FERR/FOUT with stderrfd==stdoutfd to a EDUPOUT */
    if ((flags & FDFPROG_FERR) && (flags & FDFPROG_FOUT) &&
        stderrfd && stderrfd == stdoutfd) {
        flags &= ~FDFPROG_FERR;
        flags |= FDFPROG_EDUPOUT;
        stderrfd = NULL;    /* same as stdoutfd, so do dupout op */
    }

    /* set pipe fds to invalid value (-1) for close logic in 'error' label */
    for (lcv = 0; lcv < sizeof(pipefds)/sizeof(pipefds[0]) ; lcv++) {
        pipefds[lcv] = -1;
    }

    /*
     * first, create all the pipes we need with the parent side set
     * to close-on-exec so they don't leak out on any future forks.
     * note that pipe[0] is the read side of the pipe while pipe[1]
     * is the write side.
     *
     * if the flag bit for a stdio stream is not set, then we leave
     * that stream as-is.  if the stream's fd pointer arg is NULL,
     * we will close that stream in the child.   if stderr's EDUPOUT
     * flag is set, FERR must be clear (and stderrfd must be NULL).
     * in this case make the child's stderr a dup of its stdout.
     */
    if (stdinfd) {
        /* child reads stdin from [0], parent write to child stdin on [1] */
        if (pipe(&pipefds[0]) == -1 || fdcloexec(pipefds[1], 1) == -1) {
            warn("pipe for stdin failed");
            goto error;
        }
    }
    if (stdoutfd) {
        /* child writes stdout to [3], parent reads child's stdout from [2] */
        if (pipe(&pipefds[2]) == -1 || fdcloexec(pipefds[2], 1) == -1) {
            warn("pipe for stdout failed");
            goto error;
        }
    }
    if (stderrfd) {
        /* child writes stderr to [5], parent reads child's stderr from [4] */
        if (pipe(&pipefds[4]) == -1 || fdcloexec(pipefds[4], 1) == -1) {
            warn("pipe for stderr failed");
            goto error;
        }
    }

    /*
     * if the caller is changing stderr, dup the current stderr into
     * a second fd and mark it close on exec.  we'll use this to print
     * an error if execvp() failed (rather than dying silently).
     */
    if (flags & (FDFPROG_FERR|FDFPROG_EDUPOUT)) {
        if ((pipefds[6] = dup(fileno(stderr))) == -1) {
            warn("copy of stderr failed");
            goto error;
        }
        if (fdcloexec(pipefds[6], 1) == -1) {
            goto error;
        }
    }

    /*
     * now we can fork the child process...
     */
    child = fork();
    if (child == -1) {
        warn("fork");
        goto error;
    }

    /*
     * the child can now finish the I/O setup and exec the requested program.
     */
    if (child == 0) {

        if (flags & FDFPROG_FIN) {   /* changing stdin? */
            if (stdinfd) {
                if (dup2(pipefds[0], STDIN_FILENO) < 0) {  /* dup to std */
                    err(1, "dup of pipe to stdin failed");
                }
                close(pipefds[0]);      /* close now dup'd copy */
                close(pipefds[1]);      /* close parent's side of pipe */
            } else {
                close(STDIN_FILENO);    /* no stdin for child */
            }
        }

        if (flags & FDFPROG_FOUT) {   /* changing stdout? */
            if (stdoutfd) {
                if (dup2(pipefds[3], STDOUT_FILENO) < 0) {  /* dup to std */
                    err(1, "dup of pipe to stdout failed");
                }
                close(pipefds[2]);      /* close parent's side of pipe */
                close(pipefds[3]);      /* close now dup'd copy */
            } else {
                close(STDOUT_FILENO);   /* no stdout for child */
            }
        }

        /* stderr handling, do dupout case first.  stdout is already set. */
        if (flags & FDFPROG_EDUPOUT) {
            if ((flags & FDFPROG_FOUT) && stdoutfd == NULL) {
                close(STDERR_FILENO);  /* dup no stdout to no stderr too */
            } else {
                if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
                    err(1, "failed to merge stdout and stderr");
                }
            }
        } else if (flags & FDFPROG_FERR) {
            if (stderrfd) {
                if (dup2(pipefds[5], STDERR_FILENO) < 0) {  /* dup to std */
                    err(1, "dup of pipe to stderr failed");
                }
                close(pipefds[4]);      /* close parent's side of pipe */
                close(pipefds[5]);      /* close now dup'd copy */
            } else {
                close(STDERR_FILENO);   /* no stderr for child */
            }
        }

        (void) execvp(prog, argv);

         /*
          * execvp() has failed, but we cannot print an error msg
          * to stderr because it may have been closed (in which case
          * a dup'd copy of stderr fd is in pipefds[6]).   figure
          * out where we can put an error message, and just use
          * write() to do it.
          */
         errfd = (pipefds[6] != -1) ? pipefds[6] : STDERR_FILENO;

        emsg = strerror(errno);
#define EWRITE(F,MSG) do {                                                    \
                        if (write(F, MSG, strlen(MSG)) < 0) exit(1);          \
                    } while (0);
        EWRITE(errfd, "fdforkprog exec: ");
        EWRITE(errfd, prog);
        EWRITE(errfd, ": ");
        EWRITE(errfd, emsg);
        EWRITE(errfd, " - child exiting!\n");
#undef EWRITE
        exit(1);
        /*NOTREACHED*/
    }

    /*
     * we are the parent.   finish setting up the file descriptors
     * and return success (for the fork, the exec can still fail later).
     */
    if (stdinfd) {             /* sanity check above ensured FIN is set */
        *stdinfd = pipefds[1];
        close(pipefds[0]);
    }
    if (stdoutfd) {
        *stdoutfd = pipefds[2];
        close(pipefds[3]);
    }
    if (stderrfd) {            /* cannot be EDUPOUT case */
        *stderrfd = pipefds[4];
        close(pipefds[5]);
    }
    if (pipefds[6] != -1)
        close(pipefds[6]);     /* don't need child's copy of stderr */

    /*
     * done, return child process.
     */
    return(child);

error:
    /* drop any fds we created */
    for (lcv = 0; lcv < sizeof(pipefds)/sizeof(pipefds[0]) ; lcv++) {
        if (pipefds[lcv] != -1) {
            close(pipefds[lcv]);
        }
    }
    return(-1);
}

/*
 * sigalrm_hand: doesn't need to do anything.   we just want it
 * to interrupt waitpid().
 */
static void sigalrm_hand(int dummy) {
    if (0)
        fprintf(stderr, "sigalrm_hand: SIGALRM posted\n");
    return;
}

/*
 * twaitpid: timed waidpid system call.
 *
 * params:
 *   tvwait: NULL - disable timeout, can block indefinitely
 *           < 0  - negative timeout, convert to 0 (poll)
 *           0    - poll for process, don't block
 *           > 0  - max wait time
 *
 *   pidspec: -1  - wait for any child process
 *            0   - wait for any child in process group
 *            > 0 - wait for a specific pid
 *            <-1 - wait for any process in absolute value process group
 *
 *   status: exit status returned here on successful wait
 *
 *   return values:
 *            -1  - err or no process matched pidspec that we could wait for
 *            0   - timed out before a matching process could exit
 *            pid - process id pid that we successfully waited for
 *
 * note that you can only wait for your own child processes (i.e.
 * you can't wait for other people's processes to exit, that gives -1).
 *
 * also note that the timeout code (e.g. when tvwait > 0) will not
 * work in a threaded environment without protection since it uses
 * ITIMER_REAL/SIGALRM to manage the timeout.
 */
int twaitpid(struct timeval *tvwait, int pidspec, int *status) {
    struct sigaction act, oact;
    struct itimerval it, oit;
    int rv;

    /* if no timeout we can just do a waitpid() */
    if (!tvwait)
        return(waitpid(pidspec, status, 0));

    /* zero out unexpected negative times (just poll in this case) */
    if (tvwait->tv_sec < 0) tvwait->tv_sec = 0;
    if (tvwait->tv_usec < 0) tvwait->tv_usec = 0;

    /* if polling we can just do a waitpid() with WNOHANG */
    if (tvwait->tv_sec == 0 && tvwait->tv_usec == 0)
        return(waitpid(pidspec, status, WNOHANG));

    /* need to engage a timer - set dummy signal handler */
    if (sigaction(SIGALRM, NULL, &oact) == -1) {   /* save old handler */
        warn("sigaction oact failed");
        return(-1);
    }
    act = oact;     /* edit old action */
    act.sa_handler = sigalrm_hand;
    act.sa_flags &= ~(SA_RESTART|SA_SIGINFO);   /* enable EINTR for waitpid */
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        warn("sigaction act failed");
        return(-1);
    }

    /* set timer */
    it.it_value = *tvwait;       /* time to next expire (from user) */
    /*
     * if the user's timeout is very short it is possible that we
     * lose a race and the timeout fires before we get around to
     * calling (and blocking in) waitpid().   in that case we could
     * get blocked in a waitpid() with no timeout to interrupt us.
     * this is unlikely, as we don't expect to be called with super
     * short timeouts... but just in case we set the it_interval
     * timer repeat value to 1 second so that waitpid() will continue
     * to get interrupted even after the main timeout expires.
     */
    it.it_interval.tv_sec = 1;
    it.it_interval.tv_usec = 0;

    if (setitimer(ITIMER_REAL, &it, &oit) == -1) {
        warn("setitimer");
        if (sigaction(SIGALRM, &oact, NULL) == -1)
            warn("sigaction: unable to restore SIGALRM");
        return(-1);
    }

    rv = waitpid(pidspec, status, 0);
    if (rv == -1) {
        if (errno == EINTR) {
            /*
             * EINTR means we got an alarm timeout before a process
             * matching our pidspec exited.  This is not an error
             * for us, so clear rv so that we return 0.
             */
            rv = 0;
        }
    }
    if (setitimer(ITIMER_REAL, &oit, NULL) == -1)
       warn("setitimer: unable to restore ITIMER_REAL");
    if (sigaction(SIGALRM, &oact, NULL) == -1)
        warn("sigaction: unable to restore SIGALRM");

    return(rv);
}

/*
 * ms_time: set base time or compute number of ms since base time
 * was set.   returns number of ms we are currently from base time.
 */
int ms_time(struct timeval *base, int set) {
    struct timeval now, delta;

    if (set) {
        gettimeofday(base, NULL);
        return(0);
    }

    gettimeofday(&now, NULL);
    delta.tv_sec = now.tv_sec - base->tv_sec;
    delta.tv_usec = now.tv_usec - base->tv_usec;
    if (delta.tv_usec < 0) {
        delta.tv_sec--;
        delta.tv_usec += 1000000;
    }

    if (delta.tv_sec < 0) {     /* time went backwards?  reset */
        gettimeofday(base, NULL);
        return(0);
    }

    return((delta.tv_sec * 1000) + (delta.tv_usec / 1000)); /* cvt to ms */
}

/*
 *  dirok: is it a directory we can use?   return 0 if ok, -1 if not.
 */
int dirok(char *dir) {
    struct stat st;
    if (stat(dir, &st) < 0)
        return(-1);
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return(-1);
    }
    if (access(dir, R_OK|W_OK|X_OK) != 0)
        return(-1);
    return(0);
}

/*
 * cmdpathok: is the named file an executable file on the PATH?
 * note: we do not check the file to see if it's contents are valid.
 * return 0 if ok, -1 if not.
 */
int cmdpathok(char *cmd) {
    struct stat st;
    char *path, *tmpbuf, *p;
    int rv, pathlen, tblen;
    char *pgot, *presid;

    /* anything with a '/' in it does not go through the PATH */
    if (strchr(cmd, '/')) {
        if (stat(cmd, &st) == 0 && S_ISREG(st.st_mode) &&
            access(cmd, X_OK) == 0) {
            return(0);
        }
        return(-1);
    }

    /* set known state so we can go to done */
    path = tmpbuf = NULL;
    rv = -1;
    
    p = getenv("PATH");
    pathlen = (p) ? strlen(p) : 0;
    if (pathlen == 0)                /* no meaningful path? */
        goto done;
    tblen = pathlen + 1 + strlen(cmd) + 1;  /* add '/' and '\0' */
    if ((path = strdup(p)) == NULL || (tmpbuf = malloc(tblen)) == NULL)
        goto done;

    presid = path;
    while ((pgot = strsep(&presid, ":")) != NULL) {
        if (strlen(pgot) == 0)
            continue;
        snprintf(tmpbuf, tblen, "%s/%s", pgot, cmd);
        if (stat(tmpbuf, &st) == 0 && S_ISREG(st.st_mode) &&
            access(tmpbuf, X_OK) == 0) {
            rv = 0;
            goto done;
        }
    }

done:
    if (path) free(path);
    if (tmpbuf) free(tmpbuf);
    return(rv);
}

/*
 * prefix number match: we are given a string, a prefix separator
 * character, and a non-negative number.   the string is formatted as:
 *
 *    [num-range-spec] [prefix-separator] [rest-of-string]
 *
 * the prefix-separator character can be null ('\0'), in which
 * case the entire string is treated as a num-range-spec.
 * if the prefix-separator is not null and not found in the
 * string, then no num-range-spec was given.  (note that the
 * prefix-separator cannot be a digit, a dash, or a comma.)
 *
 * if we have a num-range-spec, we match the number we are
 * given to it.   a num-range-spec is an unsorted set of 
 * comma separated number ranges (ranges are inclusive).
 * example number spec prefixes:
 *     "1"  "32"  "15,7"  "7-9,20,1-3"
 *
 * we return 0 if no num-range-spec was found (can only happen
 * if prefix-separator is not '\0' and not present in the string).
 * otherwise, we return 1 if our number matched the num-range-spec
 * and -1 if it did not match.
 *
 * we can return a pointer to rest-of-string if requested.
 * this is only useful if prefix-separator is not '\0' ...
 * if the prefix-separator is not found the returned rest-of-string
 * pointer points to the start of the string.
 */
int prefix_num_match(char *str, int presep, int n, char **restp) {
    int rv, range;
    char *cp, *start, *end;

    rv = -1;            /* assume no match */
    cp = str;
    if (restp)
        *restp = str;   /* default is to not consume anything */
    while (*cp && *cp != presep) {
        range = 0;
        start = end = NULL;

        /* get starting point, if specified */
        if (isdigit(*cp)) {
            start = cp++;
            while (*cp && *cp != presep && *cp != '-' && *cp != ',' &&
                   isdigit(*cp)) {
                cp++;
            }
        }

        /* is it a range? */
        if (*cp == '-') {
            range = 1;
            cp++;
        }

        /* get ending point, if specified */
        if (isdigit(*cp)) {
            end = cp++;
            while (*cp && *cp != presep && *cp != ',' && isdigit(*cp)) {
                cp++;
            }
        }

        /* ensure we are at the end */
        if (*cp && (*cp != ',' && *cp != presep)) {
            return(0);
        }

        /* consume the comma at the end  */
        if (*cp == ',')
            cp++;


        /* no numbers specified? */
        if (start == NULL && end == NULL) {
            if (range)       /* disallow a lone '-' without digits */
                return(0);
            continue;        /* allow empty num-range-spec, just skip over */
        }

        /* for non-range, just match the start */
        if (!range) {
            if (atoi(start) == n)
                rv = 1;
            continue;
        }

        /* start without an end... "5-" means anything >= start */
        if (end == NULL) {
            if (n >= atoi(start))
                rv = 1;
            continue;
        }

        /* end without a start... "-5" means anything <= end */
        if (start == NULL) {
            if (n <= atoi(end))
                rv = 1;
            continue;
        }

        /* full range */
        if (n >= atoi(start) && n <= atoi(end))
            rv = 1;

        /* even if we get a match, parse to end to check for syntax errs */
    }

    /* ensure we hit presep if one was given */
    if (presep && *cp != presep) {
        return(0);
    }

    if (restp)
        *restp = (presep) ? cp + 1 : cp;

    return(rv);
}

/*
 * see if a number matches the ranges defined by the string.
 * ranges are inclusive (so "0-1" matches 0 and 1).
 * example strings: "1" "32"  "1,3,5"  "1,36,4-9,15,20-30,17"
 * return 0 if the match was a success, -1 on error.
 */
int num_match(char *str, int n) {
    int rv = prefix_num_match(str, 0, n, NULL);
    return(( rv == 1) ? 0 : -1);
}

/*
 * strdupl() is a version of strdup() that can also return the string
 * length.  returns NULL if malloc fails.
 */
char *strdupl(const char *str, size_t *lenp) {
    size_t len;
    char *dup;

    len = strlen(str);
    dup = malloc(len + 1);       /* +1 to alloc (and later copy) the \0 */
    if (!dup)
        return(NULL);
    memcpy(dup, str, len + 1);
    if (lenp)
        *lenp = len;
    return(dup);
}

/*
 * load file into a malloced buffer terminated by a \0 at EOF.
 * return buffer on success, NULL on failure.   if szp is set
 * we return the length of the file (not including the \0 we added).
 */
char *loadfile(char *file, off_t *szp) {
    int fd = -1;
    struct stat st;
    char *rv = NULL, *b = NULL, *p;
    off_t resid;
    ssize_t got;

    /* open file, get size, malloc buf */
    if ( (fd = open(file, O_RDONLY)) < 0 || 
         fstat(fd, &st) < 0 ||
         (b = malloc(st.st_size + 1)) == NULL )
        goto done;

    /* load all the data in */
    for (resid = st.st_size, p = b ; resid > 0 ; resid -= got, p += got) {
        got = read(fd, p, resid);
        if (got < 1)
            goto done;
    }

    /* success! */
    b[st.st_size] = '\0';
    rv = b;
    b = NULL;
    if (szp)
       *szp = st.st_size;

done:
    if (b)
        free(b);
    if (fd >= 0)
        close(fd);
    return(rv);
}

/*
 * generate a newly malloced string from the given parts.
 * part list must end in a NULL (for stdarg to know when to stop).
 * return size of malloced buffer (including the \0), or 0 on error.
 * caller is responsible for freeing the returned string.
 */
size_t strgen(char **newstr, ...) {
    va_list ap;
    size_t newlen;
    char *n, *nstr, *part;

    /* pass 1: compute length we need */
    va_start(ap, newstr);
    newlen = 1;   /* include byte for null termination */
    while ((part = va_arg(ap, char *)) != NULL) {
         newlen += strlen(part);
    }
    va_end(ap);

    /* pass 2: malloc and assemble new string */
    n = nstr = malloc(newlen);    /* includes space for null */
    if (!nstr)
        return(0);
    va_start(ap, newstr);
    while ((part = va_arg(ap, char *)) != NULL) {
        while (*part) {
            *n++ = *part++;
        }
    }
    va_end(ap);
    *n = '\0';

    *newstr = nstr;
    return(newlen);
}

#if 0
/*
 * dump a strvec to stdout (for debugging)
 */
static void strvec_dump(char *msg, struct strvec *sv) {
    int lcv;
    printf("%s: sv: base=%p, nalloc=%d, nused=%d, nbytes=%zd, ini=%d\n", msg,
           sv->base, sv->nalloc, sv->nused, sv->nbtytes, sv->initial_alloc);
    if (sv->base) {
        for (lcv = 0 ; lcv < sv->nused ; lcv++) {
            if (sv->base[lcv])
                printf("  base[%d] len=%zd => %p %s\n", lcv,
                       sv->lens[lcv], sv->base[lcv], sv->base[lcv]);
            else
                printf("  base[%d] len=%zd => NULL\n", lcv, sv->lens[lcv]);
        }
    }
}
#endif

/*
 * free (and reset) a strvec (the value of initial_alloc is preserved).
 */
void strvec_free(struct strvec *sv) {
    int lcv;

    if (sv->base) {
        for (lcv = 0 ; lcv < sv->nused - 1; lcv++) {
            if (sv->base[lcv])
                free(sv->base[lcv]);
        }
        free(sv->base);
        sv->base = NULL;
    }
    if (sv->lens) {
        free(sv->lens);
        sv->lens = NULL;
    }
  
    sv->nalloc = sv->nused = 0;
    sv->nbytes = 0;
}

/*
 * append string(s) to end of a strvec.   ret 0 on success, -1 on failure.
 */
int strvec_append(struct strvec *sv, ...) {
    va_list ap;
    int cnt, want, old_nused, lcv;
    char *s, **newv, *sdup;
    size_t *newl, old_nbytes, dupl;

    /* allocate base[]/lens[] the first time we are called */
    if (sv->base == NULL) {
        if (sv->initial_alloc < 1)
            sv->initial_alloc = STRVEC_INITIAL_ALLOC;
        sv->base = malloc(sizeof(*sv->base) * sv->initial_alloc);
        if (sv->base == NULL)
            return(-1);
        sv->lens = malloc(sizeof(*sv->lens) * sv->initial_alloc);
        if (sv->lens == NULL) {
            free(sv->base);
            sv->base = NULL;
            return(-1);
        }
        sv->nalloc = sv->initial_alloc;
        sv->nused = 1;
        sv->nbytes = 0;     /* to be safe */
        sv->base[0] = NULL;
        sv->lens[0] = 0;
    }

    /* determine how many new slots we need */
    cnt = 0;
    va_start(ap, sv);
    while ((s = va_arg(ap, char *)) != NULL) {
        cnt++;
    }
    va_end(ap);

    /* do we need to realloc base? */
    if (cnt > sv->nalloc - sv->nused) {
        want = sv->nalloc + (cnt - (sv->nalloc - sv->nused)) + 16;
        newv = realloc(sv->base, sizeof(*sv->base) * want);
        if (!newv)
            return(-1);    
        sv->base = newv;
        newl = realloc(sv->lens, sizeof(*sv->lens) * want);
        if (!newl)
            return(-1);    
        sv->lens = newl;
        sv->nalloc = want;
        if (cnt > sv->nalloc - sv->nused)
            err(1, "strvec_append: realloc sanity check failed");
    }

    /* append each string one at a time */
    old_nused = sv->nused;
    old_nbytes = sv->nbytes;
    va_start(ap, sv);
    while ((s = va_arg(ap, char *)) != NULL) {
        sdup = strdupl(s, &dupl);
        if (!sdup) {
            /* roll back to initial state on malloc error */
            for (lcv = old_nused ; lcv < sv->nused ; lcv++) {
                if (sv->base[lcv - 1]) {
                    free(sv->base[lcv - 1]);
                }
            }
            sv->base[old_nused - 1] = NULL;
            sv->lens[old_nused - 1] = 0;
            sv->nused = old_nused;
            sv->nbytes = old_nbytes;
            return(-1);
        }
        sv->base[sv->nused - 1] = sdup;
        sv->lens[sv->nused - 1] = dupl;
        sv->base[sv->nused] = NULL;
        sv->lens[sv->nused] = 0;
        sv->nused++;
        sv->nbytes += dupl;
    }
    va_end(ap);

    return(0);
}

/*
 * flatten strvec into a single malloc'd string with the given
 * prefix, separator, and suffix.  return NULL on error.
 * if the vec is empty, we'll return a malloc'd empty string.
 */
char *strvec_flatten(struct strvec *sv, char *prefix, char *sep,
                     char *suffix) {
    size_t need, plen, seplen, slen, blen;
    int nsep;
    char *rv, *p;

    need = sv->nbytes + 1;     /* +1 for \0 at the end */
    plen = (prefix) ? strlen(prefix) : 0;
    seplen = (sep) ? strlen(sep) : 0;
    slen = (suffix) ? strlen(suffix) : 0;

    /* list of N needs N-1 separators, but last item is NULL, so we do -2 */
    nsep = (sv->nused > 2) ? sv->nused - 2 : 0;

    /* allocate return buffer */
    need += plen + (nsep * seplen) + slen; /* add prefix, seps, suffix space */
    rv = p = malloc(need);
    if (!rv)
        return(NULL);

    /* now we can safely flatten the parts */
    if (plen)
        p = (char *) memcpy(p, prefix, plen) + plen;

    for (int lcv = 0 ; lcv < sv->nused - 1; lcv++) {
        blen = sv->lens[lcv];
        p = (char *) memcpy(p, sv->base[lcv], blen) + blen;
        if (seplen && lcv != sv->nused - 2) {  /* no sep for last entry */
            p = (char *) memcpy(p, sep, seplen) + seplen;
        }
    }
    if (slen)
        p = (char *)memcpy(p, suffix, slen) + slen;
    *p = '\0';
  
    return(rv);
}

/*
 * copy a file.   dst file must not already exist.
 * return 0 on success, -1 on failure.
 */
int copyfile(char *src, char *dst) {
    int sfd = -1, dfd = -1;
    char buf[4096];
    ssize_t rrv, wrv;

    if ((sfd = open(src, O_RDONLY)) < 0)
        return(-1);
    if ((dfd = open(dst, O_WRONLY|O_CREAT|O_EXCL, 0666)) < 0) {
        close(sfd);
        return(-1);
    }

    wrv = 0;
    while ((rrv = read(sfd, buf, sizeof(buf))) > 0) {
        wrv = write(dfd, buf, rrv);
        if (wrv < 0)
            break;
        if (wrv < rrv) {
            wrv = -1;
            errno = ENOSPC;
            break;
        }
    }

    close(sfd);
    if (close(dfd) < 0)
        wrv = -1;
    if (rrv < 0 || wrv < 0) {
        unlink(dst);
        return(-1);
    }
    return(0);
}
