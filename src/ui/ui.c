/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Fonts, images and building blocks of the user interface (without SDL).
 */
#include "ui.h"
#include "i18n.h"

#include "http.h"
#include "imgtrim.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void (*g_wake)(void);

/* ---------------- Fonts ---------------- */

typedef struct {
    int px, bold;
    lv_font_t *font;
} font_slot;

static font_slot g_fonts[32];
static int g_nfonts;
static void *g_ttf[2];
static size_t g_ttf_len[2];

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = n > 0 ? malloc((size_t) n) : NULL;
    if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) *len = (size_t) n;
    return buf;
}

const lv_font_t *ui_font(int px, int bold) {
    bold = bold ? 1 : 0;
    for (int i = 0; i < g_nfonts; i++)
        if (g_fonts[i].px == px && g_fonts[i].bold == bold) return g_fonts[i].font;
    if (!g_ttf[bold] || g_nfonts >= (int) (sizeof(g_fonts) / sizeof(g_fonts[0]))) return LV_FONT_DEFAULT;
    lv_font_t *f = lv_tiny_ttf_create_data(g_ttf[bold], g_ttf_len[bold], px);
    if (!f) return LV_FONT_DEFAULT;
    /* Lato does not have symbols (arrows, dots) */
    f->fallback = px >= 20 ? &lv_font_montserrat_22 : &lv_font_montserrat_14;
    g_fonts[g_nfonts++] = (font_slot){px, bold, f};
    return f;
}

/* ---------------- Images ---------------- */

/* Fully loaded images: the file itself (PNG/JPEG), LVGL decodes when
 * drawing and keeps the result in its image cache. */
typedef struct img_entry {
    struct img_entry *next;
    char url[512];
    lv_image_dsc_t dsc;
    uint8_t *data;
    int failed;
    int transient; /* only the last TRANSIENT_MAX stay (EPG images) */
} img_entry;

#define TRANSIENT_MAX 3

typedef struct waiter {
    struct waiter *next;
    ui_image_cb cb;
    void *user;
} waiter;

typedef struct job {
    struct job *next;  /* queue or done list (under g_jlock) */
    struct job *pnext; /* running jobs (main thread only) */
    char url[512];
    waiter *waiters;
    uint8_t *data;
    size_t len;
    int raw_w, raw_h; /* > 0: data is finished pixels (ARGB8888) */
    int transient;
} job;

#define FETCH_THREADS 4

static img_entry *g_cache;
static job *g_pending; /* main thread: requested, not yet delivered */
static pthread_mutex_t g_jlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_jcond = PTHREAD_COND_INITIALIZER;
static job *g_queue, *g_queue_tail, *g_done;
static int g_threads;

/* login for images from the tvh web server (logos, EPG images from imagecache);
 * only for URLs with this prefix, set before the first fetch */
static char g_login_prefix[160], g_login_user[64], g_login_pass[64];

void ui_image_login(const char *url_prefix, const char *user, const char *pass) {
    snprintf(g_login_prefix, sizeof(g_login_prefix), "%s", url_prefix ? url_prefix : "");
    snprintf(g_login_user, sizeof(g_login_user), "%s", user ? user : "");
    snprintf(g_login_pass, sizeof(g_login_pass), "%s", pass ? pass : "");
}

static img_entry *cache_find(const char *url) {
    for (img_entry *e = g_cache; e; e = e->next)
        if (!strcmp(e->url, url)) return e;
    return NULL;
}

static void fetch(job *j) {
    http_conn *c = calloc(1, sizeof(*c));
    int login = g_login_prefix[0] && g_login_user[0] && !strncmp(j->url, g_login_prefix, strlen(g_login_prefix));
    if (c && (login ? http_open_quiet(j->url, g_login_user, g_login_pass, c) : http_open(j->url, NULL, NULL, c)) == 0) {
        size_t cap = 64 * 1024;
        j->data = malloc(cap);
        while (j->data) {
            if (j->len == cap) {
                if (cap >= 4 * 1024 * 1024) break;
                uint8_t *n = realloc(j->data, cap * 2);
                if (!n) break;
                j->data = n;
                cap *= 2;
            }
            ssize_t r = http_read(c, j->data + j->len, cap - j->len);
            if (r <= 0) break;
            j->len += (size_t) r;
        }
        http_close(c);
    }
    free(c);
}

