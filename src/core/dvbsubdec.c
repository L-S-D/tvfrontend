/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dvbsubdec.h"
#include "strbuf.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 32 /* bitmaps: about one minute of subtitles */
#define MAX_PAGE_REGIONS 64

static uint64_t g_seq;
static unsigned g_epoch;

static uint32_t rgba(int r, int g, int b, int a) {
    return ((uint32_t) r << 24) | ((uint32_t) g << 16) | ((uint32_t) b << 8) | (uint32_t) a;
}

typedef struct {
    uint32_t c2[4], c4[16], c8[256];
} clut;

/* default CLUTs (EN 300 743 10.x, as the usual decoders build them) */
static clut def_clut;
static pthread_once_t def_once = PTHREAD_ONCE_INIT;

static void make_default(void) {
    clut *c = &def_clut;
    c->c2[0] = rgba(0, 0, 0, 0);
    c->c2[1] = rgba(255, 255, 255, 255);
    c->c2[2] = rgba(0, 0, 0, 255);
    c->c2[3] = rgba(127, 127, 127, 255);
    for (int i = 0; i < 16; i++) {
        int v = i < 8 ? 255 : 127;
        c->c4[i] = rgba((i & 1) ? v : 0, (i & 2) ? v : 0, (i & 4) ? v : 0, 255);
    }
    c->c4[0] = rgba(0, 0, 0, 0);
    for (int i = 0; i < 256; i++) {
        int r, g, b, a;
        if (i < 8) {
            r = (i & 1) ? 255 : 0;
            g = (i & 2) ? 255 : 0;
            b = (i & 4) ? 255 : 0;
            a = 63;
        } else {
            switch (i & 0x88) {
            case 0x00:
            case 0x08:
                r = ((i & 1) ? 85 : 0) + ((i & 0x10) ? 170 : 0);
                g = ((i & 2) ? 85 : 0) + ((i & 0x20) ? 170 : 0);
                b = ((i & 4) ? 85 : 0) + ((i & 0x40) ? 170 : 0);
                a = (i & 0x88) == 0x08 ? 127 : 255;
                break;
            case 0x80:
                r = 127 + ((i & 1) ? 43 : 0) + ((i & 0x10) ? 85 : 0);
                g = 127 + ((i & 2) ? 43 : 0) + ((i & 0x20) ? 85 : 0);
                b = 127 + ((i & 4) ? 43 : 0) + ((i & 0x40) ? 85 : 0);
                a = 255;
                break;
            default: /* 0x88 */
                r = ((i & 1) ? 43 : 0) + ((i & 0x10) ? 85 : 0);
                g = ((i & 2) ? 43 : 0) + ((i & 0x20) ? 85 : 0);
                b = ((i & 4) ? 43 : 0) + ((i & 0x40) ? 85 : 0);
                a = 255;
                break;
            }
        }
        c->c8[i] = rgba(r, g, b, a);
    }
    c->c8[0] = rgba(0, 0, 0, 0);
}

static int clamp255(double v) {
    v += 0.5;
    return v < 0 ? 0 : v > 255 ? 255 : (int) v;
}

static uint32_t ycrcb_to_rgba(int y, int cr, int cb, int t) {
    if (y == 0) return 0; /* fully transparent */
    double yy = 1.164 * (y - 16);
    int r = clamp255(yy + 1.596 * (cr - 128));
    int g = clamp255(yy - 0.813 * (cr - 128) - 0.391 * (cb - 128));
    int b = clamp255(yy + 2.018 * (cb - 128));
    return rgba(r, g, b, 255 - t);
}

/* ---------------- Bits ---------------- */

typedef struct {
    const uint8_t *d;
    size_t len, bit;
} bits;

static uint32_t bget(bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t byte = b->bit >> 3;
        uint32_t x = byte < b->len ? (b->d[byte] >> (7 - (b->bit & 7))) & 1 : 0;
        v = (v << 1) | x;
        b->bit++;
    }
    return v;
}

