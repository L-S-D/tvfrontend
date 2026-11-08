/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recbrowser.h"
#include "i18n.h"

#include "choice.h"
#include "keys.h"
#include "log.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OPA(f) ((lv_opa_t) ((f) * 255 + 0.5))
#define PAGE 9
#define ROW_H 100
#define HDR_H 70
#define FOOT_H 50
#define LIST_W 1100

enum { R_REC, R_FOLDER, R_TIMERS, R_TIMER };

typedef struct {
    int type;
    const cJSON *item;  /* R_REC: recording, R_TIMER: timer */
    char title[256];    /* folder */
    const cJSON **items; /* folder: recordings, newest first */
    int nitems;
} row_t;

typedef struct {
    lv_obj_t *obj, *icon, *badge, *badge_l, *title, *sub, *bar, *check, *live;
    char icon_url[512];
} row_ui;

static recbrowser_actions act;
static cJSON *recs, *timers;
static int have_recs;
static row_t *rows;
static int nrows, sel, scroll, vis;
static char folder[256]; /* "" = top level */
static int in_folder, in_timers;
static char root_key[300], focus[64];

static lv_obj_t *root, *h_folder, *h_pos, *empty, *detail;
static lv_obj_t *d_icon, *d_title, *d_sub, *d_meta, *d_meta2, *d_tech, *d_status, *d_desc;
static char d_icon_url[512];
static row_ui ui[PAGE];

/* options of the open selection */
static const char *opt_action[4];
static int n_opt;
static const cJSON *opt_item;

/* ---------------- Helpers ---------------- */

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : 0;
}

static int jtrue(const cJSON *o, const char *k) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k)); }

static int has(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return i && !cJSON_IsNull(i) && !cJSON_IsFalse(i) && !(cJSON_IsString(i) && !*i->valuestring);
}

void rec_fmt_date(long t, char *out, size_t n) {
    time_t tt = (time_t) t, now = time(NULL);
    struct tm tm, tn;
    localtime_r(&tt, &tm);
    localtime_r(&now, &tn);
    if (tm.tm_year != tn.tm_year)
        snprintf(out, n, _("%s %02d/%02d/%04d %02d:%02d"), ui_weekday(tm.tm_wday), tm.tm_mday, tm.tm_mon + 1,
                 tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    else
        snprintf(out, n, _("%s %02d/%02d %02d:%02d"), ui_weekday(tm.tm_wday), tm.tm_mday, tm.tm_mon + 1, tm.tm_hour,
                 tm.tm_min);
}

void rec_fmt_duration(double s, char *out, size_t n) {
    long x = s > 0 ? (long) s : 0;
    long h = x / 3600, m = (x % 3600) / 60, sec = x % 60;
    if (h) snprintf(out, n, "%ld:%02ld:%02ld", h, m, sec);
    else snprintf(out, n, "%ld:%02ld", m, sec);
}

static void fmt_len(double s, char *out, size_t n) {
    if (s < 60) snprintf(out, n, "%d s", (int) s);
    else snprintf(out, n, "%d min", (int) (s / 60 + 0.5));
}

static const char *rec_title(const cJSON *r) {
    const char *t = jstr(r, "title");
    return *t ? t : (*jstr(r, "channelName") ? jstr(r, "channelName") : "?");
}

static void row_key(const row_t *r, char *out, size_t n) {
    if (!r) {
        out[0] = 0;
        return;
    }
    switch (r->type) {
    case R_FOLDER: snprintf(out, n, "f:%s", r->title); break;
    case R_TIMERS: snprintf(out, n, "f:\001timers"); break;
    case R_TIMER: snprintf(out, n, "t:%.0f", jnum(r->item, "timerId")); break;
    default: snprintf(out, n, "r:%s", jstr(r->item, "id")); break;
    }
}

/* ---------------- Building the rows ---------------- */

static void free_rows(void) {
    for (int i = 0; i < nrows; i++) free(rows[i].items);
    free(rows);
    rows = NULL;
    nrows = 0;
}

static row_t *add_row(int *cap) {
    if (nrows == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        row_t *n = realloc(rows, (size_t) *cap * sizeof(row_t));
        if (!n) return NULL;
        rows = n;
    }
    row_t *r = &rows[nrows++];
    memset(r, 0, sizeof(*r));
    return r;
}

static void ensure_visible(void) {
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + PAGE) scroll = sel - PAGE + 1;
    int max = nrows - PAGE > 0 ? nrows - PAGE : 0;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
}

