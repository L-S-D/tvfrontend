/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ttx.h"
#include "i18n.h"

#include "cJSON.h"
#include "fetch.h"
#include "keys.h"
#include "log.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_truetype from LVGL, here as our own static copy: Bedstead stretched to the
 * cell with separate x/y scaling (like the HTML5 app) */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "src/libs/tiny_ttf/stb_truetype_htcw.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define CW 36
#define RH 42
#define COLS 40
#define ROWS 25
#define TW (COLS * CW)
#define TH (ROWS * RH)

static const uint32_t COLORS[8] = {0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00,
                                   0xFF0000FF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF};

static ttx_actions act;
static lv_obj_t *backdrop, *canvas;
static uint32_t *buf;
static int vis;
static char pre[16]; /* "local:ttx/" (page store of the HTSP subscription), "" = no teletext */
static char page[4] = "100", input[4];
static int sub_sel; /* 0 = last received subpage */
static cJSON *data;
static int missing, mix, flash_on = 1, gen, has_flash;
static char *last_body; /* do not redraw an unchanged page */
static cJSON *pages;
static uint32_t t_load, t_flash;
static int pages_busy, step_pending; /* page list requested; steps waiting for it */

/* ---------------- Font ---------------- */

static stbtt_fontinfo font;
static unsigned char *font_data;
static int font_ok;

typedef struct {
    uint32_t cp;
    int dh;
    uint8_t *mask; /* CW x (dh ? 2RH : RH) */
} glyph;

static glyph cache[512];
static int ncache;

static void font_load(const char *res_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/fonts/bedstead.otf", res_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        mt_log("[ttx] %s missing", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    font_data = n > 0 ? malloc((size_t) n) : NULL;
    if (font_data && fread(font_data, 1, (size_t) n, f) == (size_t) n)
        font_ok = stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0));
    fclose(f);
    mt_log("[ttx] Bedstead %s", font_ok ? "loaded" : "NOT readable");
}

static const uint8_t *glyph_mask(uint32_t cp, int dh) {
    for (int i = 0; i < ncache; i++)
        if (cache[i].cp == cp && cache[i].dh == dh) return cache[i].mask;
    if (!font_ok) return NULL;
    int h = dh ? 2 * RH : RH;
    uint8_t *m = calloc((size_t) CW * h, 1);
    if (!m) return NULL;
    /* Bedstead: 1000 units/em, advance 600, ascender 800 */
    float sx = (float) CW / 600.0f, sy = (float) h / 1000.0f;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font, (int) cp, sx, sy, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0, base = (int) (800 * sy + 0.5f);
    if (gw > 0 && gh > 0) {
        uint8_t *tmp = calloc((size_t) gw * gh, 1);
        if (tmp) {
            stbtt_MakeCodepointBitmapSubpixel(&font, tmp, gw, gh, gw, sx, sy, 0, 0, (int) cp);
            for (int y = 0; y < gh; y++) {
                int ty = base + y0 + y;
                if (ty < 0 || ty >= h) continue;
                for (int x = 0; x < gw; x++) {
                    int tx = x0 + x;
                    if (tx >= 0 && tx < CW) m[ty * CW + tx] = tmp[y * gw + x];
                }
            }
            free(tmp);
        }
    }
    if (ncache == (int) (sizeof(cache) / sizeof(cache[0]))) { /* full: start over */
        for (int i = 0; i < ncache; i++) free(cache[i].mask);
        ncache = 0;
    }
    cache[ncache++] = (glyph){cp, dh, m};
    return m;
}

/* ---------------- Drawing ---------------- */

static void fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h && j < TH; j++)
        for (int i = x; i < x + w && i < TW; i++) buf[j * TW + i] = c;
}