static size_t bbyte_pos(const bits *b) { return (b->bit + 7) >> 3; } /* after byte alignment */
static int bend(const bits *b) { return (b->bit >> 3) >= b->len; }

/* ---------------- State ---------------- */

typedef struct {
    uint16_t id, x, y;
} robj;

typedef struct {
    int used;
    uint16_t w, h;
    uint8_t depth; /* bits per pixel: 2, 4, 8 */
    uint8_t clut;
    uint8_t bg[3]; /* background code for 2/4/8 bit */
    uint8_t *pixels;
    robj *objs;
    int nobj, capobj;
} region;

typedef struct {
    uint64_t seq;
    char *json;
} sevent;

struct dvbsubdec {
    pthread_mutex_t lock;
    uint16_t comp_page, anc_page;
    int64_t pts;
    /* epoch */
    uint8_t page_timeout;
    struct {
        uint8_t id;
        uint16_t x, y;
    } pr[MAX_PAGE_REGIONS];
    int npr;
    region regions[256];
    clut *cluts[256];
    uint16_t display_w, display_h;
    int page_seen;
    size_t display_sets;
    /* events */
    sevent ev[MAX_EVENTS];
    int head, n;
    int have_key;
    size_t last_nreg;
    uint64_t last_key; /* content of the last event (repetitions are not one) */
    unsigned epoch;
};

dvbsubdec *dvbsubdec_new(int composition_page, int ancillary_page) {
    pthread_once(&def_once, make_default);
    dvbsubdec *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    pthread_mutex_init(&d->lock, NULL);
    d->comp_page = (uint16_t) composition_page;
    d->anc_page = (uint16_t) ancillary_page;
    d->pts = -1;
    d->display_w = 720;
    d->display_h = 576;
    d->epoch = __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELAXED);
    return d;
}

static void clear_epoch(dvbsubdec *d) {
    for (int i = 0; i < 256; i++) {
        free(d->regions[i].pixels);
        free(d->regions[i].objs);
        memset(&d->regions[i], 0, sizeof(d->regions[i]));
        free(d->cluts[i]);
        d->cluts[i] = NULL;
    }
}

static void clear_events(dvbsubdec *d) {
    for (int i = 0; i < d->n; i++) free(d->ev[(d->head + i) % MAX_EVENTS].json);
    d->head = d->n = 0;
    d->have_key = 0;
}

void dvbsubdec_free(dvbsubdec *d) {
    if (!d) return;
    clear_epoch(d);
    clear_events(d);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

void dvbsubdec_reset(dvbsubdec *d) {
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    clear_epoch(d);
    clear_events(d);
    d->npr = 0;
    d->page_seen = 0;
    d->pts = -1;
    d->epoch = __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&d->lock);
}

/* ---------------- Segments ---------------- */

static void page_composition(dvbsubdec *d, const uint8_t *s, size_t len) {
    if (len < 2) return;
    d->page_timeout = s[0];
    const int state = (s[1] >> 2) & 3;
    if (state == 1 || state == 2) clear_epoch(d); /* Acquisition Point / Mode Change: new epoch */
    d->npr = 0;
    for (size_t i = 2; i + 6 <= len && d->npr < MAX_PAGE_REGIONS; i += 6) {
        d->pr[d->npr].id = s[i];
        d->pr[d->npr].x = (uint16_t) ((s[i + 2] << 8) | s[i + 3]);
        d->pr[d->npr].y = (uint16_t) ((s[i + 4] << 8) | s[i + 5]);
        d->npr++;
    }
    d->page_seen = 1;
}

