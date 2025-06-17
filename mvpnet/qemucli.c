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
 * qemucli.c  generate qemu command line args for execvp()
 * 13-May-2025  chuck@ece.cmu.edu
 */

/*
 * all the string handling operations needed to build the command
 * line argv[] array for running qemu are consolidated into this
 * file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

#include "mvp_mlog.h"
#include "qemucli.h"

/*
 * append socknet config to qemu command line vector.
 * return 0 on success, -1 on failure.
 */
static int qemucli_socknet_cfg(struct strvec *qvec, int rank, int nettype,
                        char **socks) {
    int rv = -1;
    char *as_type, *pcmd[2], *pval[2], dev[64];
    char *socknet_spec = NULL;

    if (nettype == SOCK_STREAM) {
        as_type = "stream";
        pcmd[0] = ",addr.type=unix,addr.path=";
        pval[0] = socks[0];
        pcmd[1] = pval[1] = "";
    } else if (nettype == SOCK_DGRAM) {
        as_type = "dgram";
        pcmd[0] = ",local.type=unix,local.path=";
        pval[0] = socks[1];
        pcmd[1] = ",remote.type=unix,remote.path=";
        pval[1] = socks[0];
    } else {
        mlog_exit(1, MVP_CRIT, "append_socknet_cfg: bad nettype %d", nettype);
    }

    if (snprintf(dev, sizeof(dev),
        "e1000,netdev=mvpnet,mac=52:55:%02x:%02x:%02x:%02x",
        (rank >> 24) & 0xff, (rank >> 16) & 0xff, (rank >> 8) & 0xff,
        rank & 0xff) >= sizeof(dev))
        goto done;

    /* generate string and append cfg */
    if (strgen(&socknet_spec, as_type, ",id=mvpnet",
               pcmd[0], pval[0], pcmd[1], pval[1], NULL) > 0 &&
        strvec_append(qvec, "-netdev", socknet_spec,
                      "-device", dev, NULL) == 0) {
        rv = 0;
    }

done:
    if (socknet_spec)
        free(socknet_spec);
    return(rv);
}

/*
 * get domain config info from /etc/resolv.conf for usernet config.
 * the resolver only honors the last domain or search line in the
 * file (previous lines are discarded), so we follow that.   we also
 * try and filter out systems that don't use /etc/resolv.conf in the
 * traditional way.  return 0 on success, -1 on failure.
 */
static int get_resolvconf(struct strvec *domains) {
    int rv = 0;          /* default == success */
    char *rslv = NULL;
    char *rp, *last, *lp, *nxt;

    rslv = loadfile("/etc/resolv.conf", NULL);
    if (!rslv)
        goto done;   /* just move on if we can't load the file */

    /* find last 'search' or 'domain' line in the file */
    rp = rslv;
    last = NULL;
    while (*rp) {
        while (*rp == ' ' || *rp == '\t')   /* skip leading space */
            rp++;
        /* check for keyword */
        if (strncmp(rp, "domain ", 7) == 0 ||
            strncmp(rp, "domain\t", 7) == 0 ||
            strncmp(rp, "search ", 7) == 0 ||
            strncmp(rp, "search\t", 7) == 0) {
            last = rp;
        }
        while (*rp && *rp != '\n')   /* skip to next line */
            rp++;
        if (*rp == '\n')             /* turn all EOLs to end of string */
            *rp++ = '\0';
    }

    if (!last)
        goto done;                   /* no useful lines in the file */

    /* tokenize the line and append each domain to 'domains' */
    for (lp = last + sizeof("domain ") - 1 ; *lp ; lp = nxt) {
        while (*lp && (*lp == ' ' || *lp == '\t'))     /* skip spaces */
            lp++;
        nxt = lp;
        while (*nxt && (*nxt != ' ' && *nxt != '\t'))  /* end of token */
            nxt++;
        if (*nxt)                                      /* terminate it */
            *nxt++ = '\0';
        if (*lp && strcmp(lp, ".") != 0) {
            if (strvec_append(domains, lp, NULL) != 0) {
                rv = -1;
                goto done;
            }
        }
    }

done:
    if (rslv)
        free(rslv);
    return(rv);

}

