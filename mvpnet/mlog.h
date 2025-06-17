/*
 * Copyright (c) 2017, Carnegie Mellon University.
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
 * mlog.h  define API for multilog message logging system
 * 08-Apr-2004  chuck@ece.cmu.edu
 */
#ifndef _MLOG_H_
#define _MLOG_H_

/*
 * mlog is an application-level logging system.  It does not require a
 * system logging daemon (e.g. syslogd) to operate.  Log message are
 * filtered using syslog-style priority levels and facility numbers.
 * Log messages that are not filtered out can be sent to multiple
 * outputs.  The possible output targets are a log file, stderr,
 * stdout, syslog(3), UDP datagrams, and an in-memory circular message
 * buffer.  The contents of the message buffer can be recovered from a
 * core dump if the program crashes.
 *
 * different mlog facility numbers are typically assigned to various
 * internal application subsystems to make logging easier to filter.
 * this differs from syslog(3) where all log messages are issued under
 * a single facility number established by an openlog(3) call.
 *
 * filtering: all log messages have a 16 bit facility number and a
 * priority.  each facility has its own log filtering mask that is
 * applied to a log's priority to determine if the mlog call should be
 * added to the log or if it should be filtered out.  the priority
 * levels mostly match syslog: EMERG, ALERT, CRIT, ERR, WARN, NOTE,
 * INFO, and DBG.  The debug (DBG) level can optionally be subdivided
 * into 4 channels for additional filtering options (i.e. you can
 * selectively enable/disable 4 different classes of debug logs).
 * Example: if the mask for a facility is set to WARN, then log
 * messages at the WARN priority or higher will be logged and those
 * below the WARN priority will be discarded.
 *
 * In addition to the facility mask used for filter, mlog also has a
 * global "stderr_mask" -- any log message that passes the filtering
 * step (above) and is at a priority level greater than or equal to
 * the stderr_mask will be always be printed to stderr.  Applications
 * can force log messages that are not filtered out to be printed to
 * stderr (or stdout) regardless of the setting of stderr_mask by
 * adding the MLOG_STDERR flag (or MLOG_STDOUT flag) to the log call.
 *
 * Rather than forcing users to use facility numbers, mlog allows you
 * to assign user-defined names to facilities.  each facility has two
 * names: a short name that is printed to the logs and an optional
 * longer name that can be used as a more human-friendly alias for the
 * short name.  For example, for a routing module the short name could
 * be "RTG" and the long alias name could be "ROUTING" ... if you
 * don't need two names, just the use short one.  The mlog_setmasks()
 * call will take either name.
 *
 * typical usage:
 *  [1] copy mlog.c and mlog.h into your project (no need for separate lib!)
 *  [2] C++ users can put mlog in a namespace by defining MLOG_NSPACE
 *      and compiling with a c++ compiler (maybe rename mlog.c to mlog.cc?)
 *  [3] determine the number of facilities your project needs
 *      and what names you want to use for them.
 *  [4] create a project-specific foo_mlog.h file that #defines shortcut
 *      names for each facility's log levels you want to use.  e.g.
 *        #define RTG_MLOG 1                   // facility 1 is for routing
 *        #define RTG_ERR (RTG_MLOG|MLOG_ERR)
 *        #define RTG_WARN (RTG_MLOG|MLOG_WARN)
 *           // etc.. for anything else you are going to use
 *      it is often helpful to write a small python/perl script
 *      to generate all these types of defines.
 *  [5] in your main() you'll first want to add a call to mlog_open()
 *      to open the log and define all the default mlog settings.
 *      you'll then want to call mlog_namefacility() to establish names
 *      for each facility you are using.
 *  [6] you can allow users to modify the default filtering settings
 *      by adding a flag or config option that you can pass to
 *      mlog_setmasks().
 *  [7] if you need to reopen the log (e.g. for log rotation) add
 *      a call to mlog_reopen() to your code.
 *  [8] you can call mlog_close() if you need to close the log.
 *
 * with that setup done, you can start adding calls to mlog() in your
 * code.  e.g.
 *
 *    mlog(RTG_WARN, "route %p (id=%d) may be bad", rt, rt->id);
 *
 * compile time options:
 *
 *   - MLOG_NSPACE can be defined to put mlog in a c++ namespace
 *
 *   - MLOG_NOMACRO_OPT turns off having mlog() be a preprocessor
 *     macro.  having mlog() as a macro allows the filter to be inline
 *     but requires the preprocessor support __VA_ARGS__.
 *
 *   - MLOG_NEVERLOG provides an easy way to completely compile out
 *     all mlog() calls (including the filter) for a performance or
 *     production build where you don't want any mlog() calls to fire.
 *     note that setting MLOG_NEVERLOG is honored only if MLOG_NOMACRO_OPT
 *     is not set (since NEVERLOG is done via preprocessor macro opts).
 *
 *   - MLOG_MINPRIORITY provides an easy way to compile out all
 *     mlog() calls below MLOG_MINPRIORITY.  e.g. if you set
 *     MLOG_MINPRIORITY=MLOG_ERR any mlog call at a priority less
 *     than MLOG_ERR will completely compiled out of the program
 *     and can never generate logs (no matter the masking level).
 *
 *   - MLOG_MUTEX: set to 0 to disable mlog pthread mutex protection
 *     for mlog data structures (e.g. for single threaded programs).
 *     mutex is enabled by default.
 */

