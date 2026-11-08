/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "subs.h"
#include "i18n.h"

#include "cJSON.h"
#include "fetch.h"
#include "keys.h"
#include "log.h"
#include "osd.h"
#include "prefs.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRAP (1LL << 33)
#define POLL_MS 700
#define MAX_EVENTS 60

static const uint32_t TTX_COLORS[8] = {0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

static subs_actions act;
static char pre[16]; /* "local:ttx/" (store of the HTSP subscription), "" = no subtitles */
static int channel, gen;
static subs_option opts[SUBS_MAX];
static int nopts, lists_pending;
static char cur_id[32]; /* "" = off */
static int dvb_pid, dvb_page;
static cJSON *events; /* array, oldest first */
static long next_seq;
static uint32_t t_poll, t_draw;
static int offset_ms, adjusting;
static int poll_busy;
static double epoch = -1;  /* time base of the events (local), change = discard */

static lv_obj_t *box;         /* teletext rows */
static char shown_sig[2048];  /* displayed text, do not rebuild if unchanged */
static lv_obj_t *dvb_canvas;  /* DVB bitmaps, 1920x1080 */
static uint32_t *dvb_buf;
static long shown_seq = -2;

/* ---------------- Helpers ---------------- */

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : 0;
}

/* a - b modulo 2^33, signed */
static long long pts_diff(long long a, long long b) {
    long long d = (a - b) % WRAP;
    if (d < 0) d += WRAP;
    return d >= WRAP / 2 ? d - WRAP : d;
}

static const char *lang_name(const char *l) {
    const char *n = i18n_iso639(l);
    return n ? n : l;
}

/* only characters that Lato surely has (otherwise one LVGL error per character) */
static void sanitize(char *out, size_t n, const char *in) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *) in;
    while (*p && o + 4 < n) {
        uint32_t c = *p;
        int len = 1;
        if (c >= 0xF0) len = 4, c &= 7;
        else if (c >= 0xE0) len = 3, c &= 15;
        else if (c >= 0xC0) len = 2, c &= 31;
        for (int k = 1; k < len; k++) {
            if ((p[k] & 0xC0) != 0x80) {
                len = k;
                break;
            }
            c = (c << 6) | (p[k] & 63u);
        }
        int ok = (c >= 0x20 && c < 0x7F) || (c >= 0xA0 && c <= 0x24F) || (c >= 0x2010 && c <= 0x2026) ||
                 c == 0x20AC;
        if (ok) {
            memcpy(out + o, p, (size_t) len);
            o += (size_t) len;
        } else if (c == 0x266A || c == 0x266B) { /* musical notes */
            out[o++] = '#';
        }
        p += len;
    }
    out[o] = 0;
}

/* ---------------- Display ---------------- */

static void clear_text(void) {
    if (shown_sig[0]) {
        lv_obj_clean(box);
        shown_sig[0] = 0;
    }
}

static void show_text(const cJSON *ev) {
    char sig[sizeof(shown_sig)];
    size_t o = 0;
    const cJSON *l;
    cJSON_ArrayForEach(l, cJSON_GetObjectItemCaseSensitive(ev, "lines")) {
        o += (size_t) snprintf(sig + o, sizeof(sig) - o, "%d|%s\n", (int) jnum(l, "c"), jstr(l, "t"));
        if (o >= sizeof(sig)) break;
    }
    if (o == 0) {
        clear_text();
        return;
    }
    sig[sizeof(sig) - 1] = 0;
    if (!strcmp(sig, shown_sig)) return;
    snprintf(shown_sig, sizeof(shown_sig), "%s", sig);
    lv_obj_clean(box);
    cJSON_ArrayForEach(l, cJSON_GetObjectItemCaseSensitive(ev, "lines")) {
        char t[512];
        sanitize(t, sizeof(t), jstr(l, "t"));
        if (!t[0]) continue;
        lv_obj_t *b = ui_box(box, 0x000000, 199, 0);
        lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(b, 16, 0);
        lv_obj_set_style_pad_ver(b, 2, 0);
        lv_obj_t *lbl = ui_label(b, 46, 0, TTX_COLORS[(int) jnum(l, "c") & 7]);
        lv_label_set_text(lbl, t);
    }
}

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64dec(const char *in, uint8_t *out, size_t max) {
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && o < max; in++) {
        int v = b64v((unsigned char) *in);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t) (acc >> bits);
        }
    }
    return o;
}

