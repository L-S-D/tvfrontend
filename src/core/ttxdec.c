/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ttxdec.h"
#include "strbuf.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SUBS 32       /* subpages per page */
#define MAX_SUBPAGES 8    /* subtitle pages */
#define MAX_EVENTS 256    /* events per subtitle page */
#define NPAGES 0x800      /* 0x100..0x8FF */

/* seq and epoch unique across all decoders (a new decoder for the same
 * channel must not mix anything up in the client) */
static uint64_t g_seq;
static unsigned g_epoch;

/* ---------------- Tables ---------------- */

static uint8_t unham[256], rev[256];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

/* Hamming 8/4 (ETS 300 706 8.2): data bits D1..D4 at bit 1, 3, 5, 7; single
 * bit errors corrected, 0xFF = not correctable */
static void make_tables(void) {
    uint8_t code[16];
    for (int d = 0; d < 16; d++) {
        int d1 = d & 1, d2 = (d >> 1) & 1, d3 = (d >> 2) & 1, d4 = (d >> 3) & 1;
        int p1 = 1 ^ d1 ^ d3 ^ d4, p2 = 1 ^ d1 ^ d2 ^ d4, p3 = 1 ^ d1 ^ d2 ^ d3;
        int p4 = 1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
        code[d] = (uint8_t) (p1 | d1 << 1 | p2 << 2 | d2 << 3 | p3 << 4 | d3 << 5 | p4 << 6 | d4 << 7);
    }
    for (int b = 0; b < 256; b++) {
        unham[b] = 0xFF;
        for (int d = 0; d < 16; d++)
            if (__builtin_popcount((unsigned) (b ^ code[d])) <= 1) {
                unham[b] = (uint8_t) d;
                break;
            }
        uint8_t r = 0;
        for (int i = 0; i < 8; i++)
            if (b & (1 << i)) r |= (uint8_t) (0x80 >> i);
        rev[b] = r;
    }
}

/* odd parity: 7 data bits, 0x80 = parity error */
static uint8_t parity7(uint8_t b) { return (__builtin_popcount(b) & 1) ? (uint8_t) (b & 0x7F) : 0x80; }

/* G0 Latin, national character sets (ETS 300 706 table 36), index C14 C13 C12 */
static const uint8_t NAT_POS[13] = {0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};
static const uint16_t NAT[8][13] = {
    {0xA3, 0x24, 0x40, 0x2190, 0xBD, 0x2192, 0x2191, 0x23, 0x2015, 0xBC, 0x2016, 0xBE, 0xF7}, /* English */
    {0xE9, 0xEF, 0xE0, 0xEB, 0xEA, 0xF9, 0xEE, 0x23, 0xE8, 0xE2, 0xF4, 0xFB, 0xE7},           /* French */
    {0x23, 0xA4, 0xC9, 0xC4, 0xD6, 0xC5, 0xDC, 0x5F, 0xE9, 0xE4, 0xF6, 0xE5, 0xFC},           /* Swedish/Finnish/Hungarian */
    {0x23, 0x16F, 0x10D, 0x165, 0x17E, 0xFD, 0xED, 0x159, 0xE9, 0xE1, 0x11B, 0xFA, 0x161},    /* Czech/Slovak */
    {0x23, 0x24, 0xA7, 0xC4, 0xD6, 0xDC, 0x5E, 0x5F, 0xB0, 0xE4, 0xF6, 0xFC, 0xDF},           /* German */
    {0xE7, 0x24, 0xA1, 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xBF, 0xFC, 0xF1, 0xE8, 0xE0},           /* Portuguese/Spanish */
    {0xA3, 0x24, 0xE9, 0xB0, 0xE7, 0x2192, 0x2191, 0x23, 0xF9, 0xE0, 0xF2, 0xE8, 0xEC},       /* Italian */
    {0xA3, 0x24, 0x40, 0x2190, 0xBD, 0x2192, 0x2191, 0x23, 0x2015, 0xBC, 0x2016, 0xBE, 0xF7}, /* (English) */
};