#include <stdarg.h>    /* need va_list for vmlog */

/*
 * mlog flag values.  includes the priority and facility number.
 */

/* mlog_open() bits.  MLOG_STDERR/MLOG_STDOUT can be used with mlog() too */
#define MLOG_STDERR     0x80000000      /* always log to stderr */
#define MLOG_UCON_ON    0x40000000      /* enable UDP console on mlog_open */
#define MLOG_UCON_ENV   0x20000000      /* get UCON list from $MLOG_UCON */
#define MLOG_SYSLOG     0x10000000      /* syslog(3) the messages as well */
#define MLOG_LOGPID     0x08000000      /* include pid in log tag */
#define MLOG_FQDN       0x04000000      /* log fully quallified domain name */
#define MLOG_STDOUT     0x02000000      /* always log to stdout */
#define MLOG_OUTTTY     0x01000000      /* always short 'tty' hdr for stdout */
/* spare bit: 0x00800000 */
/* the remaining bits are used for the flag arg in the mlog() call */
#define MLOG_PRIMASK    0x007f0000      /* priority mask */
#define MLOG_EMERG      0x00700000      /* emergency */
#define MLOG_ALERT      0x00600000      /* alert */
#define MLOG_CRIT       0x00500000      /* critical */
#define MLOG_ERR        0x00400000      /* error */
#define MLOG_WARN       0x00300000      /* warning */
#define MLOG_NOTE       0x00200000      /* notice */
#define MLOG_INFO       0x00100000      /* info */
#define MLOG_PRISHIFT   20              /* to get non-debug level */
#define MLOG_DPRISHIFT  16              /* to get debug level */
#define MLOG_DBG        0x000f0000      /* all debug streams */
#define MLOG_DBG0       0x00080000      /* debug stream 0 */
#define MLOG_DBG1       0x00040000      /* debug stream 1 */
#define MLOG_DBG2       0x00020000      /* debug stream 2 */
#define MLOG_DBG3       0x00010000      /* debug stream 3 */
#define MLOG_FACMASK    0x0000ffff      /* facility mask */

/* portable define for common printf formatting/noreturn compiler checks */
#if defined(__GNUC__) && (__GNUC__ > 2)          /* also works for clang */
#define xnoreturn()     __attribute__((__noreturn__))
#define xprintfattr(fmtarg, firstvararg) \
        __attribute__((__format__ (__printf__, fmtarg, firstvararg)))
#else
#define xnoreturn()
#define xprintfattr(fmtarg, firstvararg)
#endif

