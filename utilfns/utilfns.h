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
 * utilfns.h  some helpful util functions
 * 02-Apr-2025  chuck@ece.cmu.edu
 */

#ifndef _UTILFNS_H_
#define _UTILFNS_H_
#include <stdio.h>
#include <unistd.h>

#include <sys/select.h>
#include <sys/time.h>

/* flags for fdforkprog */
#define FDFPROG_FIN	1	/* update child's stdin */
#define FDFPROG_FOUT	2	/* update child's stdout */
#define FDFPROG_FERR	4	/* update child's stderr */
#define FDFPROG_EDUPOUT 8	/* make child's stderr a dup of its stdout */
#define FDFPROG_FIO     3       /* short for FIN|FOUT */
#define FDFPROG_FIOE    7       /* short for FIN|FOUT|FERR */
#define FDFPROG_FIOD    11      /* short for FIN|FOUT|FEDUPOUT */

/* optional fdforkprog callback function pointer */
typedef void (*fdforkprog_cb)(void *arg);

/*
 * strvec: null terminated vector of strings (e.g. for use with exec*()).
 * the vector array base[], the length array lens[], and each string
 * in base[] are allocated with malloc().  nalloc must be >= nused.
 * once base[] has been allocated nused will start at 1 (for the
 * NULL at the end of the vector).  thus, the base[nused-1] slot is
 * always NULL (so it can safely be passed to exec*()).
 * prior to allocate (nalloc == 0), base and lens are set to NULL.
 */
struct strvec {
    char **base;         /* base address of strvec allocation (malloc'd) */
    size_t *lens;        /* strlen of used entries in base[] (malloc'd) */
    int nalloc;          /* # of slots allocated */
    int nused;           /* # slots used, including NULL at end */
    size_t nbytes;       /* total # bytes allocated slots (not incl \0) */
    int initial_alloc;   /* set initial size of base allocation */
};

#define STRVEC_INITIAL_ALLOC 32
/* non-NULL/0 strvec default init values go here */
#define STRVEC_INIT                                                            \
    (struct strvec) {                                                          \
        .initial_alloc = STRVEC_INITIAL_ALLOC                                  \
    }

/* nanosleep */
#define NSLEEP(S,NS) do {                                                      \
    struct timespec ts;                                                        \
    ts.tv_sec = (S);                                                           \
    ts.tv_nsec = (NS);                                                         \
    nanosleep(&ts, 0);                                                         \
} while (0)

int cmdpathok(char *cmd);
int copyfile(char *from, char *to, int flags);
int dirok(char *dir);
int fdcloexec(int fd, int state);
pid_t fdforkprog(const char *prog, char *const argv[], int flags,
                 int *stdinfd, int *stdoutfd, int *stderrfd,
                 fdforkprog_cb fdf_cb, void *cbarg);
int fdreadreset(int select_rv, fd_set *fds, int fd);
char *loadfile(char *file, off_t *szp);
FILE *logfopen(const char *path, const char *mode);
int mksock_un(int type, const char *path);
int ms_time(struct timeval *base, int set);
int num_match(char *spec, int n);
int prefix_num_match(char *str, int presep, int n, char **restp);
void selmax(int *current, int count, ...);
char *strdupl(const char *str, size_t *lenp);
size_t strgen(char **newstr, ...);
int strvec_append(struct strvec *sv, ...);
char *strvec_flatten(struct strvec *sv, char *prefix, char *sep,
                     char *suffix);
void strvec_free(struct strvec *sv);
int tfinishpid(pid_t pid, int *wstatus, int waitsecs);
int twaitpid(struct timeval *tvwait, int pidspec, int *status);
int vec_search(char *target, int targterm, char **sarray);

#endif /* _UTILFNS_H_ */