static void rebuild(void) {
    char prev[300];
    row_key(sel < nrows ? &rows[sel] : NULL, prev, sizeof(prev));
    free_rows();
    int cap = 0;
    int nrec = cJSON_GetArraySize(recs), nt = cJSON_GetArraySize(timers);

    if (in_timers) {
        for (int i = 0; i < nt; i++) {
            row_t *r = add_row(&cap);
            if (!r) break;
            r->type = R_TIMER;
            r->item = cJSON_GetArrayItem(timers, i);
        }
    } else {
        /* groups by title, in the order of the first (= newest) recording */
        int in_folder_found = 0;
        if (!in_folder && nt) {
            row_t *r = add_row(&cap);
            if (r) {
                r->type = R_TIMERS;
                r->nitems = nt;
            }
        }
        char *used = calloc((size_t) (nrec ? nrec : 1), 1);
        for (int i = 0; used && i < nrec; i++) {
            if (used[i]) continue;
            const cJSON *a = cJSON_GetArrayItem(recs, i);
            const char *t = rec_title(a);
            int cnt = 0;
            for (int k = i; k < nrec; k++)
                if (!used[k] && !strcmp(rec_title(cJSON_GetArrayItem(recs, k)), t)) cnt++;
            const cJSON **items = calloc((size_t) cnt, sizeof(*items));
            int m = 0;
            for (int k = i; items && k < nrec; k++) {
                if (!used[k] && !strcmp(rec_title(cJSON_GetArrayItem(recs, k)), t)) {
                    used[k] = 1;
                    items[m++] = cJSON_GetArrayItem(recs, k);
                }
            }
            if (in_folder) {
                if (strcmp(t, folder) != 0) {
                    free(items);
                    continue;
                }
                in_folder_found = 1;
                for (int k = 0; k < m; k++) {
                    row_t *r = add_row(&cap);
                    if (!r) break;
                    r->type = R_REC;
                    r->item = items[k];
                }
                free(items);
                continue;
            }
            row_t *r = add_row(&cap);
            if (!r) {
                free(items);
                break;
            }
            if (m > 1) {
                r->type = R_FOLDER;
                snprintf(r->title, sizeof(r->title), "%s", t);
                r->items = items;
                r->nitems = m;
            } else {
                r->type = R_REC;
                r->item = items ? items[0] : a;
                free(items);
            }
        }
        free(used);
        if (in_folder && !in_folder_found) { /* folder has become empty */
            in_folder = 0;
            folder[0] = 0;
            rebuild();
            return;
        }
    }

    /* find the selection again: focus recording (also inside its folder), otherwise the previous row */
    int idx = -1;
    for (int i = 0; i < nrows && idx < 0; i++) {
        row_t *r = &rows[i];
        if (focus[0]) {
            if (r->type == R_REC && !strcmp(jstr(r->item, "id"), focus)) idx = i;
            for (int k = 0; r->type == R_FOLDER && k < r->nitems; k++)
                if (!strcmp(jstr(r->items[k], "id"), focus)) idx = i;
        } else {
            char key[300];
            row_key(r, key, sizeof(key));
            if (!strcmp(key, prev)) idx = i;
        }
    }
    focus[0] = 0;
    if (idx >= 0) sel = idx;
    if (sel > nrows - 1) sel = nrows - 1;
    if (sel < 0) sel = 0;
    ensure_visible();
}

/* ---------------- Building the objects ---------------- */