#if defined(__cplusplus)

/*
 * for C++ we can either use a C-style interface (so there is only one
 * instance of mlog in the program), or we can have multiple instances
 * of mlog each in their own private namespaces.  for the latter,
 * define MLOG_NSPACE to the name of the namespace to use...
 */
#ifdef MLOG_NSPACE
#define MLOG_BEGIN_DECLS namespace MLOG_NSPACE {
#define MLOG_END_DECLS }
#else
#define MLOG_BEGIN_DECLS extern "C" {
#define MLOG_END_DECLS }
#endif

#else /* __cplusplus */

/* no additional help required for plain C */
#define MLOG_BEGIN_DECLS
#define MLOG_END_DECLS

#endif /* __cplusplus */

MLOG_BEGIN_DECLS

/*
 * structures: not really part of the external API, but exposed here
 * so we can do the priority filter (mlog_filter) in a macro before
 * calling mlog() ... no point evaluating the mlog() args if the
 * filter is going to filter log out...
 */

/**
 * mlog_fac: facility name and mask info
 */
struct mlog_fac {
    int fac_mask;       /*!< log mask for this facility */
    char *fac_sname;    /*!< facility short name [malloced] */
    char *fac_lname;    /*!< optional facility long name [malloced] */
};

/**
 * mlog_xstate: exposed global state... just enough to filter logs.
 */
struct mlog_xstate {
    char *tag;                   /*!< tag string [malloced] */
    /* note that tag is NULL if mlog is not open/inited */
    struct mlog_fac *mlog_facs;  /*!< array of facility info [malloced] */
    int fac_cnt;                 /*!< # of facilities we are using */
    char *nodename;              /*!< pointer to our utsname */
};

/*
 * API prototypes and inlines
 */

/**
 * mlog_filter: determine if we should log a message based on its priority
 * and the current mask level for the facility.   flags is typically a
 * constant, so the C optimizer should be able to reduce this inline
 * quite a bit.  no locking for threaded environment here, as we assume
 * fac_cnt can only grow larger and threaded apps will shutdown threads
 * before doing a mlog_close.
 *
 * @param flags the MLOG flags
 * @return 1 if we should log, 0 if we should filter
 */
static inline int mlog_filter(int flags) {
    extern struct mlog_xstate mlog_xst;
    unsigned int fac, lvl, mask;
    /* first, ensure mlog is open */
    if (!mlog_xst.tag) {
        return(0);
    }
    /* get the facility and level of this log message */
    fac = flags & MLOG_FACMASK;
    lvl = flags & MLOG_PRIMASK;
    /*
     * check for valid facility.  if it is not valid, then we convert
     * it to the default facility because that seems like a better thing
     * to do than drop the message.
     */
    if (fac >= (unsigned)mlog_xst.fac_cnt) {
        fac = 0;    /* 0 == default facility */
    }
    /* now we can get the mask we need */
    mask = mlog_xst.mlog_facs[fac].fac_mask;
    /* for non-debug logs we directly compare the mask and level */
    if (lvl >= MLOG_INFO) {
        return( (lvl < mask) ? 0 : 1);
    }
    /*
     * for debugging logs, we check the channel mask bits.  applications
     * that don't use debugging channels always log with all the bits set.
     */
    return( (lvl & mask) == 0 ? 0 : 1);
}

/**
 * vmlog: core log function, normally front-ended by
 * mlog/mlog_abort/mlog_exit.   we expose it here to allow
 * users to create additional frontends.
 *
 * @param flags the flags (mainly fac+pri) for this log message
 * @param fmt the printf(3) format to use
 * @param ap the stdargs va_list to use for the printf format
 */
void vmlog(int flags, const char *fmt, va_list ap);

/**
 * mlog: multilog a message... generic wrapper for the the core vmlog
 * function.  note that a log line cannot be larger than MLOG_TBSZ (4096)
 * [if it is larger it will be (silently) truncated].   facility should
 * allocated with mlog_open(), mlog_namefacility(), mlog_allocfacility(),
 * or mlog_setlogmask() before being used (or the logs will get converted
 * to the default facility, #0).
 *
 * @param flags facility+level+misc flags
 * @param fmt printf-style format string
 * @param ... printf-style args
 */
