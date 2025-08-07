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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>

#include "mvp_mlog.h"
#include "qemucli.h"

/* ethernet adapters to use for our networks */
#define ADAPTER_MVPNET  "virtio-net-pci"
#define ADAPTER_USERNET "virtio-net-pci"

/* ethernet mac prefix values we use */
#define MACPREFIX_MVPNET  0x5255   /* 52:55 + MPI rank for mvp MPI net */
#define MACPREFIX_USERNET 0x5256   /* 52:56 + MPI world size for usernet */

/*
 * parse qemu-style "argval,prop1=propval1,prop2=propva2,..." args.
 * argval may be omitted (so there is just a list of property values).
 * values end with a comma or a null.  we support pulling out
 * user-defined properties into their own list.  we also support
 * adding default property values to the list (if they were not
 * present in the input arg).
 *
 * XXX: unlike qemu, we currently do not support commas in values with
 * the double comma trick.
 *
 * return 0 on success, -1 on failure.
 * on success, caller must free *argval, props, and pulls.
 */
static int qemucli_procarg(char *arg, char **argval, char **pullouts,
                           char **defaults, struct strvec *props,
                           struct strvec *pulls) {
    char *ap0, *prop, *eq, *val, *endp, *nxtp;
    int goterr, lcv;
    struct strvec *addto;

    *argval = NULL;
    *props = STRVEC_INIT;
    *pulls = STRVEC_INIT;

    for (ap0 = arg, goterr = 0 ; !goterr && *ap0 != '\0' ; ap0 = nxtp) {

        /* parse the next entry */
        prop = eq = NULL;
        val = ap0;          /* could be argval (if val==arg) or prop=propval */

        while (*val && *val != ',' && *val != '=')  /* find end of val */
            val++;

        if (*val != '=') {                         /* it's an argval */
            endp = val;
            val = ap0;
        } else {                                   /* it's a propval */
            prop = ap0;
            eq = val;
            val++;
            endp = val;
            while (*endp && *endp != ',')          /* find end of propval */
                endp++;
        }
        nxtp = (*endp == ',') ? endp + 1 : endp;   /* start for next loop */
        /* parse complete */

        /* only one argval allowed and it must be at the start of arg */
        if (eq == NULL && val != arg) {
            mlog(MVP_CRIT, "qemucli_progarg: bad prop=val: '%.*s'",
                    (int)(endp - val), val);
            goterr++;
            continue;
        }

        /* temporarily null out end points (will be restored) */
        if (eq)
            *eq = '\0';
        if (endp != nxtp)
            *endp = '\0';

        if (eq == NULL) {                          /* handle argval */
            *argval = strdup(val);
            if (*argval == NULL) {
                mlog(MVP_CRIT, "qemucli_progarg: argval strdup failed");
                goterr++;
            }
        } else {                                   /* handle propval */
            /* determine which strvec to append it to */
            addto = (vec_search(prop, 0, pullouts) != -1) ? pulls : props;
            *eq = '=';     /* restore equal now */
            if (strvec_append(addto, prop, NULL) != 0) {
                mlog(MVP_CRIT, "prop/val append malloc fail");
                goterr++;
            }
        }

        /* restore endpoint (eq already restored above) */
        if (endp != nxtp)
            *endp = ',';
    }

    /* add in any defaults to props that did not get an override */
    for (lcv = 0 ; defaults && defaults[lcv] != NULL ; lcv++) {
        if (vec_search(defaults[lcv], '=', props->base) == -1) {
            if (strvec_append(props, defaults[lcv], NULL) != 0) {
                mlog(MVP_CRIT, "default add failed");
                goterr++;
                break;
            }
        }
    }

    if (goterr) {
        if (*argval) {
            free(*argval);
            *argval = NULL;
        }
        strvec_free(props);
        strvec_free(pulls);
    }
    return((goterr) ? -1 : 0);
}