/* DVB: regions (palette "rrggbbaa"..., run lengths base64: pairs count-1, index) */
static void draw_dvb(const cJSON *ev) {
    if (!dvb_buf) return;
    memset(dvb_buf, 0, (size_t) 1920 * 1080 * 4);
    if (ev) {
        double ew = jnum(ev, "w") > 0 ? jnum(ev, "w") : 720, eh = jnum(ev, "h") > 0 ? jnum(ev, "h") : 576;
        double sx = 1920.0 / ew, sy = 1080.0 / eh;
        const cJSON *r;
        cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(ev, "regions")) {
            int rx = (int) jnum(r, "x"), ry = (int) jnum(r, "y"), rw = (int) jnum(r, "w"), rh = (int) jnum(r, "h");
            if (rw <= 0 || rh <= 0 || rw > 4096 || rh > 4096) continue;
            const char *pal = jstr(r, "pal");
            uint32_t pc[256] = {0};
            int np = 0;
            for (size_t i = 0; i + 8 <= strlen(pal) && np < 256; i += 8) {
                char h[9];
                memcpy(h, pal + i, 8);
                h[8] = 0;
                uint32_t v = (uint32_t) strtoul(h, NULL, 16); /* rrggbbaa */
                pc[np++] = ((v & 0xFF) << 24) | (v >> 8);  /* -> aarrggbb */
            }
            const char *rle = jstr(r, "rle");
            size_t cap = strlen(rle) * 3 / 4 + 4;
            uint8_t *bin = malloc(cap);
            uint32_t *px = calloc((size_t) rw * rh, 4);
            if (bin && px) {
                size_t n = b64dec(rle, bin, cap), o = 0, total = (size_t) rw * rh;
                for (size_t i = 0; i + 1 < n && o < total; i += 2) {
                    size_t cnt = (size_t) bin[i] + 1;
                    uint32_t col = pc[bin[i + 1]];
                    for (size_t q = 0; q < cnt && o < total; q++) px[o++] = col;
                }
                /* scaled (nearest neighbour) to the screen */
                int dx0 = (int) (rx * sx), dy0 = (int) (ry * sy), dw = (int) (rw * sx), dh = (int) (rh * sy);
                for (int y = 0; y < dh; y++) {
                    int ty = dy0 + y;
                    if (ty < 0 || ty >= 1080) continue;
                    int syy = (int) (y / sy);
                    if (syy >= rh) syy = rh - 1;
                    for (int x = 0; x < dw; x++) {
                        int tx = dx0 + x;
                        if (tx < 0 || tx >= 1920) continue;
                        int sxx = (int) (x / sx);
                        if (sxx >= rw) sxx = rw - 1;
                        uint32_t c = px[syy * rw + sxx];
                        if (c >> 24) dvb_buf[ty * 1920 + tx] = c;
                    }
                }
            }
            free(bin);
            free(px);
        }
    }
    lv_obj_invalidate(dvb_canvas);
}

/* ---------------- Loading ---------------- */

static void on_events(void *user, int status, const char *body, size_t len) {
    (void) len;
    poll_busy = 0;
    if ((intptr_t) user != gen || status != 200 || !body) return;
    cJSON *j = cJSON_Parse(body);
    if (!j) return;
    cJSON *evs = cJSON_DetachItemFromObjectCaseSensitive(j, "events");
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "next")))
        next_seq = (long) cJSON_GetObjectItemCaseSensitive(j, "next")->valuedouble;
    /* new time base (jump in timeshift, reconnected): old events
     * no longer match the picture */
    const cJSON *ep = cJSON_GetObjectItemCaseSensitive(j, "epoch");
    if (cJSON_IsNumber(ep) && ep->valuedouble != epoch) {
        if (epoch >= 0 && events) mt_log("[subs] new time base - %d old events dropped", cJSON_GetArraySize(events));
        epoch = ep->valuedouble;
        cJSON_Delete(events);
        events = NULL;
    }
    cJSON_Delete(j);
    if (!cJSON_IsArray(evs)) {
        cJSON_Delete(evs);
        return;
    }
    if (!events) events = cJSON_CreateArray();
    cJSON *e;
    while ((e = cJSON_DetachItemFromArray(evs, 0)) != NULL) cJSON_AddItemToArray(events, e);
    cJSON_Delete(evs);
    int keep = dvb_pid ? 12 : MAX_EVENTS;
    while (cJSON_GetArraySize(events) > keep) cJSON_DeleteItemFromArray(events, 0);
}

static void poll(void) {
    if (!cur_id[0] || poll_busy) return;
    char url[1800];
    if (dvb_pid) snprintf(url, sizeof(url), "%sdvbsub?pid=%d&page=%d&since=%ld", pre, dvb_pid, dvb_page, next_seq);
    else snprintf(url, sizeof(url), "%ssubtitles?page=%s&since=%ld", pre, cur_id, next_seq);
    poll_busy = 1;
    t_poll = lv_tick_get();
    fetch_get(url, on_events, (void *) (intptr_t) gen);
}