static uint16_t g0_char(uint8_t c, uint8_t national) {
    if (c == 0x7F) return 0x25A0; /* full block */
    for (int i = 0; i < 13; i++)
        if (NAT_POS[i] == c) return NAT[national & 7][i];
    return c;
}

static size_t put_utf8(char *s, uint16_t u) {
    if (u < 0x80) {
        s[0] = (char) u;
        return 1;
    }
    if (u < 0x800) {
        s[0] = (char) (0xC0 | (u >> 6));
        s[1] = (char) (0x80 | (u & 0x3F));
        return 2;
    }
    s[0] = (char) (0xE0 | (u >> 12));
    s[1] = (char) (0x80 | ((u >> 6) & 0x3F));
    s[2] = (char) (0x80 | (u & 0x3F));
    return 3;
}

static int decimal_page(unsigned page) { return (page & 0x0F) <= 9 && ((page >> 4) & 0x0F) <= 9; }

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---------------- Data ---------------- */

typedef struct {
    uint16_t subcode;
    uint8_t national;
    uint8_t flof, flof_row24;
    uint16_t links[6];
    int64_t updated_us;
    uint8_t rows[25][40]; /* 7-bit characters, 0x80 = parity error; row 0 = header */
} tsub;

typedef struct {
    tsub *subs; /* at most MAX_SUBS */
    int n, latest;
} tpage;

typedef struct {
    int page; /* page being received (0x100..0x8FF), -1 = none */
    int sub;  /* index into tpage.subs */
    int subtitle;
    int64_t pts; /* PTS of the PES with the page header */
} tmag;

typedef struct {
    char t[124]; /* UTF-8, at most 40 characters */
    uint8_t fg, row;
} tline;

typedef struct {
    uint64_t seq;
    int64_t pts;
    int n;
    tline lines[];
} tevent;

typedef struct {
    int page; /* 0 = free */
    tevent *ev[MAX_EVENTS];
    int head, n; /* ring: oldest at head */
} tsubpage;

struct ttxdec {
    pthread_mutex_t lock;
    tpage pages[NPAGES];
    int npages;
    tmag mags[8];
    int serial;
    int64_t pes_pts;
    uint8_t header[40]; /* last received header row (the running clock) */
    int header_valid;
    uint8_t header_national;
    tsubpage subp[MAX_SUBPAGES];
    unsigned epoch;
};

ttxdec *ttxdec_new(void) {
    pthread_once(&tables_once, make_tables);
    ttxdec *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    pthread_mutex_init(&t->lock, NULL);
    for (int i = 0; i < 8; i++) t->mags[i].page = -1;
    t->epoch = __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELAXED);
    return t;
}

static void free_events(tsubpage *sp) {
    for (int i = 0; i < sp->n; i++) free(sp->ev[(sp->head + i) % MAX_EVENTS]);
    sp->head = sp->n = 0;
}

