/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* strcasestr */
#include "recdlg.h"
#include "i18n.h"

#include "choice.h"
#include "keys.h"
#include "log.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define OPA(f) ((lv_opa_t) ((f) * 255 + 0.5))
#define W 1300
#define NROWS 12
#define ROW_H 64

typedef struct {
    lv_obj_t *obj, *time, *title;
    lv_span_t *sp[3]; /* marker, title, subtitle */
} rd_row;

static recdlg_actions act;
static lv_obj_t *box, *icon, *chname, *empty;
static rd_row rows[NROWS];
static cJSON *data;
static int vis, channel, sel, nb;
static double focus_id;
static char icon_url[512];

/* options of the open selection */
static const char *opt_action[4];
static int n_opt;

static const cJSON *bcast(int i) { return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(data, "broadcasts"), i); }

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : 0;
}

static int has(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return i && !cJSON_IsNull(i) && !(cJSON_IsString(i) && !*i->valuestring) && !cJSON_IsFalse(i);
}

void recdlg_init(const recdlg_actions *a) {
    act = *a;
    box = ui_box(lv_screen_active(), 0x141414, OPA(0.97), 8);
    lv_obj_set_size(box, W, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(box, OPA(0.15), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);

    /* header: logo, number and name */
    lv_obj_t *hdr = ui_box(box, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(hdr, lv_pct(100), 96);
    icon = lv_image_create(hdr);
    lv_obj_set_size(icon, 100, 56);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 32, 0);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    chname = ui_label(hdr, 30, 1, UI_TEXT);
    lv_obj_align(chname, LV_ALIGN_LEFT_MID, 152, 0);
    lv_obj_t *l1 = ui_box(box, 0xFFFFFF, OPA(0.1), 0);
    lv_obj_set_size(l1, lv_pct(100), 1);

    lv_obj_t *list = ui_box(box, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < NROWS; i++) {
        rd_row *r = &rows[i];
        r->obj = ui_box(list, UI_BLUE, LV_OPA_TRANSP, 0);
        lv_obj_set_size(r->obj, lv_pct(100), ROW_H);
        lv_obj_set_style_outline_color(r->obj, lv_color_hex(UI_BLUE), 0);
        lv_obj_set_style_outline_pad(r->obj, -2, 0);
        r->time = ui_label(r->obj, 24, 0, UI_TEXT);
        lv_obj_set_style_text_opa(r->time, OPA(0.6), 0);
        lv_obj_align(r->time, LV_ALIGN_LEFT_MID, 32, 0);
        r->title = lv_spangroup_create(r->obj);
        lv_obj_set_style_text_font(r->title, ui_font(26, 0), 0);
        lv_obj_set_style_text_color(r->title, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_size(r->title, W - 32 - 190 - 20 - 32, lv_font_get_line_height(ui_font(26, 0)));
        lv_spangroup_set_overflow(r->title, LV_SPAN_OVERFLOW_ELLIPSIS);
        lv_spangroup_set_max_lines(r->title, 1);
        for (int k = 0; k < 3; k++) r->sp[k] = lv_spangroup_add_span(r->title);
        lv_style_set_text_color(lv_span_get_style(r->sp[0]), lv_color_hex(UI_RED));
        lv_style_set_text_opa(lv_span_get_style(r->sp[2]), OPA(0.5));
        lv_obj_align(r->title, LV_ALIGN_LEFT_MID, 32 + 190 + 20, 0);
    }
    empty = ui_label(list, 26, 0, UI_TEXT);
    lv_obj_set_style_text_opa(empty, OPA(0.5), 0);
    lv_obj_set_style_pad_hor(empty, 32, 0);
    lv_obj_set_style_pad_ver(empty, 30, 0);

    lv_obj_t *foot = ui_box(box, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(foot, lv_pct(100), 50);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(foot, OPA(0.1), 0);
    lv_obj_t *h = ui_label(foot, 16, 0, UI_TEXT);
    lv_obj_set_style_text_opa(h, OPA(0.55), 0);
    lv_label_set_text(h, _("Up/down select        OK record / cancel        Back close"));
    lv_obj_center(h);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
}

static void on_icon(void *user, const char *url, const lv_image_dsc_t *dsc) {
    (void) user;
    if (strcmp(url, icon_url) != 0) return;
    if (dsc) {
        lv_image_set_src(icon, dsc);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fmt_date(long t, char *out, size_t n) {
    time_t tt = (time_t) t, now = time(NULL);
    struct tm tm, tn;
    localtime_r(&tt, &tm);
    localtime_r(&now, &tn);
    if (tm.tm_year != tn.tm_year)
        snprintf(out, n, "%02d.%02d.%04d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour,
                 tm.tm_min);
    else snprintf(out, n, "%02d.%02d. %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
}

static void style_sel(int i, int on) {
    if (i < 0 || i >= NROWS) return;
    lv_obj_set_style_bg_opa(rows[i].obj, on ? OPA(0.25) : LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(rows[i].obj, on ? 2 : 0, 0);
}

static void render(void) {
    if (!data) {
        lv_label_set_text(chname, _("Loading \xE2\x80\xA6"));
        for (int i = 0; i < NROWS; i++) lv_obj_add_flag(rows[i].obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text_fmt(chname, "%d  %s", (int) jnum(data, "channel"), jstr(data, "name"));
    for (int i = 0; i < NROWS; i++) {
        rd_row *r = &rows[i];
        const cJSON *b = i < nb ? bcast(i) : NULL;
        if (!b) {
            lv_obj_add_flag(r->obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(r->obj, LV_OBJ_FLAG_HIDDEN);
        char d[64], t[256], s[256];
        fmt_date((long) jnum(b, "start"), d, sizeof(d));
        lv_label_set_text(r->time, d);
        const char *rec = jstr(b, "rec");
        int series = has(b, "series") || has(b, "autorecId");
        lv_span_set_text(r->sp[0], *rec ? (series ? LV_SYMBOL_BULLET LV_SYMBOL_BULLET " " : LV_SYMBOL_BULLET " ") : "");
        ui_oneline(t, sizeof(t), jstr(b, "title"), sizeof(t) - 1);
        ui_oneline(s, sizeof(s), jstr(b, "subtitle"), sizeof(s) - 8);
        lv_span_set_text(r->sp[1], t);
        char s2[300];
        snprintf(s2, sizeof(s2), "%s%s", *s && strcmp(s, t) ? " \xE2\x80\x94 " : "", *s && strcmp(s, t) ? s : "");
        lv_span_set_text(r->sp[2], s2);
        lv_spangroup_refresh(r->title);
        style_sel(i, i == sel);
    }
    if (nb == 0) {
        lv_label_set_text(empty, _("No programme data"));
        lv_obj_remove_flag(empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(empty, LV_OBJ_FLAG_HIDDEN);
    }
}

void recdlg_open(int ch, double focus_event_id) {
    if (ch <= 0) return;
    vis = 1;
    channel = ch;
    focus_id = focus_event_id;
    cJSON_Delete(data);
    data = NULL;
    nb = 0;
    sel = 0;
    icon_url[0] = 0;
    lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    render();
    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(box);
    if (act.request) act.request(ch);
}

void recdlg_close(void) {
    vis = 0;
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
}

int recdlg_visible(void) { return vis; }

void recdlg_on_schedule(cJSON *msg) {
    if (!vis || (int) jnum(msg, "channel") != channel) {
        cJSON_Delete(msg);
        return;
    }
    /* keep the selection (after a timer action the list comes again) */
    double prev = data && sel < nb ? jnum(bcast(sel), "id") : 0;
    cJSON_Delete(data);
    data = msg;
    nb = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(data, "broadcasts"));
    if (nb > NROWS) nb = NROWS;
    double want = focus_id ? focus_id : prev;
    focus_id = 0;
    sel = 0;
    for (int i = 0; i < nb && want; i++)
        if (jnum(bcast(i), "id") == want) sel = i;
    const char *ic = jstr(data, "icon");
    if (strcmp(ic, icon_url) != 0) {
        snprintf(icon_url, sizeof(icon_url), "%s", ic);
        ui_image_fetch(icon_url, on_icon, NULL);
    }
    render();
}

static void on_option(void *user, int i) {
    (void) user;
    if (!data || sel >= nb || i < 0 || i >= n_opt) return;
    if (act.action) act.action(opt_action[i], bcast(sel), channel);
}

static void actions(void) {
    const cJSON *b = data && sel < nb ? bcast(sel) : NULL;
    if (!b) return;
    const char *labels[4];
    n_opt = 0;
    const char *rec = jstr(b, "rec");
    if (*rec) {
        labels[n_opt] = !strcmp(rec, "recording") ? _("Stop recording") : _("Cancel recording");
        opt_action[n_opt++] = "cancel";
    } else {
        labels[n_opt] = _("Record");
        opt_action[n_opt++] = "record";
    }
    if (has(b, "autorecId")) {
        labels[n_opt] = _("Delete series recording");
        opt_action[n_opt++] = "cancelSeries";
    } else if (jnum(b, "id")) {
        labels[n_opt] = _("Record series");
        opt_action[n_opt++] = "recordSeries";
    }
    char t[256];
    ui_oneline(t, sizeof(t), jstr(b, "title"), sizeof(t) - 1);
    choice_open(t, labels, n_opt, -1, 0, on_option, NULL);
}

int recdlg_pointer(int ev, int x, int y) {
    if (!vis) return 0;
    int hit = -1;
    for (int i = 0; i < nb && i < NROWS; i++)
        if (ui_hit(rows[i].obj, x, y)) hit = i;
    if (hit >= 0 && hit != sel) {
        style_sel(sel, 0);
        sel = hit;
        style_sel(sel, 1);
    }
    if (ev == PTR_UP && hit >= 0) actions();
    return 1;
}

int recdlg_key(int key) {
    if (!vis) return 0;
    switch (key) {
    case KEY_UP:
        if (sel > 0) {
            style_sel(sel, 0);
            style_sel(--sel, 1);
        }
        break;
    case KEY_DOWN:
        if (sel < nb - 1) {
            style_sel(sel, 0);
            style_sel(++sel, 1);
        }
        break;
    case KEY_OK: actions(); break;
    case KEY_BACK:
    case KEY_INFO: recdlg_close(); break;
    default: break;
    }
    return 1;
}

int recdlg_result_text(const char *action, int ok, const char *err, char *out, size_t n) {
    const char *a = action ? action : "";
    if (ok) {
        const char *t = !strcmp(a, "record")         ? _("Recording scheduled")
                        : !strcmp(a, "cancel")       ? _("Recording cancelled")
                        : !strcmp(a, "recordSeries") ? _("Series recording scheduled")
                        : !strcmp(a, "cancelSeries") ? _("Series recording deleted")
                                                     : _("Done");
        snprintf(out, n, "%s", t);
        return 0;
    }
    const char *e = err ? err : "";
    if (strcasestr(e, "access"))
        snprintf(out, n, "%s", _("Not allowed \xE2\x80\x93 created by another user"));
    else snprintf(out, n, _("Failed: %s"), e);
    return 1;
}