void mlog(int flags, const char *fmt, ...)
    xprintfattr(2, 3);

/**
 * mlog_abort: like mlog, but does an abort after processing the log.
 * for aborts, we always log to STDERR.
 *
 * @param flags facility+level+misc flags
 * @param fmt printf-style format string
 * @param ... printf-style args
 */
void mlog_abort(int flags, const char *fmt, ...)
    xprintfattr(2, 3) xnoreturn();

/*
 * mlog_aborthook_t: typedef for abort hook function
 */
typedef void (*mlog_aborthook_t)(void);

/**
 * mlog_abort_hook: establish an abort "hook" to call before doing
 * an abort (e.g. a hook to print the stack, or save some debug info).
 *
 * @param hook the abort hook function
 * @return the old hook (NULL if there wasn't one)
 */
mlog_aborthook_t mlog_abort_hook(mlog_aborthook_t newhook);

/**
 * mlog_allocfacility: allocate a new facility with the given name.
 * this function picks a free facility number for you.  in most
 * cases it is likely to be better to assign your own facility numbers
 * and use mlog_namefacility() so that log levels can be fixed constant
 * values that the compiler can optimize with.
 *
 * @param sname the short name for the facility - can be null for no name
 * @param lname the long name for the new facility - can be null for no name
 * @return new facility number on success, -1 on error - malloc problem.
 */
int mlog_allocfacility(char *sname, char *lname);

/**
 * mlog_close: close off a mlog and release any allocated resources.
 * if already close, this function is a noop.   if a program is just
 * exiting, it doesn't have to call this --- exit(3) will cleanup too.
 */
void mlog_close(void);

/**
 * mlog_dmesg: obtain pointers to the current contents of the message
 * buffer.   since the message buffer is circular, the result may come
 * back in two pieces.  note that this function returns pointers into
 * the live message buffer, so the app had best not call mlog again
 * until it is done with the pointers.   gives applications the option
 * of logging just to memory (fast) and then recovering the log for
 * saving or printing if an error occurs.
 *
 * @param b1p returns pointer to first buffer here
 * @param b1len returns length of data in b1
 * @param b2p returns pointer to second buffer here (null if none)
 * @param b2len returns length of b2 or zero if b2 is null
 * @return 0 on success, -1 on error (not open, or no message buffer)
 */
int mlog_dmesg(char **b1p, int *b1len, char **b2p, int *b2len);

/**
 * mlog_mbcount: give hint as to current size of message buffer.
 * (buffer size may change if mlog is called after this...)
 *
 * @return number of bytes in msg buffer (zero if empty/disabled)
 */
int mlog_mbcount(void);

/**
 * mlog_mbcopy: safely copy the most recent bytes of the message buffer
 * over into another buffer for use.
 *
 * @param buf buffer to copy to
 * @param offset offset in message buffer (0 to start at the end)
 * @param len length of the buffer
 * @return number of bytes copied (<= len), or -1 on error
 */
int mlog_mbcopy(char *buf, int offset, int len);

/**
 * mlog_exit: like mlog, but exits with the given status after processing
 * the log.   we always log to STDERR.
 *
 * @param status the value to exit with
 * @param flags facility+level+misc flags
 * @param fmt printf-style format string
 * @param ... printf-style args
 */
void mlog_exit(int status, int flags, const char *fmt, ...)
    xprintfattr(3, 4) xnoreturn();

/**
 * mlog_findmesgbuf: search for a message buffer inside another buffer
 * (typically a mmaped core file).  if there are multiple instances
 * of mlog in a program (e.g. via c++ name spaces) then multiple searches
 * may be required to get all the message buffers.
 *
 * @param b the buffer to search
 * @param len the length of the buffer b
 * @param b1p returns pointer to first buffer here
 * @param b1l returns length of data in b1
 * @param b2p returns pointer to second buffer here (null if none)
 * @param b2l returns length of b2 or zero if b2 is null
 * @return 0 on success, -1 on error
 */