void ttxdec_free(ttxdec *t) {
    if (!t) return;
    for (int i = 0; i < NPAGES; i++) free(t->pages[i].subs);
    for (int i = 0; i < MAX_SUBPAGES; i++) free_events(&t->subp[i]);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

/* ---------------- Subtitles ---------------- */

/* subtitle rows of a page: the text between Start Box (0x0B) and End
 * Box (0x0A) of each row, its colour the one set in the box (or before it) */
static int subtitle_lines(const uint8_t rows[25][40], uint8_t national, tline *out) {
    int n = 0;
    for (int r = 1; r < 25; r++) {
        int box = 0, colour_set = 0;
        uint8_t fg = 7;
        tline line;
        line.fg = 7;
        char text[200];
        size_t tl = 0;
        for (int c = 0; c < 40; c++) {
            uint8_t ch = rows[r][c];
            if (ch & 0x80) ch = 0x20;
            if (ch < 0x20) {
                if (ch <= 7) {
                    fg = ch;
                    if (box && !colour_set) {
                        line.fg = fg;
                        colour_set = 1;
                    }
                } else if (ch == 0x0B) {
                    if (!box && !colour_set) line.fg = fg;
                    box = 1;
                } else if (ch == 0x0A) {
                    box = 0;
                }
                if (box) text[tl++] = ' ';
                continue;
            }
            if (box) tl += put_utf8(text + tl, g0_char(ch, national));
        }
        size_t a = 0, b = tl;
        while (a < tl && text[a] == ' ') a++;
        while (b > a && text[b - 1] == ' ') b--;
        if (a == b) continue;
        size_t l = b - a < sizeof(line.t) - 1 ? b - a : sizeof(line.t) - 1;
        memcpy(line.t, text + a, l);
        line.t[l] = 0;
        line.row = (uint8_t) r;
        out[n++] = line;
    }
    return n;
}

static tsubpage *subpage(ttxdec *t, int page, int create) {
    for (int i = 0; i < MAX_SUBPAGES; i++)
        if (t->subp[i].page == page) return &t->subp[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_SUBPAGES; i++)
        if (!t->subp[i].page) {
            t->subp[i].page = page;
            return &t->subp[i];
        }
    return NULL;
}

/* page being received is complete: subtitle page -> event (lock held) */
static void end_page(ttxdec *t, tmag *m) {
    if (m->page >= 0 && m->subtitle) {
        tpage *pg = &t->pages[m->page - 0x100];
        if (m->sub >= 0 && m->sub < pg->n) {
            const tsub *s = &pg->subs[m->sub];
            tline lines[24];
            int n = subtitle_lines(s->rows, s->national, lines);
            tsubpage *sp = subpage(t, m->page, 1);
            if (sp) {
                const tevent *last = sp->n ? sp->ev[(sp->head + sp->n - 1) % MAX_EVENTS] : NULL;
                int same = last && last->n == n;
                for (int i = 0; same && i < n; i++)
                    same = !strcmp(last->lines[i].t, lines[i].t) && last->lines[i].fg == lines[i].fg;
                /* repetitions of the same subtitle are not a new event */
                if (!same) {
                    tevent *e = malloc(sizeof(*e) + (size_t) n * sizeof(tline));
                    if (e) {
                        e->seq = __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);
                        e->pts = m->pts;
                        e->n = n;
                        memcpy(e->lines, lines, (size_t) n * sizeof(tline));
                        if (sp->n == MAX_EVENTS) {
                            free(sp->ev[sp->head]);
                            sp->head = (sp->head + 1) % MAX_EVENTS;
                            sp->n--;
                        }
                        sp->ev[(sp->head + sp->n) % MAX_EVENTS] = e;
                        sp->n++;
                    }
                }
            }
        }
    }
    m->page = m->sub = -1;
    m->subtitle = 0;
    m->pts = -1;
}

/* ---------------- Input ---------------- */

