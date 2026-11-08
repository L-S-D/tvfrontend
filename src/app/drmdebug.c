/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Diagnostics only (CMake -DTVF_DRM_DEBUG=ON): messages of the DRM modules into
 * the TVF log. TVF exports PmLogGetContext/_PmLogMsgKV and open/open64 - the
 * modules (and all other libraries in the process) bind to these instead of
 * libPmLogLib or libc:
 *   - contexts whose name looks like DRM get all levels and go
 *     into the log ([pmlog <context>]); others go formatted to PmLogString_
 *   - failed open() calls on /dev/... are logged
 */
#define _GNU_SOURCE
#undef _FILE_OFFSET_BITS /* open and open64 are two separate functions here */
#include "log.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    int enabledLevel;
    int flags;
} ctxinfo;

#define NTAP 32
static ctxinfo taps[NTAP];
static char names[NTAP][48];
static int ntaps;

static int wanted(const char *n) {
    static const char *k[] = {"drm", "cdm", "playready", "widevine", "wv", "tee", "secure", "sestore", "sstr", "kadaptor", "pr"};
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (strcasestr(n, k[i])) return 1;
    return 0;
}

__attribute__((visibility("default"))) int PmLogGetContext(const char *name, const ctxinfo **ctx) {
    static int (*real)(const char *, const ctxinfo **);
    if (!real) real = (int (*)(const char *, const ctxinfo **)) dlsym(RTLD_NEXT, "PmLogGetContext");
    int r = real ? real(name, ctx) : 0;
    if (name && wanted(name)) {
        int i;
        for (i = 0; i < ntaps; i++)
            if (!strcmp(names[i], name)) break;
        if (i == ntaps && ntaps < NTAP) {
            snprintf(names[ntaps], sizeof(names[ntaps]), "%s", name);
            taps[ntaps].enabledLevel = 7;
            ntaps++;
            mt_log("[pmlog] tapping context %s", name);
        }
        if (i < NTAP) {
            *ctx = &taps[i];
            return 0;
        }
    }
    return r;
}

__attribute__((visibility("default"))) int _PmLogMsgKV(const ctxinfo *ctx, int level, unsigned flags, const char *msgid,
                                                       size_t kv_count, const char *kw, const char *kf, const char *fmt,
                                                       ...) {
    (void) flags, (void) kv_count, (void) kw, (void) kf;
    char b[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt ? fmt : "", ap);
    va_end(ap);
    if (ctx >= taps && ctx < taps + NTAP) {
        mt_log("[pmlog %s] %d %s %s", names[ctx - taps], level, msgid ? msgid : "", b);
        return 0;
    }
    static int (*real)(const ctxinfo *, int, const char *, const char *, const char *);
    if (!real) real = (int (*)(const ctxinfo *, int, const char *, const char *, const char *)) dlsym(RTLD_NEXT, "PmLogString_");
    return real ? real(ctx, level, msgid, NULL, b) : 0;
}

static int do_open(const char *sym, const char *path, int flags, mode_t mode) {
    static int (*real_open)(const char *, int, ...), (*real_open64)(const char *, int, ...);
    int (**r)(const char *, int, ...) = sym[4] ? &real_open64 : &real_open;
    if (!*r) *r = (int (*)(const char *, int, ...)) dlsym(RTLD_NEXT, sym);
    int fd = *r ? (*r)(path, flags, mode) : -1;
    if (fd < 0 && path && !strncmp(path, "/dev/", 5)) {
        int e = errno;
        mt_log("[drmdebug] %s(%s) -> %s", sym, path, strerror(e));
        errno = e;
    }
    return fd;
}

__attribute__((visibility("default"))) int open(const char *path, int flags, ...) {
    mode_t m = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        m = (mode_t) va_arg(ap, int);
        va_end(ap);
    }
    return do_open("open", path, flags, m);
}

__attribute__((visibility("default"))) int open64(const char *path, int flags, ...) {
    mode_t m = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        m = (mode_t) va_arg(ap, int);
        va_end(ap);
    }
    return do_open("open64", path, flags, m);
}