static void blit_glyph(int x, int y, uint32_t cp, int dh, uint32_t fg) {
    const uint8_t *m = glyph_mask(cp, dh);
    if (!m) return;
    int h = dh ? 2 * RH : RH;
    uint32_t fr = (fg >> 16) & 255, fgc = (fg >> 8) & 255, fb = fg & 255;
    for (int j = 0; j < h && y + j < TH; j++) {
        uint32_t *row = buf + (y + j) * TW + x;
        for (int i = 0; i < CW; i++) {
            unsigned a = m[j * CW + i];
            if (!a) continue;
            uint32_t d = row[i];
            unsigned da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
            /* blend over the background; a transparent background (mix) takes the opacity of the glyph */
            unsigned oa = a + da * (255 - a) / 255;
            unsigned r = (fr * a + dr * (255 - a)) / 255, g = (fgc * a + dg * (255 - a)) / 255,
                     b = (fb * a + db * (255 - a)) / 255;
            if (!da) r = fr, g = fgc, b = fb;
            row[i] = (oa << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void mosaic(int x, int y, int h, unsigned code, uint32_t fg) {
    unsigned mask = code & 0x3F;
    int sep = (code & 0x40) != 0;
    int cw = CW / 2, rh = h / 3, g = sep ? (CW / 12 > 2 ? CW / 12 : 2) : 0;
    for (int b = 0; b < 6; b++) {
        if (!(mask & (1u << b))) continue;
        int cx = x + (b & 1) * cw, cy = y + (b / 2) * rh;
        int hh = b / 2 == 2 ? h - 2 * rh : rh; /* last row gets the remainder */
        fill(cx + g, cy + g, cw - g, hh - g, fg);
    }
}

/* UTF-8 -> code points (at most n) */
static int decode(const char *s, uint32_t *out, int n) {
    int k = 0;
    const unsigned char *p = (const unsigned char *) s;
    while (*p && k < n) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) {
            c = ((c & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u);
            p += 3;
        } else if (c >= 0xE0 && p[0] && p[1]) {
            c = ((c & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = ((c & 31) << 6) | (p[0] & 63u);
            p += 1;
        }
        out[k++] = c;
    }
    return k;
}

static void text_at(int col, int row, const char *s, uint32_t fg) {
    uint32_t cps[COLS];
    int n = decode(s, cps, COLS);
    for (int i = 0; i < n && col + i < COLS; i++)
        if (cps[i] != ' ') blit_glyph((col + i) * CW, row * RH, cps[i], 0, fg);
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void draw(void) {
    if (!vis || !buf) return;
    has_flash = 0;
    for (int i = 0; i < TW * TH; i++) buf[i] = mix ? 0 : 0xFF000000;
    const cJSON *rows = data && !missing ? cJSON_GetObjectItemCaseSensitive(data, "rows") : NULL;
    if (!cJSON_IsArray(rows)) {
        char head[16];
        snprintf(head, sizeof(head), "P%s", input[0] ? input : page);
        if (mix) fill(0, 0, TW, RH * 1, 0xFF000000);
        text_at(3, 0, head, 0xFFFFFFFF);
        char msg[64];
        if (missing) snprintf(msg, sizeof(msg), _("Page %s not available"), page);
        else snprintf(msg, sizeof(msg), "%s", _("Loading teletext ..."));
        int len = (int) strlen(msg);
        if (mix) fill(0, 12 * RH, TW, RH, 0xFF000000);
        text_at((COLS - len) / 2, 12, msg, 0xFFFFFFFF);
        lv_obj_invalidate(canvas);
        return;
    }
    for (int r = 0; r < ROWS; r++) {
        const cJSON *row = cJSON_GetArrayItem(rows, r);
        if (!row || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "hide"))) continue;
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(row, "t");
        const cJSON *fgs = cJSON_GetObjectItemCaseSensitive(row, "fg");
        const cJSON *bgs = cJSON_GetObjectItemCaseSensitive(row, "bg");
        const cJSON *as = cJSON_GetObjectItemCaseSensitive(row, "a");
        if (!cJSON_IsString(t) || !cJSON_IsString(fgs) || !cJSON_IsString(bgs)) continue;
        uint32_t cps[COLS];
        int n = decode(t->valuestring, cps, COLS);
        for (int i = n; i < COLS; i++) cps[i] = ' ';
        if (r == 0) { /* header row: entered or selected page */
            char head[4];
            if (input[0]) snprintf(head, sizeof(head), "%.3s", input);
            else snprintf(head, sizeof(head), "%s", page);
            for (int i = (int) strlen(head); i < 3; i++) head[i] = '-';
            cps[0] = 'P';
            cps[1] = (uint8_t) head[0];
            cps[2] = (uint8_t) head[1];
            cps[3] = (uint8_t) head[2];
        }
        size_t fl = strlen(fgs->valuestring), bl = strlen(bgs->valuestring);
        size_t al = cJSON_IsString(as) ? strlen(as->valuestring) : 0;
        for (int c = 0; c < COLS; c++) {
            int x = c * CW, y = r * RH;
            int bg = (size_t) c < bl ? hexv(bgs->valuestring[c]) & 7 : 0;
            int fg = (size_t) c < fl ? hexv(fgs->valuestring[c]) & 7 : 7;
            int a = (size_t) c < al ? as->valuestring[c] - '0' : 0;
            if (a < 0 || a > 9) a = 0;
            int dh = (a & 4) && r > 0 && r < 24;
            int h = dh ? 2 * RH : RH;
            if (!(mix && bg == 0)) fill(x, y, CW, h, COLORS[bg]);
            uint32_t code = cps[c];
            if (a & 1) has_flash = 1;
            if (code == ' ' || (a & 2) || ((a & 1) && !flash_on)) continue;
            if (code >= 0xE000 && code < 0xE080) mosaic(x, y, h, code - 0xE000, COLORS[fg]);
            else blit_glyph(x, y, code, dh, COLORS[fg]);
        }
    }
    lv_obj_invalidate(canvas);
}

/* ---------------- Loading ---------------- */

static void url_for(char *out, size_t n, const char *what, int sub) {
    if (sub) snprintf(out, n, "%s%s?sub=%X", pre, what, sub);
    else snprintf(out, n, "%s%s", pre, what);
}

typedef struct {
    int gen;
    char page[4];
    int sub;
} req;

static void on_page(void *user, int status, const char *body, size_t len) {
    (void) len;
    req *q = user;
    int ok = vis && q->gen == gen && !strcmp(q->page, page) && q->sub == sub_sel;
    free(q);
    if (!ok) return;
    if (status == 200 && body) {
        if (last_body && !missing && data && !strcmp(last_body, body)) return; /* unchanged */
        cJSON *j = cJSON_Parse(body);
        if (!j) return;
        free(last_body);
        last_body = strdup(body);
        cJSON_Delete(data);
        data = j;
        missing = 0;
    } else if (status == 404) {
        missing = 1;
    } else {
        return;
    }
    draw();
}

static void load(void) {
    char url[1600];
    url_for(url, sizeof(url), page, sub_sel);
    req *q = malloc(sizeof(*q));
    if (!q) return;
    q->gen = gen;
    snprintf(q->page, sizeof(q->page), "%s", page);
    q->sub = sub_sel;
    fetch_get(url, on_page, q);
    t_load = lv_tick_get();
}

static void step_now(int dir);

static void on_pages(void *user, int status, const char *body, size_t len) {
    (void) len;
    if ((intptr_t) user != gen) return;
    pages_busy = 0;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    cJSON *p = cJSON_DetachItemFromObjectCaseSensitive(j, "pages");
    cJSON_Delete(j);
    if (cJSON_IsArray(p)) {
        cJSON_Delete(pages);
        pages = p;
    } else {
        cJSON_Delete(p); /* error: carry on with the previous list */
    }
    /* wheel/Up/Down that waited for the fresh list */
    int n = step_pending;
    step_pending = 0;
    if (!vis) return;
    for (; n > 0; n--) step_now(1);
    for (; n < 0; n++) step_now(-1);
}

static void load_pages(void) {
    char url[1600];
    if (pages_busy) return;
    pages_busy = 1;
    url_for(url, sizeof(url), "pages", 0);
    fetch_get(url, on_pages, (void *) (intptr_t) gen);
}

static void go(const char *p) {
    char np[4];
    snprintf(np, sizeof(np), "%.3s", p); /* p may be page itself */
    free(last_body);
    last_body = NULL;
    memcpy(page, np, sizeof(page));
    sub_sel = 0;
    input[0] = 0;
    missing = 0;
    cJSON_Delete(data);
    data = NULL;
    load();
    draw();
}

/* next/previous existing page according to the list */
static void step_now(int dir) {
    int n = cJSON_GetArraySize(pages);
    if (!n) return;
    long cur = strtol(page, NULL, 16);
    if (dir > 0) {
        for (int i = 0; i < n; i++) {
            const char *s = cJSON_GetArrayItem(pages, i)->valuestring;
            if (s && strtol(s, NULL, 16) > cur) {
                go(s);
                return;
            }
        }
        go(cJSON_GetArrayItem(pages, 0)->valuestring);
    } else {
        for (int i = n - 1; i >= 0; i--) {
            const char *s = cJSON_GetArrayItem(pages, i)->valuestring;
            if (s && strtol(s, NULL, 16) < cur) {
                go(s);
                return;
            }
        }
        go(cJSON_GetArrayItem(pages, n - 1)->valuestring);
    }
}

/* wheel/Up/Down: first fetch the list of existing pages (our own
 * store keeps filling up), then jump; steps during the
 * request are counted too */
static void step(int dir) {
    step_pending += dir;
    load_pages();
}

static int subcode(const cJSON *v) {
    return cJSON_IsString(v) && v->valuestring ? (int) strtol(v->valuestring, NULL, 16) : 0;
}

static void sub_step(int dir) {
    const cJSON *subs = cJSON_GetObjectItemCaseSensitive(data, "subs");
    int n = cJSON_GetArraySize(subs);
    if (n < 2) return;
    /* subpage codes come as hex text ("0001") */
    int cur = sub_sel ? sub_sel : subcode(cJSON_GetObjectItemCaseSensitive(data, "sub"));
    int i = 0;
    for (int k = 0; k < n; k++)
        if (subcode(cJSON_GetArrayItem(subs, k)) == cur) i = k;
    i = (i + dir + n) % n;
    sub_sel = subcode(cJSON_GetArrayItem(subs, i));
    free(last_body);
    last_body = NULL;
    load();
}

/* ---------------- Interface ---------------- */

void ttx_init(const char *res_dir, const ttx_actions *a) {
    act = *a;
    font_load(res_dir);
    lv_obj_t *scr = lv_screen_active();
    backdrop = ui_box(scr, 0x000000, LV_OPA_COVER, 0);
    lv_obj_set_size(backdrop, 1920, 1080);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, (1920 - TW) / 2, (1080 - TH) / 2);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
}

void ttx_set_source(int local) {
    if (vis) ttx_close();
    pre[0] = 0;
    cJSON_Delete(pages);
    pages = NULL;
    pages_busy = step_pending = 0;
    gen++;
    page[0] = 0; /* new channel: its start page when opening */
    if (local) snprintf(pre, sizeof(pre), "local:ttx/");
}

int ttx_available(void) { return pre[0] != 0; }

void ttx_open(void) {
    if (!pre[0]) return;
    if (!buf) {
        buf = malloc((size_t) TW * TH * 4);
        if (!buf) return;
        lv_canvas_set_buffer(canvas, buf, TW, TH, LV_COLOR_FORMAT_ARGB8888);
    }
    vis = 1;
    mix = 0;
    input[0] = 0;
    flash_on = 1;
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(backdrop);
    lv_obj_move_foreground(canvas);
    if (!page[0]) {
        int p = act.initial_page ? act.initial_page() : 0;
        if (p >= 0x100 && p <= 0x8FF) snprintf(page, sizeof(page), "%03X", p);
    }
    go(page[0] ? page : "100");
    step_pending = 0;
    t_flash = lv_tick_get();
}

void ttx_close(void) {
    if (!vis) return;
    vis = 0;
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
}

int ttx_visible(void) { return vis; }

int ttx_key(int key) {
    if (!vis) return 0;
    if (KEY_IS_DIGIT(key)) {
        int d = key - KEY_DIGIT0;
        size_t n = strlen(input);
        if (n == 0 && (d < 1 || d > 8)) return 1;
        input[n] = (char) ('0' + d);
        input[n + 1] = 0;
        if (n + 1 == 3) {
            char p[4];
            snprintf(p, sizeof(p), "%s", input);
            go(p);
        } else {
            draw();
        }
        return 1;
    }
    const cJSON *links = cJSON_GetObjectItemCaseSensitive(data, "links");
    switch (key) {
    case KEY_UP: step(1); break;
    case KEY_DOWN: step(-1); break;
    case KEY_RIGHT: sub_step(1); break;
    case KEY_LEFT: sub_step(-1); break;
    case KEY_RED:
    case KEY_GREEN:
    case KEY_YELLOW:
    case KEY_BLUE: {
        int idx = key == KEY_RED ? 0 : key == KEY_GREEN ? 1 : key == KEY_YELLOW ? 2 : 3;
        const cJSON *l = cJSON_GetArrayItem(links, idx);
        if (cJSON_IsString(l) && l->valuestring[0]) go(l->valuestring);
        else if (key == KEY_RED) step(-1);
        else if (key == KEY_GREEN) step(1);
        else if (key == KEY_YELLOW) go("100");
        else ttx_close();
        break;
    }
    case KEY_OK:
        mix = !mix;
        if (mix) lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
        draw();
        break;
    case KEY_BACK: ttx_close(); break;
    case KEY_CH_UP:
        ttx_close();
        if (act.zap) act.zap(1);
        break;
    case KEY_CH_DOWN:
        ttx_close();
        if (act.zap) act.zap(-1);
        break;
    default: break;
    }
    return 1;
}

void ttx_tick(void) {
    if (!vis) return;
    uint32_t t = lv_tick_get();
    if (t - t_load >= 1000) load();
    if (t - t_flash >= 500) {
        t_flash = t;
        flash_on = !flash_on;
        if (has_flash) draw();
    }
}