/*
 * helper that builds the network device config string (sets the MAC).
 */
static int build_netdev_cfgstr(char *out, size_t outlen,
                               const char *adapter, const char *netdev,
                               int mac_prefix, int mac_id) {
    int ret;

    ret = snprintf(out, outlen,
                   "%s,netdev=%s,mac=%02x:%02x:%02x:%02x:%02x:%02x",
                   adapter, netdev,
                   (mac_prefix >> 8) & 0xff, mac_prefix & 0xff,
                   (mac_id >> 24) & 0xff, (mac_id >> 16) & 0xff,
                   (mac_id >> 8) & 0xff, mac_id & 0xff);
    mlog(MVP_DBG, "netdev_cfg: %s%s", out,
        (ret >= outlen) ? " TRUNCATED!" : "");
    return ret;
}

/*
 * insert cloudinit info into the storage qvec when cloudinit has
 * been enabled.   return 0 on success, -1 on error.
 */
int qemucli_storage_cloudinit(struct strvec *qvec, char *cloudinit) {
    struct stat st;
    char *spec;
    int rv;

    if (stat(cloudinit, &st) < 0 ||
        (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
        mlog(MVP_ERR, "qemucli: cloudinit %s failed", cloudinit);
        return(-1);
    }

    if (strgen(&spec, "file=",
               (S_ISDIR(st.st_mode)) ? "fat:" : "", cloudinit,
               (S_ISDIR(st.st_mode)) ? ",file.label=cidata" : "",
               ",snapshot=on,media=disk,if=virtio", NULL) < 1) {
        mlog(MVP_CRIT, "strgen: cloudinit");
        return(-1);
    }

    rv = strvec_append(qvec, "-drive", spec, NULL);
    free(spec);    /* always need to free this */

    return((rv != 0) ? -1 : 0);
}

/*
 * apply mvpctl processing to a single image file and append its
 * corresponding storage config info to the qemu command line
 * vector.
 *
 * return 0 on success, -1 on failure.
 */
static int qemucli_storage_img(struct strvec *qvec, char *jobtag,
                               char *ranktag, char *img, char *rundir,
                               struct strvec *tmps, char *mvpctl,
                               struct strvec *mains) {
    int rv = -1;   /* assume failure */
    char *img_extdot, *dot, *ext, *image_file, *runimage_file, *cp;
    char *boot_image, *mainargs, *driveargs;

    /* first generate filenames as-per mvpctl settings */
    img_extdot = strrchr(img, '.');    /* look for ending extension */
    if (img_extdot == NULL) {          /* no extension? */
        dot = "";
        ext = "";
    } else {
        dot = ".";
        ext = img_extdot + 1;
        *img_extdot = '\0';            /* overwrite '.' in img */
    }
    image_file = runimage_file = NULL;
    mainargs = driveargs = NULL;

    if (strgen(&image_file, img,
               (strchr(mvpctl, 'J')) ? jobtag : "",
               (strchr(mvpctl, 'R')) ? ranktag : "", dot, ext, NULL) < 1) {
        mlog(MVP_CRIT, "strgen: image_file");
        goto done;
    }

    if (strchr(mvpctl, 'c') != NULL) {
        if (rundir == NULL) {
            mlog(MVP_ERR, "qemucli: mvpctl 'c' given, but no rundir!");
            goto done;
        }
        cp = strrchr(img, '/');        /* get just filename from img */
        cp = (cp) ? cp + 1 : img;      /* skip the '/' if present */

        if (strgen(&runimage_file, rundir, "/", cp,
                   (strchr(mvpctl, 'j')) ? jobtag : "",
                   (strchr(mvpctl, 'r')) ? ranktag : "", dot, ext, NULL) < 1) {
            mlog(MVP_CRIT, "strgen: runimage_file");
            goto done;
        }
    }

    /* if we have a runimage_file, we try and copy the image there */
    if (runimage_file) {
        if (copyfile(image_file, runimage_file, O_CREAT|O_EXCL) < 0) {
            /*
             * allow EEXIST -- assume some other proc has already
             * copied the file for us.  we'll just use that copy.
             */
            if (errno != EEXIST) {
                mlog(MVP_ERR, "copyfile: %s to %s (%s)", image_file,
                    runimage_file, strerror(errno));
                goto done;
            }
            mlog(MVP_NOTE, "copyfile(%s => %s) - file already present",
                 image_file, runimage_file);
        } else {
            mlog(MVP_NOTE, "copyfile(%s => %s) - success!",
                 image_file, runimage_file);
        }
        if (strchr(mvpctl, 'd') &&
            strvec_append(tmps, runimage_file, NULL) != 0) {
            mlog(MVP_CRIT, "tmps runimage_file failed");
            goto done;
        }
    }

    /* register image_file for removal if requested */
    if (strchr(mvpctl, 'D') && strvec_append(tmps, image_file, NULL) != 0) {
        mlog(MVP_CRIT, "tmps image_file failed");
        goto done;
    }

    /* generate qemu -drive arg for this image */
    boot_image = (runimage_file) ? runimage_file : image_file;

    if (mains->nused) {
        mainargs = strvec_flatten(mains, ",", ",", NULL);
        if (!mainargs) {
            mlog(MVP_CRIT, "mainargs alloc failed");
            goto done;
        }
    }

    if (strgen(&driveargs, "file=", boot_image,
               (strchr(mvpctl, 'p')) ? ",read-only=on" : "",
               (strchr(mvpctl, 's')) ? ",snapshot=on" : "",
               (mainargs) ? mainargs : "", NULL)  < 1) {
        mlog(MVP_CRIT, "driveargs alloc failed");
        goto done;
    }

    if (strvec_append(qvec, "-drive", driveargs, NULL) != 0) {
        mlog(MVP_CRIT, "drive qvec append alloc failed");
        goto done;
    }

    /* success! */
    rv = 0;

done:
    if (img_extdot)
        *img_extdot = '.';             /* restore '.' in img */
    if (image_file)
        free(image_file);
    if (runimage_file)
        free(runimage_file);
    if (mainargs)
        free(mainargs);
    if (driveargs)
        free(driveargs);
    return(rv);
}

/*
 * append storage config to qemu command line vector.
 * return 0 on success, -1 on failure.
 */
static int qemucli_storage_cfg(struct strvec *qvec, char *jobtag,
                               char *ranktag, struct strvec *imgs,
                               char *rundir, char *cloudinit,
                               struct strvec *tmps) {
    static char *pullout[] = { "mvpctl", NULL };
    static char *defaults[] = { "media=disk", "if=virtio", NULL };
    int lcv, rv;
    char *argval, *mvpctl;
    struct strvec mains, pulls;

    /* process each -i in order given */
    for (lcv = 0 ; imgs->base[lcv] != NULL ; lcv++) {

        rv = qemucli_procarg(imgs->base[lcv], &argval, pullout,
                             defaults, &mains, &pulls);
        if (rv == -1)
            return(rv);      /* mloged elsewhere */

        /*
         * if there is no argval in the current "-i" then it means
         * that the user wants to pass everything through as-is
         * to qemu's -drive flag without any mvpctl processing by us.
         */
        if (argval == NULL) {
            rv = (pulls.nused == 0) ? 0 : -1;  /* ensure no mvpctl */
            strvec_free(&mains);
            strvec_free(&pulls);
            if (rv == -1) {
                mlog(MVP_ERR, "mvpctl on qemu passthru: %s", imgs->base[lcv]);
                return(-1);
            }
            if (strvec_append(qvec, "-drive", imgs->base[lcv], NULL) != 0) {
                mlog(MVP_CRIT, "malloc/append to qvec failed!");
                return(-1);
            }

        } else {

            /*
             * argval is an image filename that we will apply mvpctl to
             * before adding the info to qvec.  determine mvpctl to use.
             */
            if (pulls.nused == 0) {   /* no mvpctl provided by user? */
                mvpctl = (rundir == NULL) ? "" : "cjrd";
            } else {
                mvpctl = pulls.base[0];   /* need to skip over 'mvpopts=' */
                while (*mvpctl && *mvpctl != '=')
                    mvpctl++;
                if (*mvpctl == '=')       /* skip the '=' too */
                    mvpctl++;
            }

            rv = qemucli_storage_img(qvec, jobtag, ranktag, argval,
                                     rundir, tmps, mvpctl, &mains);
            free(argval);
            strvec_free(&mains);
            strvec_free(&pulls);
            if (rv == -1)
                return(rv);               /* logged elsewhere */
        }

        /* if we have cloudinit data, insert it after the first entry */
        if (lcv == 0 && cloudinit) {
            rv = qemucli_storage_cloudinit(qvec, cloudinit);
            if (rv == -1)
                return(rv);      /* mloged elsewhere */
        }
    }

    return(0);
}

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

    if (build_netdev_cfgstr(dev, sizeof(dev), ADAPTER_MVPNET, "mvpnet",
                            MACPREFIX_MVPNET, rank) >= sizeof(dev))
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
static int qemucli_usernet_cfg(struct strvec *qvec, int rank, int wsize,
                               struct strvec *domains, char *tftpdir,
                               int localport) {
    int rv = -1;
    char as_hostname[32], as_hostfwd[64], dev[64];
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
    if (build_netdev_cfgstr(dev, sizeof(dev), ADAPTER_USERNET, "usernet",
                            MACPREFIX_USERNET, wsize) >= sizeof(dev))
        goto done;

    /* generate string and append cfg */
    if (strgen(&usernet_spec, "user,id=usernet,net=192.168.1.0/24",
               ",hostname=", as_hostname, acmd, tcmd, tval,
               ",hostfwd=", as_hostfwd, NULL) > 0 &&
        strvec_append(qvec, "-netdev", usernet_spec, "-device",
                      dev, NULL) == 0) {
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
void qemucli_gen(struct qemucli_args *qa, struct strvec *tmps,
                 struct strvec *qvec) {
    char mbuf[32];

    /* start command line with wrapper and its args */
    if (strvec_append(qvec, qa->mopt->monwrap, NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qvec setup monwrap");
    if (qa->wraplog && strvec_append(qvec, "-l", qa->wraplog, NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup monwrap log");

    /* qemu command and flags to set a serial console */
    if (strvec_append(qvec, qa->mopt->qemucmd, "-serial", "mon:stdio",
                       "-nographic", "-display", "none",
                       "-vga", "none", "-cpu", qa->mopt->processor,
                       NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup serial");

    /* kvm option */
    if (qa->mopt->kvm && strvec_append(qvec, "-enable-kvm", NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup kvm");

    /* guest memory size */
    snprintf(mbuf, sizeof(mbuf), "%dM", qa->mopt->mem_mb);
    if (strvec_append(qvec, "-m", mbuf, NULL) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup memory");

    /* storage configuration */
    if (qemucli_storage_cfg(qvec, qa->jobtag, qa->ranktag, &qa->mopt->image,
                            qa->mopt->rundir, qa->mopt->cloudinit,
                            tmps) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup storage");

    /* usernet configuration */
    if (qemucli_usernet_cfg(qvec, qa->mi.rank, qa->mi.wsize, &qa->mopt->domain,
                            qa->mopt->tftpdir, qa->localport) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup usernet");

    /* socknet configuration */
    if (qemucli_socknet_cfg(qvec, qa->mi.rank, qa->mopt->nettype,
                            qa->socknames) != 0)
        mlog_exit(1, MVP_CRIT, "qemuvec setup socknet");

    /*
     * done!
     */
}