/* PNG (channel logos): cut away the border (imgtrim.c); the result stays a PNG, LVGL decodes it as before when
 * drawing. Fetch thread - LVGL's lodepng belongs to the main thread. */
static void trim_png(job *j) {
    uint8_t *png = NULL;
    size_t len = 0;
    if (!img_trim_png(j->data, j->len, &png, &len)) return;
    free(j->data);
    j->data = png;
    j->len = len;
}

/* decode JPEG ourselves (stb can do progressive, LVGL's TJPGD cannot);
 * at most half HD size, no view shows more */
static void decode_jpeg(job *j) {
    uint8_t *px = NULL;
    int w = 0, h = 0;
    if (!img_decode_jpeg(j->data, j->len, 960, 540, &px, &w, &h)) return;
    free(j->data);
    j->data = px;
    j->len = (size_t) w * (size_t) h * 4;
    j->raw_w = w;
    j->raw_h = h;
}

static void *fetch_thread(void *arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&g_jlock);
        while (!g_queue) pthread_cond_wait(&g_jcond, &g_jlock);
        job *j = g_queue;
        g_queue = j->next;
        if (!g_queue) g_queue_tail = NULL;
        pthread_mutex_unlock(&g_jlock);

        fetch(j);
        trim_png(j);
        decode_jpeg(j);

        pthread_mutex_lock(&g_jlock);
        j->next = g_done;
        g_done = j;
        pthread_mutex_unlock(&g_jlock);
        if (g_wake) g_wake();
    }
    return NULL;
}

static void fetch_ex(const char *url, ui_image_cb cb, void *user, int transient);

void ui_image_fetch(const char *url, ui_image_cb cb, void *user) { fetch_ex(url, cb, user, 0); }

void ui_image_fetch_transient(const char *url, ui_image_cb cb, void *user) { fetch_ex(url, cb, user, 1); }

/* transient entries: free the oldest beyond TRANSIENT_MAX (new ones
 * go to the front of the list) - whoever shows them has switched before */
static void transient_trim(void) {
    int n = 0;
    for (img_entry **pp = &g_cache; *pp;) {
        img_entry *e = *pp;
        if (e->transient && ++n > TRANSIENT_MAX) {
            *pp = e->next;
            lv_image_cache_drop(&e->dsc);
            free(e->data);
            free(e);
            continue;
        }
        pp = &e->next;
    }
}

static void fetch_ex(const char *url, ui_image_cb cb, void *user, int transient) {
    if (!url || !*url) {
        cb(user, url, NULL);
        return;
    }
    img_entry *e = cache_find(url);
    if (e) {
        cb(user, url, e->failed ? NULL : &e->dsc);
        return;
    }
    waiter *w = calloc(1, sizeof(*w));
    if (!w) return;
    w->cb = cb;
    w->user = user;
    for (job *j = g_pending; j; j = j->pnext) {
        if (!strcmp(j->url, url)) { /* already running */
            w->next = j->waiters;
            j->waiters = w;
            return;
        }
    }
    job *j = calloc(1, sizeof(*j));
    if (!j) {
        free(w);
        return;
    }
    snprintf(j->url, sizeof(j->url), "%s", url);
    j->transient = transient;
    j->waiters = w;
    j->pnext = g_pending;
    g_pending = j;

    pthread_mutex_lock(&g_jlock);
    if (g_queue_tail) g_queue_tail->next = j;
    else g_queue = j;
    g_queue_tail = j;
    pthread_cond_signal(&g_jcond);
    pthread_mutex_unlock(&g_jlock);

    while (g_threads < FETCH_THREADS) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int ok = pthread_create(&t, &at, fetch_thread, NULL) == 0;
        pthread_attr_destroy(&at);
        if (!ok) break;
        g_threads++;
    }
}