static void region_composition(dvbsubdec *d, const uint8_t *s, size_t len) {
    if (len < 10) return;
    region *r = &d->regions[s[0]];
    const int fill = (s[1] >> 3) & 1;
    const uint16_t w = (uint16_t) ((s[2] << 8) | s[3]), h = (uint16_t) ((s[4] << 8) | s[5]);
    const int dc = (s[6] >> 2) & 7;
    const uint8_t depth = dc == 1 ? 2 : dc == 2 ? 4 : 8;
    if (w > 4096 || h > 4096) {
        free(r->pixels);
        r->pixels = NULL;
        r->w = r->h = 0;
        r->nobj = 0;
        return;
    }
    const int fresh = !r->pixels || r->w != w || r->h != h || r->depth != depth;
    r->used = 1;
    r->w = w;
    r->h = h;
    r->depth = depth;
    r->clut = s[7];
    r->bg[2] = s[8];
    r->bg[1] = (s[9] >> 4) & 0x0F;
    r->bg[0] = (s[9] >> 2) & 0x03;
    if (fresh) {
        free(r->pixels);
        r->pixels = w && h ? calloc((size_t) w * h, 1) : NULL;
    }
    if (r->pixels && (fresh || fill)) {
        uint8_t bg = depth == 2 ? r->bg[0] : depth == 4 ? r->bg[1] : r->bg[2];
        memset(r->pixels, bg, (size_t) w * h);
    }
    r->nobj = 0;
    for (size_t i = 10; i + 6 <= len;) {
        if (r->nobj == r->capobj) {
            int nc = r->capobj ? r->capobj * 2 : 8;
            robj *no = realloc(r->objs, (size_t) nc * sizeof(robj));
            if (!no) break;
            r->objs = no;
            r->capobj = nc;
        }
        robj *o = &r->objs[r->nobj++];
        o->id = (uint16_t) ((s[i] << 8) | s[i + 1]);
        const int otype = (s[i + 2] >> 6) & 3;
        o->x = (uint16_t) (((s[i + 2] & 0x0F) << 8) | s[i + 3]);
        o->y = (uint16_t) (((s[i + 4] & 0x0F) << 8) | s[i + 5]);
        i += (otype == 1 || otype == 2) ? 8 : 6;
    }
}

static void clut_definition(dvbsubdec *d, const uint8_t *s, size_t len) {
    if (len < 2) return;
    clut *c = d->cluts[s[0]];
    if (!c) {
        c = malloc(sizeof(*c));
        if (!c) return;
        *c = def_clut;
        d->cluts[s[0]] = c;
    }
    for (size_t i = 2; i + 2 <= len;) {
        const uint8_t id = s[i], flags = s[i + 1];
        int y, cr, cb, t;
        if (flags & 1) {
            if (i + 6 > len) break;
            y = s[i + 2];
            cr = s[i + 3];
            cb = s[i + 4];
            t = s[i + 5];
            i += 6;
        } else {
            if (i + 4 > len) break;
            const uint16_t v = (uint16_t) ((s[i + 2] << 8) | s[i + 3]);
            y = ((v >> 10) & 0x3F) << 2;
            cr = ((v >> 6) & 0x0F) << 4;
            cb = ((v >> 2) & 0x0F) << 4;
            t = (v & 3) << 6;
            i += 4;
        }
        const uint32_t col = ycrcb_to_rgba(y, cr, cb, t);
        if ((flags & 0x80) && id < 4) c->c2[id] = col;
        if ((flags & 0x40) && id < 16) c->c4[id] = col;
        if (flags & 0x20) c->c8[id] = col;
    }
}

typedef struct {
    region *r;
    int x, y;
    int non_mod;
    uint8_t m24[4], m28[4], m48[16];
} drawctx;

static void put(drawctx *c, uint32_t code, int nbits, uint32_t run) {
    region *r = c->r;
    uint8_t v;
    if (nbits == r->depth) v = (uint8_t) code;
    else if (nbits == 2) v = r->depth == 4 ? c->m24[code & 3] : c->m28[code & 3];
    else if (nbits == 4) v = r->depth == 8 ? c->m48[code & 15] : (uint8_t) (code >> 2);
    else v = r->depth == 4 ? (uint8_t) (code >> 4) : (uint8_t) (code >> 6);
    for (uint32_t i = 0; i < run; i++, c->x++)
        if (!(c->non_mod && code == 1) && c->x >= 0 && c->y >= 0 && c->x < r->w && c->y < r->h)
            r->pixels[(size_t) c->y * r->w + (size_t) c->x] = v;
}