int mlog_findmesgbuf(char *b, int len, char **b1p, int *b1l,
                     char **b2p, int *b2l);

/**
 * mlog_namefacility: assign a name to a facility.   since the facility
 * number is used as an index into an array, don't choose large numbers.
 *
 * @param facility the facility to name
 * @param sname the new short name, or null to remove the name
 * @param lname optional long name (null if not needed)
 * @return 0 on success, -1 on error (malloc problem).
 */
int mlog_namefacility(int facility, const char *sname, const char *lname);

/**
 * mlog_open: open a multilog (uses malloc).  you can only have one
 * multilog open at a time (unless you use c++ namespaces).   you
 * can use multiple facilities with a mlog.  if an mlog is already open,
 * then this call will fail.  if you use an in-memory message buffer, you
 * will prob want it to be 1K or larger.
 *
 * @param tag string we tag each line with, optionally followed by pid
 * @param maxfac_hint hint on the number of facilities we will use
 * @param default_mask the default mask to use for each facility
 * @param stderr_mask print unfiltered logs above this level to stderr
 *        If this is 0, then output goes to stderr only if MLOG_STDERR
 *        is used (either in mlog_open or in mlog).
 * @param logfile log file name, or null if no log file
 * @param msgbuf_len size of message buffer, or zero if no message buffer
 * @param flags STDERR, UCON_ON, UCON_ENV, SYSLOG, LOGPID
 * @param syslogfac syslog facility to use if MLOG_SYSLOG is set in flags
 * @return 0 on success, -1 on error.
 */
int mlog_open(const char *tag, int maxfac_hint, int default_mask,
              int stderr_mask, char *logfile, int msgbuf_len,
              int flags, int syslogfac);

/**
 * mlog_str2pri: convert a priority string to an int pri value to allow
 * for more user-friendly programs.
 *
 * @param pstr the priority string
 * @return -1 (an invalid pri) on error.
 */
int mlog_str2pri(const char *pstr);

/**
 * mlog_reopen: reopen a multilog.  this will reopen the log file,
 * reset the ucon socket (if on), and refresh the pid in the tag (if
 * present).   call this to rotate log files or after a fork (which
 * changes the pid and may also close fds).    the logfile param should
 * be set to a zero-length string ("") to keep the old value of logfile.
 * if the logfile is NULL, then any open logfiles will be switched off.
 * if the logfile is a non-zero length string, it is the new logfile name.
 *
 * @param logfile settings for the logfile after reopen (see above).
 * @return 0 on success, -1 on error.
 */
int mlog_reopen(char *logfile);

/**
 * mlog_setlogmask: set the logmask for a given facility.  if the user
 * uses a new facility, we ensure that our facility array covers it
 * (expanding as needed).
 *
 * @param facility the facility we are adjusting (16 bit int)
 * @param mask the new mask to apply
 * @return the old mask val, or -1 on error.  cannot fail if facility array
 * was preallocated.
 */
int mlog_setlogmask(int facility, int mask);

/**
 * mlog_setmasks: set mlog masks for a set of facilities to a given level.
 * the input string should look like: PREFIX1=LEVEL1,PREFIX2=LEVEL2,...
 * where the "PREFIX" is either short/long facility name defined with
 * mlog_allocfacility() or mlog_namefacility().
 *
 * @param mstr settings to use (doesn't have to be null term'd if mstr >= 0)
 * @param mlen length of mstr (if < 0, assume null terminated, use strlen)
 */
void mlog_setmasks(char *mstr, int mlen);

/**
 * mlog_getmasks: get current mask level as a C string.
 * if the buffer is null, we probe for length rather than fill.
 *
 * @param buf the buffer to put the results in (NULL == probe for length)
 * @param discard bytes to discard before starting to fill buf (normally 0)
 * @param len length of the buffer
 * @param unterm if non-zero do not include a trailing null
 * @return bytes returned (may be trunced and non-null terminated if == len)
 */