/* pk: [0] field parity/line, [1] framing code, [2..3] MRAG, [4..43] data */
static void on_packet(ttxdec *t, const uint8_t *pk, int64_t now) {
    const uint8_t a0 = unham[pk[2]], a1 = unham[pk[3]];
    if (a0 == 0xFF || a1 == 0xFF) return;
    const uint8_t addr = (uint8_t) (a0 | (a1 << 4));
    const int mag = (addr & 7) ? (addr & 7) : 8;
    const int y = addr >> 3;
    const uint8_t *d = pk + 4;
    tmag *m = &t->mags[mag - 1];

    if (y == 0) {
        uint8_t h[8];
        for (int i = 0; i < 8; i++) h[i] = unham[d[i]];
        /* a header ends the page being received in its magazine (in serial
         * mode all of them) - even if it is not readable itself */
        if (h[7] != 0xFF) t->serial = h[7] & 1;
        if (t->serial)
            for (int i = 0; i < 8; i++) end_page(t, &t->mags[i]);
        else
            end_page(t, m);
        for (int i = 0; i < 8; i++)
            if (h[i] == 0xFF) return;
        const unsigned pp = (unsigned) ((h[1] << 4) | h[0]);
        if (pp == 0xFF || !decimal_page(pp)) return; /* filler header / page not for display */
        const int page = mag * 0x100 + (int) pp;
        const uint16_t subcode =
            (uint16_t) ((h[2] & 0xF) | ((h[3] & 7) << 4) | ((h[4] & 0xF) << 8) | ((h[5] & 3) << 12));
        const int erase = h[3] & 8;
        const uint8_t national = (uint8_t) ((h[7] >> 1) & 7);
        tpage *pg = &t->pages[page - 0x100];
        int idx = -1;
        for (int i = 0; i < pg->n; i++)
            if (pg->subs[i].subcode == subcode) idx = i;
        int fresh = 0;
        if (idx < 0) {
            fresh = 1;
            if (pg->n < MAX_SUBS) {
                tsub *ns = realloc(pg->subs, (size_t) (pg->n + 1) * sizeof(tsub));
                if (!ns) return;
                pg->subs = ns;
                if (pg->n == 0) {
                    pg->latest = -1;
                    t->npages++;
                }
                idx = pg->n++;
            } else {
                idx = 0;
                for (int i = 1; i < pg->n; i++)
                    if (pg->subs[i].updated_us < pg->subs[idx].updated_us) idx = i;
            }
        }
        tsub *s = &pg->subs[idx];
        if (fresh || erase) {
            memset(s->rows, 0x20, sizeof(s->rows));
            s->flof = s->flof_row24 = 0;
            memset(s->links, 0, sizeof(s->links));
        }
        s->subcode = subcode;
        s->national = national;
        memset(s->rows[0], 0x20, 8);
        for (int i = 8; i < 40; i++) s->rows[0][i] = parity7(d[i]);
        s->updated_us = now;
        pg->latest = idx;
        memcpy(t->header, s->rows[0], 40);
        t->header_valid = 1;
        t->header_national = national;
        m->page = page;
        m->sub = idx;
        m->subtitle = (h[5] & 8) != 0; /* C6 */
        m->pts = t->pes_pts;
        return;
    }
    if (m->page < 0) return;
    tpage *pg = &t->pages[m->page - 0x100];
    if (m->sub < 0 || m->sub >= pg->n) return;
    tsub *s = &pg->subs[m->sub];
    if (y >= 1 && y <= 24) {
        for (int i = 0; i < 40; i++) s->rows[y][i] = parity7(d[i]);
        s->updated_us = now;
    } else if (y == 27) {
        if (unham[d[0]] != 0) return; /* designation 0: editorial (FLOF) links */
        for (int i = 0; i < 6; i++) {
            const uint8_t *b = d + 1 + 6 * i;
            uint8_t u = unham[b[0]], tt = unham[b[1]], s2 = unham[b[3]], s4 = unham[b[5]];
            if (u == 0xFF || tt == 0xFF || s2 == 0xFF || s4 == 0xFF || u > 9 || tt > 9) {
                s->links[i] = 0;
                continue;
            }
            int rel = ((s2 >> 3) & 1) | (((s4 >> 2) & 1) << 1) | (((s4 >> 3) & 1) << 2);
            int lm = (mag & 7) ^ rel;
            s->links[i] = (uint16_t) ((lm ? lm : 8) * 0x100 + (tt << 4) + u);
        }
        uint8_t lc = unham[d[37]];
        /* ZDF, ORF, ARD send the packet with nothing but empty links (they use TOP) */
        s->flof = 0;
        for (int i = 0; i < 6; i++) s->flof = s->flof || s->links[i];
        s->flof_row24 = lc != 0xFF && (lc & 8);
    }
}

void ttxdec_feed(ttxdec *t, const uint8_t *d, size_t len, int64_t pts) {
    if (!t || len < 1 || d[0] < 0x10 || d[0] > 0x1F) return; /* data_identifier: EBU data */
    int64_t now = now_us();
    pthread_mutex_lock(&t->lock);
    t->pes_pts = pts;
    size_t pos = 1;
    while (pos + 2 <= len) {
        uint8_t id = d[pos], ul = d[pos + 1];
        if (pos + 2 + ul > len) break;
        if ((id == 0x02 || id == 0x03) && ul == 0x2C) {
            uint8_t pk[44];
            for (int i = 0; i < 44; i++) pk[i] = rev[d[pos + 2 + (size_t) i]];
            on_packet(t, pk, now);
        }
        pos += 2 + (size_t) ul;
    }
    pthread_mutex_unlock(&t->lock);
}

