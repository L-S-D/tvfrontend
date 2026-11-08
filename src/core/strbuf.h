/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Growing text buffer for JSON responses (malloc, always 0-terminated).
 * When out of memory it stays marked empty (err) - sb_take then returns NULL.
 */
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *p;
    size_t n, cap;
    int err;
} strbuf;

static inline int sb_grow(strbuf *s, size_t add) {
    if (s->err) return 0;
    if (s->n + add + 1 <= s->cap) return 1;
    size_t nc = s->cap ? s->cap : 256;
    while (nc < s->n + add + 1) nc *= 2;
    char *q = realloc(s->p, nc);
    if (!q) {
        s->err = 1;
        return 0;
    }
    s->p = q;
    s->cap = nc;
    return 1;
}

static inline void sb_addn(strbuf *s, const char *t, size_t n) {
    if (!sb_grow(s, n)) return;
    memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = 0;
}

static inline void sb_add(strbuf *s, const char *t) { sb_addn(s, t, strlen(t)); }

static inline void sb_addc(strbuf *s, char c) { sb_addn(s, &c, 1); }

__attribute__((format(printf, 2, 3))) static inline void sb_addf(strbuf *s, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (r < 0) return;
    if ((size_t) r < sizeof(tmp)) {
        sb_addn(s, tmp, (size_t) r);
        return;
    }
    if (!sb_grow(s, (size_t) r)) return;
    va_start(ap, fmt);
    vsnprintf(s->p + s->n, (size_t) r + 1, fmt, ap);
    va_end(ap);
    s->n += (size_t) r;
}

/* take the contents (NULL on error); the buffer is empty afterwards */
static inline char *sb_take(strbuf *s, size_t *len) {
    char *p = s->err ? NULL : s->p;
    if (s->err) free(s->p);
    if (p && len) *len = s->n;
    if (!p && !s->err) {
        p = calloc(1, 1);
        if (len) *len = 0;
    }
    s->p = NULL;
    s->n = s->cap = 0;
    s->err = 0;
    return p;
}

static inline void sb_free(strbuf *s) {
    free(s->p);
    s->p = NULL;
    s->n = s->cap = 0;
    s->err = 0;
}
