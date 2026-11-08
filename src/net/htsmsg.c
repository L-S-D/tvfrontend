/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "htsmsg.h"

#include <stdlib.h>
#include <string.h>

enum { HMF_MAP = 1, HMF_S64 = 2, HMF_STR = 3, HMF_BIN = 4, HMF_LIST = 5, HMF_DBL = 6, HMF_BOOL = 7, HMF_UUID = 8 };

void hmsg_init(hmsg *m) {
    memset(m, 0, sizeof(*m));
    m->cap = 256;
    m->p = malloc(m->cap);
    if (!m->p) m->err = 1;
    m->len = 4; /* room for the length */
}

void hmsg_free(hmsg *m) {
    free(m->p);
    memset(m, 0, sizeof(*m));
}

static void put(hmsg *m, const void *d, size_t n) {
    if (m->err) return;
    if (m->len + n > m->cap) {
        size_t c = m->cap * 2;
        while (c < m->len + n) c *= 2;
        unsigned char *p = realloc(m->p, c);
        if (!p) {
            m->err = 1;
            return;
        }
        m->p = p;
        m->cap = c;
    }
    memcpy(m->p + m->len, d, n);
    m->len += n;
}

static void field(hmsg *m, int type, const char *name, const void *data, size_t n) {
    size_t nl = strlen(name);
    if (nl > 255) {
        m->err = 1;
        return;
    }
    unsigned char h[6] = {(unsigned char) type, (unsigned char) nl, (unsigned char) (n >> 24), (unsigned char) (n >> 16),
                          (unsigned char) (n >> 8), (unsigned char) n};
    put(m, h, 6);
    put(m, name, nl);
    put(m, data, n);
}

void hmsg_s64(hmsg *m, const char *name, int64_t v) {
    unsigned char b[8];
    uint64_t u = (uint64_t) v;
    size_t n = 0;
    while (u && n < 8) {
        b[n++] = (unsigned char) (u & 0xFF);
        u >>= 8;
    }
    /* negative numbers: all 8 bytes (sign) */
    if (v < 0) {
        u = (uint64_t) v;
        for (n = 0; n < 8; n++) b[n] = (unsigned char) (u >> (8 * n));
    }
    field(m, HMF_S64, name, b, n);
}

void hmsg_str(hmsg *m, const char *name, const char *s) { field(m, HMF_STR, name, s, strlen(s)); }

void hmsg_list_s64(hmsg *m, const char *name, const int64_t *v, size_t n) {
    /* elements are fields without names: build them in a helper message, copy it without its length field */
    hmsg l;
    hmsg_init(&l);
    for (size_t i = 0; i < n; i++) hmsg_s64(&l, "", v[i]);
    if (l.err) m->err = 1;
    else field(m, HMF_LIST, name, l.p + 4, l.len - 4);
    hmsg_free(&l);
}

void hmsg_bin(hmsg *m, const char *name, const void *data, size_t n) { field(m, HMF_BIN, name, data, n); }

const unsigned char *hmsg_wire(hmsg *m, size_t *len) {
    if (m->err) return NULL;
    size_t body = m->len - 4;
    m->p[0] = (unsigned char) (body >> 24);
    m->p[1] = (unsigned char) (body >> 16);
    m->p[2] = (unsigned char) (body >> 8);
    m->p[3] = (unsigned char) body;
    *len = m->len;
    return m->p;
}

static cJSON *hex(const unsigned char *d, size_t n) {
    static const char x[] = "0123456789abcdef";
    char *s = malloc(n * 2 + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) {
        s[2 * i] = x[d[i] >> 4];
        s[2 * i + 1] = x[d[i] & 15];
    }
    s[n * 2] = 0;
    cJSON *j = cJSON_CreateString(s);
    free(s);
    return j;
}

int hmsg_unhex(const char *h, unsigned char *out, size_t max) {
    size_t n = h ? strlen(h) : 0;
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = h[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        out[i] = (unsigned char) v;
    }
    return (int) (n / 2);
}

/* fields of buf into parent (object or list); 0 = ok */
static int parse_into(cJSON *parent, int list, const unsigned char *buf, size_t len, int depth) {
    if (depth > 16) return -1;
    while (len > 0) {
        if (len < 6) return -1;
        int type = buf[0];
        size_t nl = buf[1];
        size_t dl = (size_t) buf[2] << 24 | (size_t) buf[3] << 16 | (size_t) buf[4] << 8 | buf[5];
        buf += 6;
        len -= 6;
        /* not nl + dl: dl up to 2^32-1 overflows with size_t (32 bits) */
        if (dl > len || nl > len - dl) return -1;
        char name[256];
        memcpy(name, buf, nl);
        name[nl] = 0;
        const unsigned char *d = buf + nl;
        cJSON *v = NULL;
        switch (type) {
        case HMF_S64: {
            uint64_t u = 0;
            for (size_t i = dl; i > 0; i--) u = (u << 8) | d[i - 1];
            /* fewer than 8 bytes are never negative; 8 bytes are signed */
            v = cJSON_CreateNumber(dl == 8 ? (double) (int64_t) u : (double) u);
            break;
        }
        case HMF_STR: {
            char *s = malloc(dl + 1);
            if (!s) return -1;
            memcpy(s, d, dl);
            s[dl] = 0;
            v = cJSON_CreateString(s);
            free(s);
            break;
        }
        case HMF_BIN:
        case HMF_UUID: v = hex(d, dl); break;
        case HMF_BOOL: v = cJSON_CreateBool(dl == 1 && d[0]); break;
        case HMF_MAP:
        case HMF_LIST:
            v = type == HMF_MAP ? cJSON_CreateObject() : cJSON_CreateArray();
            if (!v || parse_into(v, type == HMF_LIST, d, dl, depth + 1) != 0) {
                cJSON_Delete(v);
                return -1;
            }
            break;
        default: break; /* unknown (DBL does not occur in binary): skip */
        }
        if (v) {
            if (list) cJSON_AddItemToArray(parent, v);
            else cJSON_AddItemToObject(parent, name, v);
        }
        buf += nl + dl;
        len -= nl + dl;
    }
    return 0;
}

cJSON *hmsg_parse(const unsigned char *buf, size_t len) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    if (parse_into(j, 0, buf, len, 0) != 0) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}

int hmsg_next(const unsigned char *buf, size_t len, size_t *pos, hmsg_field *f) {
    if (*pos >= len) return 0;
    if (len - *pos < 6) return -1;
    const unsigned char *p = buf + *pos;
    size_t nl = p[1];
    size_t dl = (size_t) p[2] << 24 | (size_t) p[3] << 16 | (size_t) p[4] << 8 | p[5];
    size_t rem = len - *pos - 6;
    if (dl > rem || nl > rem - dl) return -1; /* without overflow of nl + dl */
    f->type = p[0];
    f->name = (const char *) p + 6;
    f->nlen = nl;
    f->data = p + 6 + nl;
    f->dlen = dl;
    *pos += 6 + nl + dl;
    return 1;
}

int hmsg_is(const hmsg_field *f, const char *name) {
    size_t n = strlen(name);
    return f->nlen == n && !memcmp(f->name, name, n);
}

int64_t hmsg_s64_of(const hmsg_field *f) {
    if (f->type != HMF_S64 || f->dlen > 8) return 0;
    uint64_t u = 0;
    for (size_t i = f->dlen; i > 0; i--) u = (u << 8) | f->data[i - 1];
    return (int64_t) u;
}