static void draw_field(region *r, const robj *o, const uint8_t *s, size_t len, int first_line, int non_mod) {
    drawctx c = {.r = r, .x = o->x, .y = o->y + first_line, .non_mod = non_mod,
                 .m24 = {0x0, 0x7, 0x8, 0xF}, .m28 = {0x00, 0x77, 0x88, 0xFF}};
    for (int i = 0; i < 16; i++) c.m48[i] = (uint8_t) (i * 0x11);
    size_t p = 0;
    while (p < len) {
        const uint8_t type = s[p++];
        if (type == 0x10 || type == 0x11 || type == 0x12) {
            bits b = {s + p, len - p, 0};
            int end = 0;
            while (!end && !bend(&b)) {
                if (type == 0x10) {
                    uint32_t v = bget(&b, 2);
                    if (v) {
                        put(&c, v, 2, 1);
                    } else if (bget(&b, 1)) {
                        uint32_t run = bget(&b, 3) + 3;
                        put(&c, bget(&b, 2), 2, run);
                    } else if (bget(&b, 1)) {
                        put(&c, 0, 2, 1);
                    } else {
                        switch (bget(&b, 2)) {
                        case 0: end = 1; break;
                        case 1: put(&c, 0, 2, 2); break;
                        case 2: {
                            uint32_t run = bget(&b, 4) + 12;
                            put(&c, bget(&b, 2), 2, run);
                            break;
                        }
                        default: {
                            uint32_t run = bget(&b, 8) + 29;
                            put(&c, bget(&b, 2), 2, run);
                            break;
                        }
                        }
                    }
                } else if (type == 0x11) {
                    uint32_t v = bget(&b, 4);
                    if (v) {
                        put(&c, v, 4, 1);
                    } else if (!bget(&b, 1)) {
                        uint32_t run = bget(&b, 3);
                        if (run) put(&c, 0, 4, run + 2);
                        else end = 1;
                    } else if (!bget(&b, 1)) {
                        uint32_t run = bget(&b, 2) + 4;
                        put(&c, bget(&b, 4), 4, run);
                    } else {
                        switch (bget(&b, 2)) {
                        case 0: put(&c, 0, 4, 1); break;
                        case 1: put(&c, 0, 4, 2); break;
                        case 2: {
                            uint32_t run = bget(&b, 4) + 9;
                            put(&c, bget(&b, 4), 4, run);
                            break;
                        }
                        default: {
                            uint32_t run = bget(&b, 8) + 25;
                            put(&c, bget(&b, 4), 4, run);
                            break;
                        }
                        }
                    }
                } else {
                    uint32_t v = bget(&b, 8);
                    if (v) {
                        put(&c, v, 8, 1);
                    } else if (!bget(&b, 1)) {
                        uint32_t run = bget(&b, 7);
                        if (run) put(&c, 0, 8, run);
                        else end = 1;
                    } else {
                        uint32_t run = bget(&b, 7);
                        put(&c, bget(&b, 8), 8, run);
                    }
                }
            }
            p += bbyte_pos(&b);
        } else if (type == 0x20) {
            if (p + 2 > len) break;
            c.m24[0] = s[p] >> 4;
            c.m24[1] = s[p] & 15;
            c.m24[2] = s[p + 1] >> 4;
            c.m24[3] = s[p + 1] & 15;
            p += 2;
        } else if (type == 0x21) {
            if (p + 4 > len) break;
            memcpy(c.m28, s + p, 4);
            p += 4;
        } else if (type == 0x22) {
            if (p + 16 > len) break;
            memcpy(c.m48, s + p, 16);
            p += 16;
        } else if (type == 0xF0) { /* end of object line */
            c.x = o->x;
            c.y += 2;
        } else {
            break;
        }
    }
}