void ttxdec_reset_input(ttxdec *t) {
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    for (int i = 0; i < 8; i++) {
        t->mags[i].page = t->mags[i].sub = -1;
        t->mags[i].subtitle = 0;
        t->mags[i].pts = -1;
    }
    pthread_mutex_unlock(&t->lock);
}

void ttxdec_clear_events(ttxdec *t) {
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    for (int i = 0; i < MAX_SUBPAGES; i++) free_events(&t->subp[i]);
    t->epoch = __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&t->lock);
}

int ttxdec_page_count(ttxdec *t) {
    if (!t) return 0;
    pthread_mutex_lock(&t->lock);
    int n = t->npages;
    pthread_mutex_unlock(&t->lock);
    return n;
}

/* ---------------- Output ---------------- */

/* one cell: Unicode (mosaic U+E000 + sextant mask, +0x40 separated) */
typedef struct {
    uint16_t ch;
    uint8_t fg, bg;
    uint8_t attr; /* 1 flash, 2 conceal, 4 double height */
} cell;

/* Level 1 attributes (Set-At / Set-After, ETS 300 706 12.2) */
static void render_row(const uint8_t *row, uint8_t national, cell *out) {
    uint8_t fg = 7, bg = 0;
    int mosaic = 0, sep = 0, hold = 0, flash = 0, conceal = 0, dh = 0;
    uint16_t held = 0x20;
    for (int col = 0; col < 40; col++) {
        uint8_t c = row[col];
        if (c & 0x80) c = 0x20; /* parity error */
        cell *ce = &out[col];
        if (c < 0x20) {
            switch (c) { /* Set-At */
            case 0x09: flash = 0; break;
            case 0x0C: dh = 0; held = 0x20; break;
            case 0x18: conceal = 1; break;
            case 0x19: sep = 0; break;
            case 0x1A: sep = 1; break;
            case 0x1C: bg = 0; break;
            case 0x1D: bg = fg; break;
            case 0x1E: hold = 1; break;
            default: break;
            }
            ce->ch = (hold && mosaic) ? held : 0x20;
            ce->fg = fg;
            ce->bg = bg;
            ce->attr = (uint8_t) ((flash ? 1 : 0) | (conceal ? 2 : 0) | (dh ? 4 : 0));
            switch (c) { /* Set-After */
            case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
                fg = c;
                if (mosaic) held = 0x20;
                mosaic = 0;
                conceal = 0;
                break;
            case 0x08: flash = 1; break;
            case 0x0D:
            case 0x0F: dh = 1; held = 0x20; break;
            case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
                fg = (uint8_t) (c - 0x10);
                if (!mosaic) held = 0x20;
                mosaic = 1;
                conceal = 0;
                break;
            case 0x1F: hold = 0; break;
            default: break;
            }
            continue;
        }
        if (mosaic && (c & 0x20)) {
            uint16_t mask = (uint16_t) ((c & 0x1F) | ((c & 0x40) >> 1));
            ce->ch = (uint16_t) (0xE000 + mask + (sep ? 0x40 : 0));
            held = ce->ch;
        } else {
            ce->ch = g0_char(c, national);
        }
        ce->fg = fg;
        ce->bg = bg;
        ce->attr = (uint8_t) ((flash ? 1 : 0) | (conceal ? 2 : 0) | (dh ? 4 : 0));
    }
}

static void json_cells(strbuf *j, const cell *cells) {
    sb_addc(j, '"');
    for (int i = 0; i < 40; i++) {
        uint16_t u = cells[i].ch;
        if (u >= 0x20 && u < 0x7F && u != '"' && u != '\\') sb_addc(j, (char) u);
        else sb_addf(j, "\\u%04X", u);
    }
    sb_addc(j, '"');
}