static lv_obj_t *lbl(lv_obj_t *p, int px, int bold, uint32_t color, lv_opa_t opa) {
    lv_obj_t *l = ui_label(p, px, bold, color);
    if (opa != LV_OPA_COVER) lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

void recbrowser_init(const recbrowser_actions *a) {
    act = *a;
    root = ui_box(lv_screen_active(), 0x000000, OPA(0.95), 0);
    lv_obj_set_size(root, 1920, 1080);

    /* header */
    lv_obj_t *t = lbl(root, 28, 1, UI_BLUE, LV_OPA_COVER);
    lv_label_set_text(t, _("Recordings"));
    lv_obj_set_pos(t, 40, (HDR_H - lv_font_get_line_height(ui_font(28, 1))) / 2);
    h_folder = lbl(root, 28, 0, UI_TEXT, LV_OPA_COVER);
    lv_obj_set_width(h_folder, 1300);
    ui_label_dots(h_folder);
    lv_obj_set_pos(h_folder, 230, (HDR_H - lv_font_get_line_height(ui_font(28, 0))) / 2);
    h_pos = lbl(root, 22, 0, UI_TEXT, OPA(0.5));
    lv_obj_align(h_pos, LV_ALIGN_TOP_RIGHT, -40, (HDR_H - lv_font_get_line_height(ui_font(22, 0))) / 2);
    lv_obj_t *l = ui_box(root, 0xFFFFFF, OPA(0.15), 0);
    lv_obj_set_pos(l, 0, HDR_H - 1);
    lv_obj_set_size(l, 1920, 1);

    /* list */
    for (int i = 0; i < PAGE; i++) {
        row_ui *u = &ui[i];
        u->obj = ui_box(root, UI_BLUE, LV_OPA_TRANSP, 0);
        lv_obj_set_pos(u->obj, 0, HDR_H + 12 + i * ROW_H);
        lv_obj_set_size(u->obj, LIST_W, ROW_H);
        lv_obj_set_style_outline_color(u->obj, lv_color_hex(UI_BLUE), 0);
        lv_obj_set_style_outline_pad(u->obj, -2, 0);
        lv_obj_t *line = ui_box(u->obj, 0xFFFFFF, OPA(0.04), 0);
        lv_obj_set_pos(line, 0, ROW_H - 1);
        lv_obj_set_size(line, LIST_W, 1);

        u->icon = lv_image_create(u->obj);
        lv_obj_set_size(u->icon, 90, 54);
        lv_obj_align(u->icon, LV_ALIGN_LEFT_MID, 40, 0);
        lv_image_set_inner_align(u->icon, LV_IMAGE_ALIGN_CONTAIN);
        u->badge = ui_box(u->obj, 0xFFFFFF, OPA(0.1), 6);
        lv_obj_set_size(u->badge, 90, 54);
        lv_obj_align(u->badge, LV_ALIGN_LEFT_MID, 40, 0);
        u->badge_l = lbl(u->badge, 26, 1, UI_TEXT, LV_OPA_COVER);
        lv_obj_center(u->badge_l);

        int tx = 40 + 90 + 22, tw = LIST_W - tx - 22 - 130 - 28;
        u->title = lbl(u->obj, 28, 0, UI_TEXT, LV_OPA_COVER);
        lv_obj_set_width(u->title, tw);
        ui_label_dots(u->title);
        lv_obj_set_pos(u->title, tx, 14);
        u->sub = lbl(u->obj, 20, 0, UI_TEXT, OPA(0.55));
        lv_obj_set_width(u->sub, tw);
        ui_label_dots(u->sub);
        lv_obj_set_pos(u->sub, tx, 14 + lv_font_get_line_height(ui_font(28, 0)) + 4);

        u->bar = lv_bar_create(u->obj);
        lv_obj_set_size(u->bar, 120, 6);
        lv_obj_align(u->bar, LV_ALIGN_RIGHT_MID, -28, 0);
        lv_bar_set_range(u->bar, 0, 1000);
        lv_obj_set_style_bg_color(u->bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(u->bar, OPA(0.15), LV_PART_MAIN);
        lv_obj_set_style_bg_color(u->bar, lv_color_hex(UI_BLUE), LV_PART_INDICATOR);
        lv_obj_set_style_radius(u->bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(u->bar, 0, LV_PART_INDICATOR);
        u->check = lv_label_create(u->obj);
        lv_obj_set_style_text_font(u->check, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(u->check, lv_color_hex(UI_GREEN), 0);
        lv_label_set_text(u->check, LV_SYMBOL_OK);
        lv_obj_align(u->check, LV_ALIGN_RIGHT_MID, -40, 0);
        u->live = ui_box(u->obj, UI_RED, LV_OPA_COVER, 3);
        lv_obj_set_size(u->live, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(u->live, 10, 0);
        lv_obj_set_style_pad_ver(u->live, 2, 0);
        lv_obj_t *lt = lbl(u->live, 18, 1, 0xFFFFFF, LV_OPA_COVER);
        lv_obj_set_style_text_letter_space(lt, 1, 0);
        lv_label_set_text(lt, "REC");
        lv_obj_align(u->live, LV_ALIGN_RIGHT_MID, -28, 0);
    }
    empty = lbl(root, 30, 0, UI_TEXT, OPA(0.5));
    lv_obj_set_pos(empty, 40, HDR_H + 60);

    /* details on the right */
    detail = ui_box(root, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(detail, LIST_W, HDR_H);
    lv_obj_set_size(detail, 1920 - LIST_W, 1080 - HDR_H - FOOT_H);
    lv_obj_set_style_border_side(detail, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(detail, 1, 0);
    lv_obj_set_style_border_color(detail, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(detail, OPA(0.1), 0);
    lv_obj_set_style_pad_hor(detail, 50, 0);
    lv_obj_set_style_pad_ver(detail, 40, 0);
    lv_obj_set_flex_flow(detail, LV_FLEX_FLOW_COLUMN);
    int dw = 1920 - LIST_W - 100;
    d_icon = lv_image_create(detail);
    lv_obj_set_size(d_icon, 160, 96);
    lv_image_set_inner_align(d_icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_style_margin_bottom(d_icon, 24, 0);
    d_title = lbl(detail, 36, 1, UI_TEXT, LV_OPA_COVER);
    lv_obj_set_width(d_title, dw);
    lv_obj_set_style_margin_bottom(d_title, 8, 0);
    d_sub = lbl(detail, 26, 0, UI_TEXT, OPA(0.8));
    lv_obj_set_width(d_sub, dw);
    lv_obj_set_style_margin_bottom(d_sub, 16, 0);
    d_meta = lbl(detail, 22, 0, UI_TEXT, OPA(0.6));
    lv_obj_set_width(d_meta, dw);
    lv_obj_set_style_margin_bottom(d_meta, 6, 0);
    d_meta2 = lbl(detail, 22, 0, UI_TEXT, OPA(0.6));
    lv_obj_set_width(d_meta2, dw);
    lv_obj_set_style_margin_bottom(d_meta2, 6, 0);
    d_tech = lbl(detail, 22, 0, UI_TEXT, OPA(0.4));
    lv_obj_set_width(d_tech, dw);
    d_status = lbl(detail, 22, 0, UI_BLUE, LV_OPA_COVER);
    lv_obj_set_width(d_status, dw);
    lv_obj_set_style_margin_top(d_status, 18, 0);
    d_desc = lbl(detail, 22, 0, UI_TEXT, OPA(0.75));
    lv_obj_set_width(d_desc, dw);
    lv_obj_set_style_margin_top(d_desc, 20, 0);
    lv_obj_set_style_text_line_space(d_desc, 11, 0);
    lv_obj_set_style_max_height(d_desc, 462, 0);
    lv_label_set_long_mode(d_desc, LV_LABEL_LONG_MODE_WRAP);

    /* footer */
    lv_obj_t *foot = ui_box(root, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(foot, 0, 1080 - FOOT_H);
    lv_obj_set_size(foot, 1920, FOOT_H);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(foot, OPA(0.1), 0);
    lv_obj_t *h = lbl(foot, 16, 0, UI_TEXT, OPA(0.55));
    lv_label_set_text(h, _("Up/down select        Left/right page        OK play / open        Back up / close"));
    lv_obj_center(h);

    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- Drawing ---------------- */

static void on_icon(void *user, const char *url, const lv_image_dsc_t *dsc) {
    row_ui *u = user;
    if (strcmp(url, u->icon_url) != 0) return;
    if (dsc) {
        lv_image_set_src(u->icon, dsc);
        lv_obj_remove_flag(u->icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_d_icon(void *user, const char *url, const lv_image_dsc_t *dsc) {
    (void) user;
    if (strcmp(url, d_icon_url) != 0) return;
    if (dsc) {
        lv_image_set_src(d_icon, dsc);
        lv_obj_remove_flag(d_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_icon(row_ui *u, const char *url) {
    if (!strcmp(url, u->icon_url) && *url) {
        lv_obj_remove_flag(u->icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    snprintf(u->icon_url, sizeof(u->icon_url), "%s", url);
    lv_obj_add_flag(u->icon, LV_OBJ_FLAG_HIDDEN);
    if (*url) ui_image_fetch(u->icon_url, on_icon, u);
}

static void hide(lv_obj_t *o) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
static void show(lv_obj_t *o) { lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); }

static void fill_row(int i) {
    row_ui *u = &ui[i];
    int r_i = scroll + i;
    if (r_i >= nrows) {
        hide(u->obj);
        return;
    }
    show(u->obj);
    row_t *r = &rows[r_i];
    int selected = r_i == sel;
    lv_obj_set_style_bg_opa(u->obj, selected ? OPA(0.25) : LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(u->obj, selected ? 2 : 0, 0);
    hide(u->bar);
    hide(u->check);
    hide(u->live);
    lv_obj_set_style_text_opa(u->title, LV_OPA_COVER, 0);
    char a[96], b[96], c[64], line[512];

    if (r->type == R_TIMERS) {
        hide(u->icon);
        u->icon_url[0] = 0;
        show(u->badge);
        lv_obj_set_style_bg_color(u->badge, lv_color_hex(UI_RED), 0);
        lv_obj_set_style_bg_opa(u->badge, OPA(0.35), 0);
        lv_label_set_text_fmt(u->badge_l, "%d", r->nitems);
        lv_label_set_text(u->title, _("Scheduled"));
        const cJSON *t = cJSON_GetArrayItem(timers, 0);
        rec_fmt_date((long) jnum(t, "start"), a, sizeof(a));
        snprintf(line, sizeof(line), _("next: %s \xC2\xB7 %s"), jstr(t, "title"), a);
        lv_label_set_text(u->sub, line);
        return;
    }
    if (r->type == R_FOLDER) {
        hide(u->icon);
        u->icon_url[0] = 0;
        show(u->badge);
        lv_obj_set_style_bg_color(u->badge, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(u->badge, OPA(0.1), 0);
        lv_label_set_text_fmt(u->badge_l, "%d", r->nitems);
        ui_oneline(line, sizeof(line), r->title, sizeof(line) - 1);
        lv_label_set_text(u->title, line);
        int unseen = 0;
        for (int k = 0; k < r->nitems; k++)
            if (!jtrue(r->items[k], "watched")) unseen++;
        rec_fmt_date((long) jnum(r->items[0], "start"), a, sizeof(a));
        if (unseen < r->nitems)
            snprintf(line, sizeof(line), _("%d recordings \xC2\xB7 %d unwatched \xC2\xB7 latest %s"), r->nitems, unseen, a);
        else snprintf(line, sizeof(line), _("%d recordings \xC2\xB7 latest %s"), r->nitems, a);
        lv_label_set_text(u->sub, line);
        return;
    }
    hide(u->badge);
    const cJSON *it = r->item;
    set_icon(u, jstr(it, "icon"));
    rec_fmt_date((long) jnum(it, "start"), a, sizeof(a));
    if (r->type == R_TIMER) {
        const char *st = jstr(it, "state");
        snprintf(line, sizeof(line), "%s%s%s", jstr(it, "title"), *jstr(it, "subtitle") ? " \xE2\x80\x94 " : "",
                 jstr(it, "subtitle"));
        char one[512];
        ui_oneline(one, sizeof(one), line, sizeof(one) - 1);
        lv_label_set_text(u->title, one);
        fmt_len(jnum(it, "stop") - jnum(it, "start"), c, sizeof(c));
        snprintf(line, sizeof(line), "%s \xC2\xB7 %s \xC2\xB7 %s", a, jstr(it, "channelName"), c);
        lv_label_set_text(u->sub, line);
        if (!strcmp(st, "recording")) show(u->live);
        return;
    }
    /* recording: inside a folder the subtitle as title */
    const char *title = in_folder && *jstr(it, "subtitle") ? jstr(it, "subtitle") : rec_title(it);
    ui_oneline(line, sizeof(line), title, sizeof(line) - 1);
    lv_label_set_text(u->title, line);
    fmt_len(jnum(it, "duration"), c, sizeof(c));
    snprintf(b, sizeof(b), "%s", jstr(it, "channelName"));
    snprintf(line, sizeof(line), "%s \xC2\xB7 %s \xC2\xB7 %s", a, b, c);
    lv_label_set_text(u->sub, line);
    int watched = jtrue(it, "watched");
    if (watched) lv_obj_set_style_text_opa(u->title, OPA(0.5), 0);
    double pos = jnum(it, "position"), dur = jnum(it, "duration");
    if (!strcmp(jstr(it, "state"), "recording")) show(u->live);
    else if (watched) show(u->check);
    else if (pos > 0 && dur > 0) {
        lv_bar_set_value(u->bar, (int32_t) (pos / dur * 1000 > 1000 ? 1000 : pos / dur * 1000), LV_ANIM_OFF);
        show(u->bar);
    }
}

static void set_text(lv_obj_t *l, const char *t) {
    if (t && *t) {
        lv_label_set_text(l, t);
        show(l);
    } else {
        hide(l);
    }
}

static void fill_detail(void) {
    row_t *r = sel < nrows ? &rows[sel] : NULL;
    char a[96], b[96], c[64], line[1024];
    hide(d_icon);
    d_icon_url[0] = 0;
    if (!r) {
        hide(detail);
        return;
    }
    show(detail);
    set_text(d_meta2, NULL);
    set_text(d_tech, NULL);
    set_text(d_desc, NULL);
    set_text(d_sub, NULL);
    if (r->type == R_TIMERS) {
        set_text(d_title, _("Scheduled recordings"));
        snprintf(line, sizeof(line), _("%d timers"), r->nitems);
        set_text(d_meta, line);
        set_text(d_status, _("OK: open"));
        return;
    }
    if (r->type == R_FOLDER) {
        set_text(d_title, r->title);
        /* channels of the recordings */
        line[0] = 0;
        size_t o = 0;
        for (int k = 0; k < r->nitems; k++) {
            const char *cn = jstr(r->items[k], "channelName");
            if (!*cn || strstr(line, cn)) continue;
            o += (size_t) snprintf(line + o, sizeof(line) - o, "%s%s", o ? ", " : "", cn);
            if (o >= sizeof(line)) break;
        }
        char m[1200];
        snprintf(m, sizeof(m), _("%d recordings \xC2\xB7 %s"), r->nitems, line);
        set_text(d_meta, m);
        rec_fmt_date((long) jnum(r->items[r->nitems - 1], "start"), a, sizeof(a));
        rec_fmt_date((long) jnum(r->items[0], "start"), b, sizeof(b));
        snprintf(line, sizeof(line), "%s \xE2\x80\x93 %s", a, b);
        set_text(d_meta2, line);
        set_text(d_status, _("OK: open"));
        return;
    }
    const cJSON *it = r->item;
    const char *icon = jstr(it, "icon");
    if (*icon) {
        snprintf(d_icon_url, sizeof(d_icon_url), "%s", icon);
        ui_image_fetch(d_icon_url, on_d_icon, NULL);
    }
    set_text(d_title, jstr(it, "title"));
    set_text(d_sub, jstr(it, "subtitle"));
    rec_fmt_date((long) jnum(it, "start"), a, sizeof(a));
    if (r->type == R_TIMER) {
        fmt_len(jnum(it, "stop") - jnum(it, "start"), c, sizeof(c));
        snprintf(line, sizeof(line), "%s \xC2\xB7 %s \xC2\xB7 %s", a, jstr(it, "channelName"), c);
        set_text(d_meta, line);
        int o = snprintf(line, sizeof(line), "%s",
                         !strcmp(jstr(it, "state"), "recording") ? _("Recording") : _("Scheduled"));
        if (has(it, "autorecId")) {
            if (*jstr(it, "seriesTitle"))
                snprintf(line + o, sizeof(line) - (size_t) o, _(" \xC2\xB7 series \xE2\x80\x9E%s\xE2\x80\x9C"),
                         jstr(it, "seriesTitle"));
            else snprintf(line + o, sizeof(line) - (size_t) o, "%s", _(" \xC2\xB7 series"));
        }
        set_text(d_status, line);
        set_text(d_desc, jstr(it, "description"));
        return;
    }
    fmt_len(jnum(it, "duration"), c, sizeof(c));
    snprintf(line, sizeof(line), "%s \xC2\xB7 %s \xC2\xB7 %s", a, jstr(it, "channelName"), c);
    set_text(d_meta, line);
    int o = 0;
    if (*jstr(it, "video")) o += snprintf(line + o, sizeof(line) - (size_t) o, "%s \xC2\xB7 ", jstr(it, "video"));
    if (*jstr(it, "audio")) o += snprintf(line + o, sizeof(line) - (size_t) o, "%s \xC2\xB7 ", jstr(it, "audio"));
    snprintf(line + o, sizeof(line) - (size_t) o, "%.1f GB", jnum(it, "size") / 1e9);
    set_text(d_tech, line);
    line[0] = 0;
    if (!strcmp(jstr(it, "state"), "recording")) snprintf(line, sizeof(line), "%s", _("Recording"));
    else if (jtrue(it, "watched")) snprintf(line, sizeof(line), "%s", _("Watched"));
    else if (jnum(it, "position") > 0) {
        rec_fmt_duration(jnum(it, "position"), b, sizeof(b));
        snprintf(line, sizeof(line), _("Resume at %s"), b);
    }
    if (*jstr(it, "error")) {
        size_t l = strlen(line);
        snprintf(line + l, sizeof(line) - l, "%s%s", l ? " \xC2\xB7 " : "", jstr(it, "error"));
    }
    set_text(d_status, line);
    set_text(d_desc, jstr(it, "description"));
}

static void render(void) {
    if (in_timers) lv_label_set_text_fmt(h_folder, "\xE2\x80\xBA   %s", _("Scheduled"));
    else if (in_folder) lv_label_set_text_fmt(h_folder, "\xE2\x80\xBA   %s", folder);
    else lv_label_set_text(h_folder, "");
    if (!have_recs || nrows == 0) {
        for (int i = 0; i < PAGE; i++) hide(ui[i].obj);
        lv_label_set_text(empty, !have_recs ? _("Loading \xE2\x80\xA6") : in_timers ? _("Nothing scheduled") : _("No recordings"));
        show(empty);
        hide(detail);
        lv_label_set_text(h_pos, "");
        return;
    }
    hide(empty);
    lv_label_set_text_fmt(h_pos, "%d / %d", sel + 1, nrows);
    for (int i = 0; i < PAGE; i++) fill_row(i);
    fill_detail();
}

/* ---------------- Interface ---------------- */

void recbrowser_open(const char *focus_id) {
    snprintf(focus, sizeof(focus), "%s", focus_id ? focus_id : "");
    if (!vis) {
        vis = 1;
        show(root);
        lv_obj_move_foreground(root);
    }
    if (have_recs) rebuild();
    render();
    if (act.request) act.request();
}

void recbrowser_close(void) {
    if (!vis) return;
    vis = 0;
    hide(root);
}

int recbrowser_visible(void) { return vis; }

void recbrowser_on_recordings(cJSON *list) {
    cJSON_Delete(recs);
    recs = cJSON_IsArray(list) ? list : (cJSON_Delete(list), cJSON_CreateArray());
    have_recs = 1;
    rebuild();
    if (vis) render();
}

void recbrowser_on_timers(cJSON *list) {
    cJSON_Delete(timers);
    timers = cJSON_IsArray(list) ? list : (cJSON_Delete(list), cJSON_CreateArray());
    if (!have_recs) return;
    rebuild();
    if (vis) render();
}

static void move(int d) {
    if (!nrows) return;
    int old_scroll = scroll, old = sel;
    sel += d;
    if (sel < 0) sel = 0;
    if (sel > nrows - 1) sel = nrows - 1;
    if (sel == old) return;
    ensure_visible();
    if (scroll != old_scroll) {
        render();
        return;
    }
    fill_row(old - scroll);
    fill_row(sel - scroll);
    lv_label_set_text_fmt(h_pos, "%d / %d", sel + 1, nrows);
    fill_detail();
}

static void on_option(void *user, int i) {
    (void) user;
    if (i < 0 || i >= n_opt || !opt_item) return;
    if (!act.play) return;
    if (!strcmp(opt_action[i], "resume")) act.play(jstr(opt_item, "id"), 0);
    else if (!strcmp(opt_action[i], "start")) act.play(jstr(opt_item, "id"), 1);
    else if (act.timer_action) act.timer_action(opt_action[i], opt_item, (int) jnum(opt_item, "channelNumber"));
}

static void select_row(void) {
    if (sel >= nrows) return;
    row_t *r = &rows[sel];
    const char *labels[4];
    char l0[96];
    if (r->type == R_TIMER) {
        const cJSON *t = r->item;
        n_opt = 0;
        labels[n_opt] = !strcmp(jstr(t, "state"), "recording") ? _("Stop recording") : _("Cancel recording");
        opt_action[n_opt++] = "cancel";
        if (has(t, "autorecId")) {
            labels[n_opt] = _("Delete series recording");
            opt_action[n_opt++] = "cancelSeries";
        } else if (has(t, "eventId")) {
            labels[n_opt] = _("Record series");
            opt_action[n_opt++] = "recordSeries";
        }
        opt_item = t;
        choice_open(jstr(t, "title"), labels, n_opt, -1, 0, on_option, NULL);
        return;
    }
    if (r->type == R_FOLDER || r->type == R_TIMERS) {
        row_key(r, root_key, sizeof(root_key));
        if (r->type == R_TIMERS) in_timers = 1;
        else {
            in_folder = 1;
            snprintf(folder, sizeof(folder), "%s", r->title);
        }
        sel = 0;
        scroll = 0;
        rebuild();
        sel = 0;
        scroll = 0;
        render();
        return;
    }
    const cJSON *it = r->item;
    double pos = jnum(it, "position");
    if (pos > 30 && !jtrue(it, "watched")) {
        char d[32];
        rec_fmt_duration(pos, d, sizeof(d));
        snprintf(l0, sizeof(l0), _("Resume at %s"), d);
        labels[0] = l0;
        labels[1] = _("From the beginning");
        opt_action[0] = "resume";
        opt_action[1] = "start";
        n_opt = 2;
        opt_item = it;
        choice_open(*jstr(it, "subtitle") ? jstr(it, "subtitle") : jstr(it, "title"), labels, 2, -1, 0, on_option,
                    NULL);
    } else {
        if (act.play) act.play(jstr(it, "id"), 0);
    }
}

static void back(void) {
    if (in_folder || in_timers) {
        in_folder = in_timers = 0;
        folder[0] = 0;
        rebuild();
        for (int i = 0; i < nrows; i++) {
            char k[300];
            row_key(&rows[i], k, sizeof(k));
            if (!strcmp(k, root_key)) sel = i;
        }
        ensure_visible();
        render();
        return;
    }
    recbrowser_close();
    if (act.closed) act.closed();
}

int recbrowser_pointer(int ev, int x, int y) {
    if (!vis) return 0;
    int hit = -1;
    for (int i = 0; i < PAGE; i++)
        if (scroll + i < nrows && ui_hit(ui[i].obj, x, y)) hit = scroll + i;
    if (hit >= 0 && hit != sel) move(hit - sel);
    if (ev == PTR_UP && hit >= 0) select_row();
    return 1;
}

int recbrowser_key(int key) {
    if (!vis) return 0;
    switch (key) {
    case KEY_UP: move(-1); break;
    case KEY_DOWN: move(1); break;
    case KEY_LEFT:
    case KEY_CH_UP: move(-PAGE); break;
    case KEY_RIGHT:
    case KEY_CH_DOWN: move(PAGE); break;
    case KEY_OK: select_row(); break;
    case KEY_BACK:
    case KEY_STOP: back(); break;
    default: break;
    }
    return 1;
}