static void object_data(dvbsubdec *d, const uint8_t *s, size_t len) {
    if (len < 7) return;
    const uint16_t id = (uint16_t) ((s[0] << 8) | s[1]);
    const int coding = (s[2] >> 2) & 3;
    const int non_mod = (s[2] >> 1) & 1;
    if (coding != 0) return; /* character objects: the broadcasters here do not use them */
    const size_t top = ((size_t) s[3] << 8) | s[4], bottom = ((size_t) s[5] << 8) | s[6];
    if (7 + top + bottom > len) return;
    for (int k = 0; k < 256; k++) {
        region *r = &d->regions[k];
        if (!r->used || !r->pixels) continue;
        for (int i = 0; i < r->nobj; i++)
            if (r->objs[i].id == id) {
                draw_field(r, &r->objs[i], s + 7, top, 0, non_mod);
                if (bottom) draw_field(r, &r->objs[i], s + 7 + top, bottom, 1, non_mod);
                else draw_field(r, &r->objs[i], s + 7, top, 1, non_mod); /* bottom = top */
            }
    }
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(strbuf *out, const uint8_t *in, size_t n) {
    size_t i = 0;
    char q[4];
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8) | in[i + 2];
        q[0] = B64[v >> 18];
        q[1] = B64[(v >> 12) & 63];
        q[2] = B64[(v >> 6) & 63];
        q[3] = B64[v & 63];
        sb_addn(out, q, 4);
    }
    if (i + 1 == n) {
        uint32_t v = (uint32_t) in[i] << 16;
        q[0] = B64[v >> 18];
        q[1] = B64[(v >> 12) & 63];
        q[2] = q[3] = '=';
        sb_addn(out, q, 4);
    } else if (i + 2 == n) {
        uint32_t v = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8);
        q[0] = B64[v >> 18];
        q[1] = B64[(v >> 12) & 63];
        q[2] = B64[(v >> 6) & 63];
        q[3] = '=';
        sb_addn(out, q, 4);
    }
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static const uint32_t *palette_of(const dvbsubdec *d, const region *r, int *n) {
    const clut *c = d->cluts[r->clut] ? d->cluts[r->clut] : &def_clut;
    if (r->depth == 2) {
        *n = 4;
        return c->c2;
    }
    if (r->depth == 4) {
        *n = 16;
        return c->c4;
    }
    *n = 256;
    return c->c8;
}

static void end_of_display_set(dvbsubdec *d) {
    if (!d->page_seen) return;
    d->page_seen = 0;
    d->display_sets++;
    /* visible regions */
    int vis[MAX_PAGE_REGIONS], nvis = 0;
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < d->npr; i++) {
        const region *r = &d->regions[d->pr[i].id];
        if (!r->used || !r->pixels) continue;
        int np;
        const uint32_t *pal = palette_of(d, r, &np);
        size_t total = (size_t) r->w * r->h;
        int visible = 0;
        for (size_t k = 0; k < total && !visible; k++)
            if (r->pixels[k] < np && (pal[r->pixels[k]] & 0xFF)) visible = 1;
        if (!visible) continue; /* fully transparent: nothing to draw */
        vis[nvis++] = i;
        key = fnv(key, &d->pr[i].x, sizeof(d->pr[i].x));
        key = fnv(key, &d->pr[i].y, sizeof(d->pr[i].y));
        key = fnv(key, r->pixels, total);
        key = fnv(key, pal, (size_t) np * 4);
    }
    if (d->have_key && d->last_nreg == (size_t) nvis && d->last_key == key) return; /* repeated display set */
    d->have_key = 1;
    d->last_nreg = (size_t) nvis;
    d->last_key = key;

    uint64_t seq = __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);
    strbuf j = {0};
    sb_addf(&j, "{\"seq\":%llu,\"pts\":%lld,\"timeout\":%u,\"w\":%u,\"h\":%u,\"regions\":[", (unsigned long long) seq,
            (long long) d->pts, d->page_timeout, d->display_w, d->display_h);
    for (int v = 0; v < nvis; v++) {
        int i = vis[v];
        const region *r = &d->regions[d->pr[i].id];
        int np;
        const uint32_t *pal = palette_of(d, r, &np);
        sb_addf(&j, "%s{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"pal\":\"", v ? "," : "", d->pr[i].x, d->pr[i].y, r->w,
                r->h);
        for (int k = 0; k < np; k++) sb_addf(&j, "%08x", pal[k]);
        sb_add(&j, "\",\"rle\":\"");
        size_t total = (size_t) r->w * r->h;
        uint8_t *rle = malloc(total * 2 + 2);
        if (rle) {
            size_t o = 0;
            for (size_t p = 0; p < total;) {
                size_t n = 1;
                while (p + n < total && r->pixels[p + n] == r->pixels[p] && n < 256) n++;
                rle[o++] = (uint8_t) (n - 1);
                rle[o++] = r->pixels[p];
                p += n;
            }
            base64(&j, rle, o);
            free(rle);
        }
        sb_add(&j, "\"}");
    }
    sb_add(&j, "]}");
    char *json = sb_take(&j, NULL);
    if (!json) return;
    if (d->n == MAX_EVENTS) {
        free(d->ev[d->head].json);
        d->head = (d->head + 1) % MAX_EVENTS;
        d->n--;
    }
    sevent *e = &d->ev[(d->head + d->n) % MAX_EVENTS];
    e->seq = seq;
    e->json = json;
    d->n++;
}