static void json_digits(strbuf *j, const cell *cells, int what) {
    static const char hex[] = "0123456789abcdef";
    char b[42];
    b[0] = b[41] = '"';
    for (int i = 0; i < 40; i++)
        b[1 + i] = hex[(what == 0 ? cells[i].fg : what == 1 ? cells[i].bg : cells[i].attr) & 15];
    sb_addn(j, b, 42);
}

static const tsub *find_sub(const ttxdec *t, int page, int sub) {
    if (page < 0x100 || page > 0x8FF) return NULL;
    const tpage *pg = &t->pages[page - 0x100];
    if (!pg->n) return NULL;
    if (sub < 0) return pg->latest >= 0 ? &pg->subs[pg->latest] : &pg->subs[0];
    for (int i = 0; i < pg->n; i++)
        if (pg->subs[i].subcode == sub) return &pg->subs[i];
    return NULL;
}

char *ttxdec_pages_json(ttxdec *t) {
    strbuf j = {0};
    sb_add(&j, "{\"pages\":[");
    if (t) {
        pthread_mutex_lock(&t->lock);
        int first = 1;
        for (int i = 0; i < NPAGES; i++)
            if (t->pages[i].n) {
                sb_addf(&j, "%s\"%03X\"", first ? "" : ",", i + 0x100);
                first = 0;
            }
        pthread_mutex_unlock(&t->lock);
    }
    sb_add(&j, "]}");
    return sb_take(&j, NULL);
}

static int cmp_u16(const void *a, const void *b) {
    return (int) *(const uint16_t *) a - (int) *(const uint16_t *) b;
}

/* header row: "P100" in columns 0-3, columns 8-39 the last received header
 * (clock). Mosaic: U+E000 + mask (bit0 top left, bit1 top right, bit2
 * middle left, bit3 middle right, bit4 bottom left, bit5 bottom right),
 * +0x40 separated. a: 1 flash, 2 conceal, 4 double height (the row
 * below a double-height one is not shown: "hide") */
char *ttxdec_page_json(ttxdec *t, int page, int sub) {
    if (!t) return NULL;
    int64_t now = now_us();
    pthread_mutex_lock(&t->lock);
    const tsub *s = find_sub(t, page, sub);
    if (!s) {
        pthread_mutex_unlock(&t->lock);
        return NULL;
    }
    const tpage *pg = &t->pages[page - 0x100];
    strbuf j = {0};
    sb_addf(&j, "{\"page\":\"%03X\",\"sub\":\"%04X\",\"subs\":[", page, s->subcode);
    uint16_t subs[MAX_SUBS];
    for (int i = 0; i < pg->n; i++) subs[i] = pg->subs[i].subcode;
    qsort(subs, (size_t) pg->n, sizeof(subs[0]), cmp_u16);
    for (int i = 0; i < pg->n; i++) sb_addf(&j, "%s\"%04X\"", i ? "," : "", subs[i]);
    sb_addf(&j, "],\"national\":%u,\"age_ms\":%lld", s->national, (long long) ((now - s->updated_us) / 1000));
    sb_add(&j, ",\"links\":");
    if (s->flof) {
        sb_addc(&j, '[');
        for (int i = 0; i < 6; i++) {
            if (i) sb_addc(&j, ',');
            if (s->links[i]) sb_addf(&j, "\"%03X\"", s->links[i]);
            else sb_add(&j, "null");
        }
        sb_addc(&j, ']');
    } else {
        sb_add(&j, "null");
    }
    sb_add(&j, s->flof_row24 ? ",\"fastext\":true" : ",\"fastext\":false");
    sb_add(&j, ",\"rows\":[");
    uint8_t row[40];
    cell cells[40];
    int prev_dh = 0;
    for (int r = 0; r < 25; r++) {
        if (r == 0) {
            memcpy(row, t->header_valid ? t->header : s->rows[0], 40);
            char p[8];
            snprintf(p, sizeof(p), "P%03X", page);
            memset(row, 0x20, 8);
            memcpy(row, p, 4);
            render_row(row, t->header_valid ? t->header_national : s->national, cells);
        } else {
            render_row(s->rows[r], s->national, cells);
        }
        if (r) sb_addc(&j, ',');
        sb_add(&j, "{\"t\":");
        json_cells(&j, cells);
        sb_add(&j, ",\"fg\":");
        json_digits(&j, cells, 0);
        sb_add(&j, ",\"bg\":");
        json_digits(&j, cells, 1);
        sb_add(&j, ",\"a\":");
        json_digits(&j, cells, 2);
        if (prev_dh) sb_add(&j, ",\"hide\":true");
        sb_addc(&j, '}');
        int dh = 0;
        for (int i = 0; i < 40; i++) dh = dh || (cells[i].attr & 4);
        prev_dh = dh && !prev_dh && r > 0 && r < 24;
    }
    sb_add(&j, "]}");
    pthread_mutex_unlock(&t->lock);
    return sb_take(&j, NULL);
}