static void stop(void) {
    cur_id[0] = 0;
    dvb_pid = dvb_page = 0;
    cJSON_Delete(events);
    events = NULL;
    next_seq = 0;
    epoch = -1;
    clear_text();
    if (dvb_canvas && shown_seq != -2) {
        draw_dvb(NULL);
        lv_obj_add_flag(dvb_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    shown_seq = -2;
}

static void start(const char *id) {
    stop();
    snprintf(cur_id, sizeof(cur_id), "%s", id);
    if (sscanf(id, "dvb:%d:%d", &dvb_pid, &dvb_page) == 2) {
        if (!dvb_buf) {
            dvb_buf = calloc((size_t) 1920 * 1080, 4);
            if (!dvb_buf) {
                stop();
                return;
            }
            lv_canvas_set_buffer(dvb_canvas, dvb_buf, 1920, 1080, LV_COLOR_FORMAT_ARGB8888);
        }
        lv_obj_remove_flag(dvb_canvas, LV_OBJ_FLAG_HIDDEN);
    } else {
        dvb_pid = dvb_page = 0;
    }
    gen++;
    poll_busy = 0;
    mt_log("[subs] subtitles %s", id);
    poll();
}

static void lists_done(void) {
    if (--lists_pending > 0) return;
    char key[32];
    snprintf(key, sizeof(key), "subs.%d", channel);
    const cJSON *p = prefs_get(key);
    if (!cJSON_IsString(p)) return;
    for (int i = 0; i < nopts; i++)
        if (!strcmp(opts[i].id, p->valuestring) && strcmp(cur_id, opts[i].id)) start(opts[i].id);
}

static void on_dvb_list(void *user, int status, const char *body, size_t len) {
    (void) len;
    if ((intptr_t) user != gen) return;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(j, "streams")) {
        if (nopts >= SUBS_MAX) break;
        subs_option *o = &opts[nopts++];
        snprintf(o->id, sizeof(o->id), "dvb:%d:%d", (int) jnum(s, "pid"), (int) jnum(s, "page"));
        int t = (int) jnum(s, "type");
        snprintf(o->label, sizeof(o->label), _("Subtitles %s%s (DVB)"), lang_name(jstr(s, "lang")),
                 t >= 0x20 && t <= 0x25 ? _(" (hard of hearing)") : "");
    }
    cJSON_Delete(j);
    lists_done();
}

static void on_ttx_list(void *user, int status, const char *body, size_t len) {
    (void) len;
    if ((intptr_t) user != gen) return;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *p, *info = cJSON_GetObjectItemCaseSensitive(j, "info");
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "pages")) {
        if (nopts >= SUBS_MAX || !cJSON_IsString(p)) break;
        subs_option *o = &opts[nopts++];
        snprintf(o->id, sizeof(o->id), "%s", p->valuestring);
        /* details from the PMT (language, type): without page number */
        const cJSON *e, *pi = NULL;
        cJSON_ArrayForEach(e, info) if (!strcmp(jstr(e, "page"), p->valuestring)) pi = e;
        if (pi && jstr(pi, "lang")[0]) {
            snprintf(o->label, sizeof(o->label), (int) jnum(pi, "type") == 5 ? _("Subtitles %s for the hard of hearing")
                                                                              : _("Subtitles %s"),
                     lang_name(jstr(pi, "lang")));
        } else {
            snprintf(o->label, sizeof(o->label), _("Subtitles %s (teletext)"), p->valuestring);
        }
    }
    cJSON_Delete(j);
    lists_done();
}

static void load_lists(void) {
    char url[1600];
    nopts = 0;
    lists_pending = 2;
    snprintf(url, sizeof(url), "%sdvbsub", pre);
    fetch_get(url, on_dvb_list, (void *) (intptr_t) gen);
    snprintf(url, sizeof(url), "%ssubtitles", pre);
    fetch_get(url, on_ttx_list, (void *) (intptr_t) gen);
}

/* ---------------- Interface ---------------- */