static void on_segment(dvbsubdec *d, uint8_t type, const uint8_t *s, size_t len) {
    switch (type) {
    case 0x10: page_composition(d, s, len); break;
    case 0x11: region_composition(d, s, len); break;
    case 0x12: clut_definition(d, s, len); break;
    case 0x13: object_data(d, s, len); break;
    case 0x14: /* Display Definition */
        if (len >= 5) {
            d->display_w = (uint16_t) (((s[1] << 8) | s[2]) + 1);
            d->display_h = (uint16_t) (((s[3] << 8) | s[4]) + 1);
        }
        break;
    case 0x80: end_of_display_set(d); break;
    default: break;
    }
}

void dvbsubdec_feed(dvbsubdec *d, const uint8_t *seg, size_t len, int64_t pts) {
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    if (pts >= 0) d->pts = pts;
    size_t pos = 0;
    while (pos + 6 <= len && seg[pos] == 0x0F) {
        const uint8_t type = seg[pos + 1];
        const uint16_t page = (uint16_t) ((seg[pos + 2] << 8) | seg[pos + 3]);
        const size_t slen = ((size_t) seg[pos + 4] << 8) | seg[pos + 5];
        if (pos + 6 + slen > len) break;
        if (page == d->comp_page || page == d->anc_page) on_segment(d, type, seg + pos + 6, slen);
        pos += 6 + slen;
    }
    pthread_mutex_unlock(&d->lock);
}

char *dvbsubdec_events_json(dvbsubdec *d, uint64_t since) {
    strbuf j = {0};
    uint64_t next = since;
    unsigned epoch = 0;
    sb_addf(&j, "{\"page\":%u,\"events\":[", d ? d->comp_page : 0);
    if (d) {
        pthread_mutex_lock(&d->lock);
        epoch = d->epoch;
        int k = 0;
        for (int i = 0; i < d->n; i++) {
            const sevent *e = &d->ev[(d->head + i) % MAX_EVENTS];
            if (e->seq <= since) continue;
            if (e->seq > next) next = e->seq;
            if (k++) sb_addc(&j, ',');
            sb_add(&j, e->json);
        }
        pthread_mutex_unlock(&d->lock);
    }
    sb_addf(&j, "],\"epoch\":%u,\"next\":%llu}", epoch, (unsigned long long) next);
    return sb_take(&j, NULL);
}

size_t dvbsubdec_display_sets(dvbsubdec *d) {
    if (!d) return 0;
    pthread_mutex_lock(&d->lock);
    size_t n = d->display_sets;
    pthread_mutex_unlock(&d->lock);
    return n;
}