void ui_poll(void) {
    pthread_mutex_lock(&g_jlock);
    job *list = g_done;
    g_done = NULL;
    pthread_mutex_unlock(&g_jlock);
    while (list) {
        job *j = list;
        list = j->next;
        for (job **pp = &g_pending; *pp; pp = &(*pp)->pnext) {
            if (*pp == j) {
                *pp = j->pnext;
                break;
            }
        }
        img_entry *e = cache_find(j->url);
        if (!e) {
            e = calloc(1, sizeof(*e));
            if (!e) continue;
            snprintf(e->url, sizeof(e->url), "%s", j->url);
            if (j->data && j->len > 8) {
                e->data = j->data;
                j->data = NULL;
                e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                if (j->raw_w > 0) { /* decoded JPEG */
                    e->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
                    e->dsc.header.w = (uint32_t) j->raw_w;
                    e->dsc.header.h = (uint32_t) j->raw_h;
                    e->dsc.header.stride = (uint32_t) j->raw_w * 4;
                } else {
                    e->dsc.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
                }
                e->dsc.data = e->data;
                e->dsc.data_size = (uint32_t) j->len;
            } else {
                e->failed = 1;
                mt_log("[ui] image not loaded: %s", j->url);
            }
            e->transient = j->transient;
            e->next = g_cache;
            g_cache = e;
            if (e->transient) transient_trim();
        }
        while (j->waiters) {
            waiter *w = j->waiters;
            j->waiters = w->next;
            w->cb(w->user, j->url, e->failed ? NULL : &e->dsc);
            free(w);
        }
        free(j->data);
        free(j);
    }
}

/* ---------------- Building blocks ---------------- */

lv_obj_t *ui_box(lv_obj_t *parent, uint32_t color, lv_opa_t opa, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, int px, int bold, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, ui_font(px, bold), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

int ui_hit(lv_obj_t *obj, int x, int y) {
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return 0;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

void ui_label_dots(lv_obj_t *label) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
}

const char *ui_weekday(int wday) {
    static const char *d[] = {N_("Sun"), N_("Mon"), N_("Tue"), N_("Wed"), N_("Thu"), N_("Fri"), N_("Sat")};
    return _(d[((wday % 7) + 7) % 7]);
}

void ui_fmt_hm(long t, char *out, size_t n) {
    time_t tt = (time_t) t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void ui_oneline(char *out, size_t n, const char *in, size_t max) {
    size_t o = 0;
    const char *p = in ? in : "";
    if (!n) return;
    if (max >= n) max = n - 1;
    for (; *p && o < max; p++) {
        char c = *p;
        /* some EPG sources deliver "\n" as two characters */
        if (c == '\\' && (p[1] == 'n' || p[1] == 'r' || p[1] == 't')) {
            p++;
            c = '\n';
        }
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = c;
    }
    /* cut in the middle of a UTF-8 character: drop the whole character */
    if (*p && (*p & 0xC0) == 0x80) {
        while (o > 0 && (out[o - 1] & 0xC0) == 0x80) o--;
        if (o > 0) o--;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* ---------------- Startup ---------------- */

void ui_res_init(const char *res_dir, void (*wake)(void)) {
    g_wake = wake;
    char path[512];
    snprintf(path, sizeof(path), "%s/fonts/Lato-Regular.ttf", res_dir);
    g_ttf[0] = read_file(path, &g_ttf_len[0]);
    snprintf(path, sizeof(path), "%s/fonts/Lato-Bold.ttf", res_dir);
    g_ttf[1] = read_file(path, &g_ttf_len[1]);
    mt_log("[ui] fonts %s/%s", g_ttf[0] ? "ok" : "MISSING", g_ttf[1] ? "ok" : "MISSING");
}