void subs_init(const subs_actions *a) {
    act = *a;
    lv_obj_t *scr = lv_screen_active();
    dvb_canvas = lv_canvas_create(scr);
    lv_obj_set_pos(dvb_canvas, 0, 0);
    lv_obj_add_flag(dvb_canvas, LV_OBJ_FLAG_HIDDEN);
    box = ui_box(scr, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(box, 1920, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -70);
}

static uint32_t t_lists_retry;

static void load_offset(void) {
    char key[40];
    snprintf(key, sizeof(key), "subs.offset.%d", channel);
    const cJSON *v = prefs_get(key);
    if (!cJSON_IsNumber(v)) v = prefs_get("subs.offset");
    offset_ms = cJSON_IsNumber(v) ? v->valueint : 0;
}

static void save_offset(void) {
    char key[40];
    snprintf(key, sizeof(key), "subs.offset.%d", channel);
    prefs_set(key, cJSON_CreateNumber(offset_ms));
    prefs_set("subs.offset", cJSON_CreateNumber(offset_ms));
}

void subs_set_source(int local, int ch) {
    stop();
    gen++;
    nopts = 0;
    lists_pending = 0;
    channel = ch;
    if (adjusting) {
        adjusting = 0;
        osd_message(NULL, 0, 0);
    }
    load_offset();
    pre[0] = 0;
    if (!local) return;
    /* DVB subtitles are known with the subscription, teletext subtitle pages
     * only once one has been sent - keep checking the lists (costs
     * nothing) */
    snprintf(pre, sizeof(pre), "local:ttx/");
    t_lists_retry = lv_tick_get() + 1500;
}

int subs_options(subs_option *out, int max, int *current) {
    int n = 0;
    if (current) *current = -1;
    for (int i = 0; i < nopts && n < max; i++) {
        out[n] = opts[i];
        if (current && !strcmp(opts[i].id, cur_id)) *current = n;
        n++;
    }
    return n;
}

void subs_choose(const char *id) {
    char key[32];
    snprintf(key, sizeof(key), "subs.%d", channel);
    prefs_set(key, id ? cJSON_CreateString(id) : NULL);
    if (id) start(id);
    else stop();
}

void subs_select_session(const char *id) {
    if (!pre[0]) return;
    if (id) start(id);
    else stop();
}

int subs_enabled(void) { return cur_id[0] != 0; }

void subs_offset_label(char *out, size_t n) {
    int a = offset_ms < 0 ? -offset_ms : offset_ms;
    snprintf(out, n, "%s%d ms", offset_ms > 0 ? "+" : offset_ms < 0 ? "\xE2\x88\x92" : "\xC2\xB1", a);
}

static void show_adjust(void) {
    char l[32], t[160];
    subs_offset_label(l, sizeof(l));
    snprintf(t, sizeof(t), _("Subtitle offset %s     Left earlier \xC2\xB7 Right later \xC2\xB7 OK done"), l);
    osd_message(t, 0, 0);
}

void subs_adjust_start(void) {
    adjusting = 1;
    show_adjust();
}

int subs_adjusting(void) { return adjusting; }

int subs_adjust_key(int key) {
    if (!adjusting) return 0;
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        offset_ms += key == KEY_RIGHT ? 25 : -25;
        save_offset();
        shown_seq = -2; /* re-evaluate the DVB image */
        show_adjust();
    } else if (key == KEY_OK || key == KEY_BACK) {
        adjusting = 0;
        osd_message(NULL, 0, 0);
    }
    return 1;
}

void subs_tick(void) {
    uint32_t t = lv_tick_get();
    /* fetch the lists (after the channel starts; without a result again every 5 s) */
    if (pre[0] && t_lists_retry && (int32_t) (t - t_lists_retry) >= 0) {
        load_lists();
        t_lists_retry = t + 5000;
    }
    if (!cur_id[0]) return;
    if (t - t_poll >= POLL_MS) poll();
    if (t - t_draw < 100) return;
    t_draw = t;
    long long now = act.now_pts ? act.now_pts() : -1;
    if (now >= 0) now = ((now - (long long) offset_ms * 90) % WRAP + WRAP) % WRAP;
    /* the most recent event up to now (with equal PTS the later one) - after
     * jumps in timeshift the list is not ordered by PTS */
    const cJSON *show = NULL, *e;
    long long best = -1;
    if (now >= 0) {
        cJSON_ArrayForEach(e, events) {
            double p = jnum(e, "pts");
            if (p < 0) continue;
            long long d = pts_diff(now, (long long) p);
            if (d >= 0 && (best < 0 || d <= best)) {
                best = d;
                show = e;
            }
        }
    }
    if (dvb_pid) {
        /* page timeout: hide after it */
        if (show && jnum(show, "timeout") > 0 && pts_diff(now, (long long) jnum(show, "pts")) > jnum(show, "timeout") * 90000)
            show = NULL;
        long seq = show ? (long) jnum(show, "seq") : -1;
        if (seq != shown_seq) {
            shown_seq = seq;
            draw_dvb(show && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(show, "regions")) ? show : NULL);
        }
        return;
    }
    if (show) show_text(show);
    else clear_text();
}