/*
 * return qemu guest dns config as a malloced string for usernet config.
 * return 0 on success, -1 on failure.
 */
static int get_domains(struct strvec *domains, char **dstringp) {
    char *rv;

    *dstringp = NULL;

    /* if no '-d' on command line, try /etc/resolv.conf */
    if (domains->base == NULL && get_resolvconf(domains) != 0)
        return(-1);

    if (domains->nused < 2 || domains->nbytes == 0)   /* defined, but empty */
        return(0);

    rv = strvec_flatten(domains, ",dnssearch=", ",dnssearch=", NULL);
    if (!rv)
        return(-1);

    *dstringp = rv;
    return(0);

}

/*
 * append usernet config to qemu command line vector.
 * return 0 on success, -1 on failure.
 */
static int qemucli_usernet_cfg(struct strvec *qvec, int rank,
                               struct strvec *domains, char *tftpdir,
                               int localport) {
    int rv = -1;
    char as_hostname[32], as_hostfwd[64];
    char *as_domains = NULL;     /* malloced */
    char *acmd, *tcmd, *tval;
    char *usernet_spec = NULL;

    /* assemble all the parts */
    if (snprintf(as_hostname, sizeof(as_hostname), "n%04d",
                 rank) >= sizeof(as_hostname))
        goto done;
    if (snprintf(as_hostfwd, sizeof(as_hostfwd), "tcp:127.0.0.1:%d-:22",
             localport) >= sizeof(as_hostfwd))
        goto done;
    if (get_domains(domains, &as_domains) != 0)
        goto done;
    acmd = (as_domains) ? as_domains : "";
    if (!tftpdir) {
        tcmd = tval = "";
    } else {
        tcmd = ",tftp=";
        tval = tftpdir;
    }

    /* generate string and append cfg */
    if (strgen(&usernet_spec, "user,id=usernet,net=192.168.1.0/24",
               ",hostname=", as_hostname, acmd, tcmd, tval,
               ",hostfwd=", as_hostfwd, NULL) > 0 &&
        strvec_append(qvec, "-netdev", usernet_spec, "-device",
                      "e1000,netdev=usernet,mac=52:54:98:76:54:32",
                      NULL) == 0) {
        rv = 0;
    }

done:
    if (as_domains)
        free(as_domains);
    if (usernet_spec)
        free(usernet_spec);
    return(rv);
}

/*
 * top-level function used to generate the qemu command line.
 * we build the command line argv[] array in a strvec.
 * we will error out on failure...
 */
void qemucli_gen(struct qemucli_args *qa, struct strvec *qvec) {
    char *bootdrive, mbuf[32];

    /* generate bootdrive option string */
    if (strgen(&bootdrive, "file=", qa->bootimg,
               ",media=disk,if=virtio", NULL) < 1)
        mlog_exit(1, MVP_CRIT, "strgen: bootdrive");

    /* start command line with wrapper and its args */
    if (strvec_append(qvec, qa->mopt->monwrap, NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qvec setup monwrap");
    if (qa->wraplog && strvec_append(qvec, "-l", qa->wraplog, NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup monwrap log");

    /* qemu command and flags to set a serial console */
    if (strvec_append(qvec, qa->mopt->qemucmd, "-serial", "mon:stdio",
                       "-nographic", "-display", "none",
                       "-vga", "none", NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup serial");

    /* kvm option */
    if (qa->mopt->kvm && strvec_append(qvec, "-enable-kvm", NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup kvm");

    /* guest memory size and bootdrive */
    snprintf(mbuf, sizeof(mbuf), "%dM", qa->mopt->mem_mb);
    if (strvec_append(qvec, "-m", mbuf,
                       "-drive", bootdrive, NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup bootdrive");

    /* usernet configuration */
    if (qemucli_usernet_cfg(qvec, qa->mi.rank, &qa->mopt->domain,
                            qa->mopt->tftpdir, qa->localport) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup usernet");

    /* socknet configuration */
    if (qemucli_socknet_cfg(qvec, qa->mi.rank, qa->mopt->nettype,
                            qa->socknames) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup socknet");

    /*
     * done!  we can free bootdrive since it was copied into qvec.
     */
    free(bootdrive);
}