int ttxdec_page_text(ttxdec *t, int page, int sub, char rows[25][160]) {
    if (!t) return 0;
    pthread_mutex_lock(&t->lock);
    const tsub *s = find_sub(t, page, sub);
    if (s) {
        cell cells[40];
        for (int r = 0; r < 25; r++) {
            render_row(s->rows[r], s->national, cells);
            size_t o = 0;
            for (int i = 0; i < 40; i++) {
                uint16_t u = cells[i].ch >= 0xE000 && cells[i].ch < 0xE080 ? 0x20 : cells[i].ch;
                o += put_utf8(rows[r] + o, u);
            }
            rows[r][o] = 0;
        }
    }
    pthread_mutex_unlock(&t->lock);
    return s != NULL;
}

char *ttxdec_subtitle_pages_json(ttxdec *t) {
    strbuf j = {0};
    sb_add(&j, "{\"pages\":[");
    if (t) {
        pthread_mutex_lock(&t->lock);
        uint16_t p[MAX_SUBPAGES];
        int n = 0;
        for (int i = 0; i < MAX_SUBPAGES; i++)
            if (t->subp[i].page) p[n++] = (uint16_t) t->subp[i].page;
        pthread_mutex_unlock(&t->lock);
        qsort(p, (size_t) n, sizeof(p[0]), cmp_u16);
        for (int i = 0; i < n; i++) sb_addf(&j, "%s\"%03X\"", i ? "," : "", p[i]);
    }
    sb_add(&j, "]}");
    return sb_take(&j, NULL);
}

char *ttxdec_subtitles_json(ttxdec *t, int page, uint64_t since) {
    strbuf j = {0};
    uint64_t next = since;
    unsigned epoch = 0;
    sb_addf(&j, "{\"page\":\"%03X\",\"events\":[", page);
    if (t) {
        pthread_mutex_lock(&t->lock);
        epoch = t->epoch;
        const tsubpage *sp = subpage(t, page, 0);
        int k = 0;
        for (int i = 0; sp && i < sp->n; i++) {
            const tevent *e = sp->ev[(sp->head + i) % MAX_EVENTS];
            if (e->seq <= since) continue;
            if (e->seq > next) next = e->seq;
            sb_addf(&j, "%s{\"seq\":%llu,\"pts\":%lld,\"lines\":[", k++ ? "," : "", (unsigned long long) e->seq,
                    (long long) e->pts);
            for (int l = 0; l < e->n; l++) {
                sb_add(&j, l ? ",{\"t\":\"" : "{\"t\":\"");
                for (const unsigned char *c = (const unsigned char *) e->lines[l].t; *c; c++) {
                    if (*c == '"' || *c == '\\') {
                        sb_addc(&j, '\\');
                        sb_addc(&j, (char) *c);
                    } else if (*c < 0x20) {
                        sb_addc(&j, ' ');
                    } else {
                        sb_addc(&j, (char) *c); /* UTF-8 as it is */
                    }
                }
                sb_addf(&j, "\",\"c\":%u,\"r\":%u}", e->lines[l].fg, e->lines[l].row);
            }
            sb_add(&j, "]}");
        }
        pthread_mutex_unlock(&t->lock);
    }
    sb_addf(&j, "],\"epoch\":%u,\"next\":%llu}", epoch, (unsigned long long) next);
    return sb_take(&j, NULL);
}