int mlog_getmasks(char *buf, int discard, int len, int unterm);

/**
 * mlog_ucon_add: add an endpoint as a ucon
 *
 * @param host hostname (or IP) of remote endpoint
 * @param port udp port (in host byte order)
 * @return 0 on success, -1 on error
 */
int mlog_ucon_add(char *host, int port);

/**
 * mlog_ucon_on: enable ucon (UDP console)
 *
 * @return 0 on success, -1 on error
 */
int mlog_ucon_on(void);

/**
 * mlog_ucon_off: disable ucon (UDP console) if enabled
 *
 * @return 0 on success, -1 on error
 */
int mlog_ucon_off(void);

/**
 * mlog_ucon_rm: remove an ucon endpoint
 *
 * @param host hostname (or IP) of remote endpoint
 * @param port udp port (in host byte order)
 * @return 0 on success, -1 on error
 */
int mlog_ucon_rm(char *host, int port);


#ifndef MLOG_NOMACRO_OPT
/*
 * here's some pre-processor based optimizations
 */

/*
 * use -DMLOG_NEVERLOG=1 to compile out all the mlog calls (e.g. for
 * performance testing, when you want to get rid of all extra overheads).
 */
#ifndef MLOG_NEVERLOG
#define MLOG_NEVERLOG 0      /* default value is to keep mlog */
#endif

/*
 * use -DMLOG_MINPRIORITY=level (e.g. level == MLOG_WARN) to compile
 * out any mlog() call lower than MLOG_MINPRIORITY.
 */
#ifndef MLOG_MINPRIORITY
#define MLOG_MINPRIORITY 0   /* default is to compile in all mlog calls */
#endif

/*
 * turn log into a macro so that we can check the log level before
 * evaluating all the mlog() args.   no point in computing the args
 * and building a call stack if we are not going to do anything.
 *
 * you can't do this with inline functions, because gcc will not
 * inline functions that use "..." so doing something like:
 *  void inline mlog(int f, char *c, ...) { return; }
 * and then
 *  mlog(MLOG_INFO, "fputs=%d", fputs("fputs was called", stdout));
 * will not inline out the function call setup, so fputs() will
 * get called even though mlog doesn't do anything.
 *
 * vmlog() will refilter, but it also has to handle stderr_mask, so
 * it isn't a big deal to have it recheck the level... could add
 * a flag to tell vmlog() to skip the filter if it was an issue.
 *
 * this assumes your cpp supports "..." and __VA_ARGS__ (gcc does).
 *
 * note that cpp does not expand the mlog() call inside the #define,
 * that goes to the real mlog function.
 */

#ifdef MLOG_NSPACE /* need to add namespace to calls in macro? */

#define mlog(LEVEL, ...) do {                                                 \
    if (MLOG_NEVERLOG == 0 && ((LEVEL) & MLOG_PRIMASK) >= MLOG_MINPRIORITY && \
        MLOG_NSPACE::mlog_filter(LEVEL))                                      \
        MLOG_NSPACE::mlog((LEVEL), __VA_ARGS__);                              \
    } while (0)

#else  /*MLOG_NSPACE */

#define mlog(LEVEL, ...) do {                                                 \
    if (MLOG_NEVERLOG == 0 && ((LEVEL) & MLOG_PRIMASK) >= MLOG_MINPRIORITY && \
        mlog_filter(LEVEL))                                                   \
        mlog((LEVEL), __VA_ARGS__);                                           \
    } while (0)

#endif /* MLOG_NSPACE */

#endif /* MLOG_NOMACRO_OPT */

MLOG_END_DECLS

#undef xnoreturn
#undef xprintfattr
#undef MLOG_BEGIN_DECLS
#undef MLOG_END_DECLS

#endif  /* _MLOG_H_ */
