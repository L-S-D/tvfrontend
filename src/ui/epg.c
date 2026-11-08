/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Channel list/EPG - layout and behaviour as epg in TvhWebOsClient/js/app.js:
 * 10 rows per page, Up/Down selects (at the page edge the page jumps),
 * Left/Right pages, CH+/- jumps to the next/previous group,
 * Red/Green shift the time window by 2 h, Yellow switches the view,
 * Blue opens the group selection, OK switches channel.
 *
 * The rows are a fixed set of LVGL objects that are refilled when
 * paging; only the programme cells of the grid are created anew per page.
 */
#include "epg.h"
#include "i18n.h"

#include "keys.h"
#include "log.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCR_W 1920
#define ROWS 10 /* pageSize */
#define HDR_H 50
#define FOOT_H 50
#define ROW_H 96
#define ROW_PITCH 97 /* row + 1 px line */
#define INFO_W 420
#define PROG_W (SCR_W - INFO_W)
#define WIN_DUR 10800
#define SHIFT 7200
#define MARKS (WIN_DUR / 1800)
#define NN_TIME_W 170
#define NN_BAR_W 120
#define PLOT_MAX 400

#define OPA(f) ((lv_opa_t) ((f) * 255 + 0.5))

/* ---------------- Data ---------------- */

typedef struct {
    char id[64];
    char label[128];
    int start;
} grp;

static epg_actions act;
static cJSON *g_msg;
static const cJSON **g_ch; /* row -> channel object {id,name,icon,broadcasts} */
static int g_nch;
static grp *g_grp;
static int g_ngrp;
static int g_cur_ch;

/* ---------------- State ---------------- */

static int vis, sel, scroll, view_grid, keep_sel, popup, popup_idx;
static long win_start;
static char last_group[64];
static uint32_t last_tick;

/* ---------------- Objects ---------------- */

#define NSPAN 6 /* marker, title, -, subtitle, -, synopsis */

typedef struct {
    lv_obj_t *obj, *glabel, *icon, *num, *name;
    lv_obj_t *nn, *now_time, *now_title, *now_bar, *next_time, *next_title;
    lv_span_t *now_sp[NSPAN], *next_sp[NSPAN];
    lv_obj_t *grid;
    char icon_url[512];
    double now_id, next_id; /* programmes of the Now/Next lines (pointer) */
} row_ui;

static lv_obj_t *root, *hdr_group, *ruler, *ruler_lbl[MARKS], *now_line, *view_hint, *body, *body_msg;
static lv_obj_t *pop, *pop_list;
static row_ui rows[ROWS];

/* ---------------- JSON ---------------- */

static const cJSON *jget(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }

static double jnum(const cJSON *o, const char *k) {
    const cJSON *i = jget(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : 0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = jget(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

/* group IDs are numbers (tvh) or text ("other") */
static void jid(const cJSON *o, const char *k, char *out, size_t n) {
    const cJSON *i = jget(o, k);
    if (cJSON_IsString(i) && i->valuestring) snprintf(out, n, "%s", i->valuestring);
    else if (cJSON_IsNumber(i)) snprintf(out, n, "%.0f", i->valuedouble);
    else out[0] = 0;
}

static int ch_id(int row) { return (int) jnum(g_ch[row], "id"); }

/* ---------------- Groups and rows ---------------- */

static int group_at(int row) {
    int g = 0;
    for (int i = 1; i < g_ngrp; i++)
        if (g_grp[i].start <= row) g = i;
    return g;
}

static int group_starting_at(int row) {
    for (int i = 0; i < g_ngrp; i++)
        if (g_grp[i].start == row) return i;
    return -1;
}

/* row of a channel, preferably in the group group ("" = any) */
static int find_row(int id, const char *group) {
    int first = -1;
    for (int i = 0; i < g_nch; i++) {
        if (ch_id(i) != id) continue;
        if (first < 0) first = i;
        if (!group || !*group || (g_ngrp && !strcmp(g_grp[group_at(i)].id, group))) return i;
    }
    return first;
}

static void clamp_scroll(void) {
    int max = g_nch - ROWS;
    if (max < 0) max = 0;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
    if (sel > g_nch - 1) sel = g_nch - 1;
    if (sel < 0) sel = 0;
}

static void free_data(void) {
    cJSON_Delete(g_msg);
    g_msg = NULL;
    free(g_ch);
    g_ch = NULL;
    g_nch = 0;
    free(g_grp);
    g_grp = NULL;
    g_ngrp = 0;
}

/* upper case for the group line (ASCII and umlauts) */
static void upper(char *out, size_t n, const char *in) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *) in; *p && o + 2 < n; p++) {
        if (*p >= 'a' && *p <= 'z') {
            out[o++] = (char) (*p - 32);
        } else if (*p == 0xC3 && (p[1] == 0xA4 || p[1] == 0xB6 || p[1] == 0xBC)) {
            out[o++] = (char) 0xC3;
            out[o++] = (char) (p[1] - 0x20);
            p++;
        } else {
            out[o++] = (char) *p;
        }
    }
    out[o] = 0;
}

static const char *group_label(const char *l) {
    /* tvsrc names the catch-all groups in English */
    if (!strcmp(l, "Other channels")) return _("Other channels");
    if (!strcmp(l, "All channels")) return _("All channels");
    return l;
}

/* ---------------- Text ---------------- */

static void span_style(lv_span_t *sp, uint32_t color, lv_opa_t opa) {
    lv_style_t *st = lv_span_get_style(sp);
    lv_style_set_text_color(st, lv_color_hex(color));
    lv_style_set_text_opa(st, opa);
}

/* single-line span group: marker, title - subtitle - synopsis */
static lv_obj_t *desc_create(lv_obj_t *parent, int px, int w, lv_span_t **sp) {
    lv_obj_t *g = lv_spangroup_create(parent);
    lv_obj_set_style_text_font(g, ui_font(px, 0), 0);
    lv_obj_set_style_text_color(g, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_size(g, w, lv_font_get_line_height(ui_font(px, 0)));
    lv_spangroup_set_overflow(g, LV_SPAN_OVERFLOW_ELLIPSIS);
    lv_spangroup_set_max_lines(g, 1);
    for (int i = 0; i < NSPAN; i++) sp[i] = lv_spangroup_add_span(g);
    span_style(sp[0], UI_RED, LV_OPA_COVER);
    span_style(sp[1], UI_TEXT, LV_OPA_COVER);
    span_style(sp[2], UI_TEXT, OPA(0.4));
    span_style(sp[3], UI_TEXT, LV_OPA_COVER);
    span_style(sp[4], UI_TEXT, OPA(0.4));
    span_style(sp[5], UI_TEXT, OPA(0.6));
    return g;
}

static void desc_set(lv_obj_t *g, lv_span_t **sp, const cJSON *b, const char *empty) {
    static char sub[512], plot[PLOT_MAX + 8], title[512];
    if (!b) {
        lv_span_set_text(sp[0], "");
        lv_span_set_text(sp[1], empty);
        for (int i = 2; i < NSPAN; i++) lv_span_set_text(sp[i], "");
        lv_spangroup_refresh(g);
        return;
    }
    const char *rec = jstr(b, "rec");
    int series = cJSON_IsTrue(jget(b, "series"));
    lv_span_set_text(sp[0], *rec ? (series ? LV_SYMBOL_BULLET LV_SYMBOL_BULLET " " : LV_SYMBOL_BULLET " ") : "");
    ui_oneline(title, sizeof(title), jstr(b, "title"), sizeof(title) - 1);
    ui_oneline(sub, sizeof(sub), jstr(b, "subtitle"), sizeof(sub) - 1);
    ui_oneline(plot, sizeof(plot), jstr(b, "plot"), PLOT_MAX);
    /* leave out repetitions (subtitle = title, synopsis = subtitle) */
    if (!strcmp(sub, title)) sub[0] = 0;
    if (!strcmp(plot, title) || (*sub && !strcmp(plot, sub))) plot[0] = 0;
    lv_span_set_text(sp[1], title);
    lv_span_set_text(sp[2], *sub ? " \xE2\x80\x94 " : "");
    lv_span_set_text(sp[3], sub);
    lv_span_set_text(sp[4], *plot ? " \xE2\x80\x94 " : "");
    lv_span_set_text(sp[5], plot);
    lv_spangroup_refresh(g);
}

/* ---------------- Construction ---------------- */

static lv_obj_t *line_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, lv_opa_t opa) {
    lv_obj_t *o = ui_box(parent, color, opa, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static int line_h(int px) { return lv_font_get_line_height(ui_font(px, 0)); }

static void build_row(row_ui *u, int i) {
    u->obj = ui_box(body, UI_BLUE, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(u->obj, 0, i * ROW_PITCH);
    lv_obj_set_size(u->obj, SCR_W, ROW_H);
    lv_obj_set_style_border_color(u->obj, lv_color_hex(UI_BLUE), 0);
    lv_obj_set_style_border_side(u->obj, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_outline_color(u->obj, lv_color_hex(UI_BLUE), 0);
    lv_obj_set_style_outline_pad(u->obj, -2, 0);
    line_box(body, 0, i * ROW_PITCH + ROW_H, SCR_W, 1, 0xFFFFFF, OPA(0.04));

    /* channel: logo, number, name */
    u->icon = lv_image_create(u->obj);
    lv_obj_set_pos(u->icon, 12, (ROW_H - 40) / 2);
    lv_obj_set_size(u->icon, 60, 40);
    lv_image_set_inner_align(u->icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(u->icon, LV_OBJ_FLAG_HIDDEN);

    u->num = ui_label(u->obj, 26, 0, UI_TEXT);
    lv_obj_set_style_text_opa(u->num, OPA(0.5), 0);
    lv_obj_set_width(u->num, 64);
    lv_obj_set_style_text_align(u->num, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(u->num, LV_ALIGN_LEFT_MID, 82, 0);

    u->name = ui_label(u->obj, 28, 0, UI_TEXT);
    lv_obj_set_width(u->name, INFO_W - 12 - 156);
    ui_label_dots(u->name);
    lv_obj_align(u->name, LV_ALIGN_LEFT_MID, 156, 0);

    u->glabel = ui_label(u->obj, 15, 1, UI_BLUE);
    lv_obj_set_style_text_letter_space(u->glabel, 1, 0);
    lv_obj_set_pos(u->glabel, 12, 4);

    /* Now/Next */
    u->nn = ui_box(u->obj, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(u->nn, INFO_W, 0);
    lv_obj_set_size(u->nn, PROG_W, ROW_H);
    line_box(u->nn, 0, 0, 1, ROW_H, 0xFFFFFF, OPA(0.06));
    int x_title = 16 + NN_TIME_W + 12;

    u->now_time = ui_label(u->nn, 26, 0, UI_TEXT);
    lv_obj_set_style_text_opa(u->now_time, OPA(0.6), 0);
    lv_obj_set_pos(u->now_time, 16, 8 + (40 - line_h(26)) / 2);
    u->now_title = desc_create(u->nn, 28, PROG_W - 16 - x_title - NN_BAR_W - 12, u->now_sp);
    lv_obj_set_pos(u->now_title, x_title, 8 + (40 - line_h(28)) / 2);
    u->now_bar = lv_bar_create(u->nn);
    lv_obj_set_pos(u->now_bar, PROG_W - 16 - NN_BAR_W, 8 + 18);
    lv_obj_set_size(u->now_bar, NN_BAR_W, 4);
    lv_bar_set_range(u->now_bar, 0, 1000);
    lv_obj_set_style_bg_color(u->now_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(u->now_bar, OPA(0.1), LV_PART_MAIN);
    lv_obj_set_style_bg_color(u->now_bar, lv_color_hex(UI_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(u->now_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(u->now_bar, 2, LV_PART_INDICATOR);

    u->next_time = ui_label(u->nn, 23, 0, UI_TEXT);
    lv_obj_set_style_text_opa(u->next_time, OPA(0.3), 0);
    lv_obj_set_pos(u->next_time, 16, 48 + (40 - line_h(23)) / 2);
    u->next_title = desc_create(u->nn, 25, PROG_W - 16 - x_title, u->next_sp);
    lv_obj_set_style_opa(u->next_title, OPA(0.5), 0);
    lv_obj_set_pos(u->next_title, x_title, 48 + (40 - line_h(25)) / 2);

    /* grid */
    u->grid = ui_box(u->obj, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(u->grid, INFO_W, 0);
    lv_obj_set_size(u->grid, PROG_W, ROW_H);
    lv_obj_add_flag(u->grid, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *hint(lv_obj_t *parent, const char *text, uint32_t color) {
    lv_obj_t *l = ui_label(parent, 16, 0, color);
    lv_label_set_text(l, text);
    return l;
}

/* hint with symbols: the symbols directly from Montserrat (Lato does not have
 * them, the detour via the fallback font costs a lookup per character
 * and an LVGL error in the log) */
static void hint_sym(lv_obj_t *parent, const char *sym, const char *text) {
    lv_obj_t *box = ui_box(parent, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 6, 0);
    lv_obj_t *s = lv_label_create(box);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s, lv_color_hex(UI_TEXT), 0);
    lv_label_set_text(s, sym);
    hint(box, text, UI_TEXT);
}

void epg_init(const epg_actions *a) {
    act = *a;
    lv_obj_t *scr = lv_screen_active();
    root = ui_box(scr, 0x000000, OPA(0.95), 0);
    lv_obj_set_size(root, SCR_W, 1080);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

    /* header: group, timeline */
    line_box(root, 0, HDR_H - 1, SCR_W, 1, 0xFFFFFF, OPA(0.15));
    hdr_group = ui_label(root, 22, 1, UI_BLUE);
    lv_obj_set_width(hdr_group, INFO_W - 32);
    ui_label_dots(hdr_group);
    lv_obj_set_pos(hdr_group, 16, (HDR_H - line_h(22)) / 2);
    ruler = ui_box(root, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(ruler, INFO_W, 0);
    lv_obj_set_size(ruler, PROG_W, HDR_H - 1);
    for (int i = 0; i < MARKS; i++) {
        int x = i * PROG_W / MARKS;
        line_box(ruler, x, 0, 1, HDR_H - 1, 0xFFFFFF, OPA(0.1));
        ruler_lbl[i] = ui_label(ruler, 22, 0, UI_TEXT);
        lv_obj_set_style_text_opa(ruler_lbl[i], OPA(0.5), 0);
        lv_obj_set_pos(ruler_lbl[i], x + 10, (HDR_H - line_h(22)) / 2);
    }

    /* rows */
    body = ui_box(root, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(body, 0, HDR_H);
    lv_obj_set_size(body, SCR_W, 1080 - HDR_H - FOOT_H);
    for (int i = 0; i < ROWS; i++) build_row(&rows[i], i);
    body_msg = ui_label(body, 30, 0, UI_DIM);
    lv_obj_center(body_msg);

    /* now line (grid only) */
    now_line = line_box(root, INFO_W, HDR_H, 2, 1080 - HDR_H - FOOT_H, UI_RED, LV_OPA_COVER);
    lv_obj_add_flag(now_line, LV_OBJ_FLAG_HIDDEN);

    /* footer: key hints */
    lv_obj_t *foot = ui_box(root, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(foot, 0, 1080 - FOOT_H);
    lv_obj_set_size(foot, SCR_W, FOOT_H);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(foot, OPA(0.1), 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(foot, 32, 0);
    lv_obj_set_style_opa(foot, OPA(0.55), 0);
    hint(foot, "-2h", UI_RED);
    hint(foot, "+2h", UI_GREEN);
    view_hint = hint(foot, "", UI_YELLOW);
    hint(foot, _("Groups"), UI_HINT_BLUE);
    hint_sym(foot, LV_SYMBOL_UP LV_SYMBOL_DOWN, _("Select"));
    hint_sym(foot, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, _("Page"));
    hint(foot, _("CH+/- group"), UI_TEXT);
    hint(foot, _("OK switch"), UI_TEXT);
    hint(foot, _("INFO/REC record"), UI_TEXT);

    /* group selection */
    pop = ui_box(root, 0x141414, OPA(0.97), 8);
    lv_obj_set_width(pop, 760);
    lv_obj_set_height(pop, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(pop, 918, 0);
    lv_obj_set_style_border_width(pop, 1, 0);
    lv_obj_set_style_border_color(pop, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(pop, OPA(0.15), 0);
    lv_obj_set_style_pad_ver(pop, 24, 0);
    lv_obj_set_flex_flow(pop, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(pop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pop, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(pop, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *pt = ui_label(pop, 24, 1, UI_TEXT);
    lv_label_set_text(pt, _("Channel groups"));
    lv_obj_set_style_text_opa(pt, OPA(0.6), 0);
    lv_obj_set_style_pad_hor(pt, 32, 0);
    lv_obj_set_style_pad_bottom(pt, 16, 0);
    lv_obj_t *pl = ui_box(pop, 0xFFFFFF, OPA(0.1), 0);
    lv_obj_set_size(pl, lv_pct(100), 1);
    lv_obj_set_style_margin_bottom(pl, 8, 0);
    pop_list = ui_box(pop, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(pop_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pop_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- Drawing ---------------- */

static void on_icon(void *user, const char *url, const lv_image_dsc_t *dsc) {
    row_ui *u = user;
    if (strcmp(url, u->icon_url) != 0) return; /* a different row by now */
    if (dsc) {
        lv_image_set_src(u->icon, dsc);
        lv_obj_remove_flag(u->icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(u->icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fmt_range(long a, long b, char *out, size_t n) {
    char x[8], y[8];
    ui_fmt_hm(a, x, sizeof(x));
    ui_fmt_hm(b, y, sizeof(y));
    snprintf(out, n, "%s - %s", x, y);
}

static void fill_nownext(row_ui *u, const cJSON *ch, long now) {
    const cJSON *nowb = NULL, *nextb = NULL, *b;
    cJSON_ArrayForEach(b, jget(ch, "broadcasts")) {
        long s = (long) jnum(b, "start"), e = (long) jnum(b, "stop");
        if (now >= s && now < e) nowb = b;
        else if (s > now && !nextb) nextb = b;
    }
    char r[32];
    u->now_id = nowb ? jnum(nowb, "id") : 0;
    u->next_id = nextb ? jnum(nextb, "id") : 0;
    if (nowb) {
        long s = (long) jnum(nowb, "start"), e = (long) jnum(nowb, "stop");
        fmt_range(s, e, r, sizeof(r));
        lv_label_set_text(u->now_time, r);
        lv_bar_set_value(u->now_bar, e > s ? (int32_t) ((now - s) * 1000 / (e - s)) : 0, LV_ANIM_OFF);
        lv_obj_remove_flag(u->now_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(u->now_time, "");
        lv_obj_add_flag(u->now_bar, LV_OBJ_FLAG_HIDDEN);
    }
    desc_set(u->now_title, u->now_sp, nowb, _("No data"));
    lv_obj_set_style_opa(u->now_title, nowb ? LV_OPA_COVER : OPA(0.3), 0);
    if (nextb) {
        fmt_range((long) jnum(nextb, "start"), (long) jnum(nextb, "stop"), r, sizeof(r));
        lv_label_set_text(u->next_time, r);
    } else {
        lv_label_set_text(u->next_time, "");
    }
    desc_set(u->next_title, u->next_sp, nextb, _("No data"));
    lv_obj_set_style_opa(u->next_title, nextb ? OPA(0.5) : OPA(0.15), 0);
}

static void fill_grid(row_ui *u, const cJSON *ch, long now, int selected) {
    lv_obj_clean(u->grid);
    line_box(u->grid, 0, 0, 1, ROW_H, 0xFFFFFF, OPA(0.06));
    const cJSON *b;
    cJSON_ArrayForEach(b, jget(ch, "broadcasts")) {
        long s = (long) jnum(b, "start"), e = (long) jnum(b, "stop");
        long bs = s > win_start ? s : win_start, be = e < win_start + WIN_DUR ? e : win_start + WIN_DUR;
        int x = (int) ((bs - win_start) * PROG_W / WIN_DUR);
        int w = (int) ((be - win_start) * PROG_W / WIN_DUR) - x;
        if (w * 1000 < PROG_W * 3) continue; /* < 0.3 % */
        int active = now >= s && now < e;

        lv_obj_t *c = ui_box(u->grid, active ? UI_BLUE : 0xFFFFFF,
                             active ? (selected ? OPA(0.25) : OPA(0.12)) : OPA(0.06), 3);
        lv_obj_set_user_data(c, (void *) (intptr_t) (long) jnum(b, "id")); /* programme for the pointer */
        lv_obj_set_pos(c, x, 2);
        lv_obj_set_size(c, w, ROW_H - 4);
        lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(active ? UI_BLUE : 0xFFFFFF), 0);
        lv_obj_set_style_border_opa(c, active ? LV_OPA_COVER : OPA(0.12), 0);
        lv_obj_set_style_pad_hor(c, 8, 0);
        lv_obj_set_style_pad_ver(c, 4, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        int inner = w - 16 - 2;
        if (inner < 4) inner = 4;
        lv_span_t *sp[NSPAN];
        lv_obj_t *t = desc_create(c, 24, inner, sp);
        desc_set(t, sp, b, "");
        lv_obj_t *tm = ui_label(c, 18, 0, UI_TEXT);
        lv_obj_set_style_text_opa(tm, OPA(0.45), 0);
        lv_obj_set_width(tm, inner);
        lv_label_set_long_mode(tm, LV_LABEL_LONG_MODE_CLIP);
        char hm[8];
        ui_fmt_hm(s, hm, sizeof(hm));
        lv_label_set_text(tm, hm);
    }
}

static void style_selected(row_ui *u, int selected) {
    lv_obj_set_style_bg_opa(u->obj, selected ? OPA(0.25) : LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(u->obj, selected ? 2 : 0, 0);
}

static void fill_row(int i) {
    row_ui *u = &rows[i];
    int r = scroll + i;
    if (r >= g_nch) {
        lv_obj_add_flag(u->obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(u->obj, LV_OBJ_FLAG_HIDDEN);
    const cJSON *ch = g_ch[r];
    long now = (long) time(NULL);
    int selected = r == sel;

    int g = group_starting_at(r);
    if (g >= 0) {
        char up[160];
        upper(up, sizeof(up), group_label(g_grp[g].label));
        lv_label_set_text(u->glabel, up);
        lv_obj_remove_flag(u->glabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_width(u->obj, 2, 0);
        lv_obj_set_style_border_opa(u->obj, OPA(0.6), 0);
    } else {
        lv_obj_add_flag(u->glabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_width(u->obj, 0, 0);
    }
    style_selected(u, selected);

    const char *icon = jstr(ch, "icon");
    if (strcmp(icon, u->icon_url) != 0) {
        snprintf(u->icon_url, sizeof(u->icon_url), "%s", icon);
        lv_obj_add_flag(u->icon, LV_OBJ_FLAG_HIDDEN);
        ui_image_fetch(u->icon_url, on_icon, u);
    }
    int id = (int) jnum(ch, "id");
    lv_label_set_text_fmt(u->num, "%d", id);
    lv_label_set_text(u->name, jstr(ch, "name"));
    lv_obj_set_style_text_color(u->name, lv_color_hex(id == g_cur_ch ? UI_BLUE : UI_TEXT), 0);

    if (view_grid) {
        lv_obj_add_flag(u->nn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(u->grid, LV_OBJ_FLAG_HIDDEN);
        fill_grid(u, ch, now, selected);
    } else {
        lv_obj_add_flag(u->grid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(u->nn, LV_OBJ_FLAG_HIDDEN);
        fill_nownext(u, ch, now);
    }
}

static void update_header(void) {
    const char *gl = "";
    if (g_ngrp) gl = group_label(g_grp[group_at(sel)].label);
    else if (g_msg) gl = group_label(jstr(g_msg, "groupLabel"));
    lv_label_set_text(hdr_group, gl);
    lv_label_set_text(view_hint, view_grid ? _("Grid") : _("Now/next"));
}

static void update_now_line(void) {
    long now = (long) time(NULL);
    if (!view_grid || now < win_start || now > win_start + WIN_DUR) {
        lv_obj_add_flag(now_line, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_x(now_line, INFO_W + (int) ((now - win_start) * PROG_W / WIN_DUR));
    lv_obj_remove_flag(now_line, LV_OBJ_FLAG_HIDDEN);
}

static void render(void) {
    if (!g_msg) {
        for (int i = 0; i < ROWS; i++) lv_obj_add_flag(rows[i].obj, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(body_msg, _("Loading programme data \xE2\x80\xA6"));
        lv_obj_center(body_msg);
        lv_obj_remove_flag(body_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(body_msg, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < ROWS; i++) fill_row(i);
    }
    if (view_grid) {
        lv_obj_remove_flag(ruler, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < MARKS; i++) {
            char hm[8];
            ui_fmt_hm(win_start + i * 1800L, hm, sizeof(hm));
            lv_label_set_text(ruler_lbl[i], hm);
        }
    } else {
        lv_obj_add_flag(ruler, LV_OBJ_FLAG_HIDDEN);
    }
    update_now_line();
    update_header();
}

/* only the selection changes: redo two rows */
static void update_selection(int old) {
    for (int i = 0; i < ROWS; i++) {
        int r = scroll + i;
        if (r != old && r != sel) continue;
        if (view_grid) fill_row(i); /* active programme becomes brighter */
        else style_selected(&rows[i], r == sel);
    }
    update_header();
}

/* ---------------- Group selection ---------------- */

static void popup_render(void) {
    lv_obj_clean(pop_list);
    int cur = group_at(sel);
    for (int i = 0; i < g_ngrp; i++) {
        lv_obj_t *it = ui_box(pop_list, UI_BLUE, i == popup_idx ? OPA(0.25) : LV_OPA_TRANSP, 0);
        lv_obj_set_size(it, lv_pct(50), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(it, 32, 0);
        lv_obj_set_style_pad_ver(it, 12, 0);
        lv_obj_t *l = ui_label(it, 26, 0, i == cur ? UI_BLUE : UI_TEXT);
        lv_obj_set_width(l, lv_pct(100));
        ui_label_dots(l);
        lv_label_set_text(l, group_label(g_grp[i].label));
    }
    lv_obj_update_layout(pop);
    lv_obj_t *it = lv_obj_get_child(pop_list, popup_idx);
    if (it) lv_obj_scroll_to_view(it, LV_ANIM_OFF);
}

static void popup_move(int d) {
    int n = popup_idx + d;
    if (n < 0 || n >= g_ngrp) return;
    lv_obj_t *o = lv_obj_get_child(pop_list, popup_idx);
    if (o) lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    popup_idx = n;
    o = lv_obj_get_child(pop_list, popup_idx);
    if (o) {
        lv_obj_set_style_bg_opa(o, OPA(0.25), 0);
        lv_obj_scroll_to_view(o, LV_ANIM_OFF);
    }
}

static void popup_show(void) {
    if (!g_ngrp) return;
    popup = 1;
    popup_idx = group_at(sel);
    lv_obj_remove_flag(pop, LV_OBJ_FLAG_HIDDEN);
    popup_render();
}

static void popup_hide(void) {
    popup = 0;
    lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- Navigation ---------------- */

static void to_group(int g) {
    if (!g_msg || g < 0 || g >= g_ngrp) return;
    sel = g_grp[g].start;
    scroll = sel;
    clamp_scroll();
    render();
}

/* at the page edge the page jumps; the selection moves only one row on
 * (the HTML5 app puts it at the page edge - with pages that do not start at
 * multiples of 10 it jumped back because of that) */
static void move_up(void) {
    if (sel <= 0) return;
    int old = sel--;
    if (sel < scroll) {
        scroll = scroll - ROWS > 0 ? scroll - ROWS : 0;
        render();
    } else {
        update_selection(old);
    }
}

static void move_down(void) {
    if (!g_msg || sel >= g_nch - 1) return;
    int old = sel++;
    if (sel >= scroll + ROWS) {
        int smax = g_nch - ROWS > 0 ? g_nch - ROWS : 0;
        scroll = scroll + ROWS < smax ? scroll + ROWS : smax;
        render();
    } else {
        update_selection(old);
    }
}

static void page_up(void) {
    sel = sel - ROWS > 0 ? sel - ROWS : 0;
    scroll = scroll - ROWS > 0 ? scroll - ROWS : 0;
    render();
}

static void page_down(void) {
    if (!g_msg) return;
    int max = g_nch - 1;
    sel = sel + ROWS < max ? sel + ROWS : max;
    int smax = g_nch - ROWS > 0 ? g_nch - ROWS : 0;
    scroll = scroll + ROWS < smax ? scroll + ROWS : smax;
    render();
}

static void select_channel(void) {
    if (!g_msg || sel >= g_nch) return;
    int id = ch_id(sel);
    snprintf(last_group, sizeof(last_group), "%s", g_ngrp ? g_grp[group_at(sel)].id : "");
    mt_log("[epg] channel %d (group %s)", id, last_group);
    epg_close();
    if (act.tune) act.tune(id);
}

static void shift_time(long d) {
    win_start += d;
    keep_sel = 1;
    if (act.request) act.request(win_start, WIN_DUR);
}

/* ---------------- Interface ---------------- */

void epg_open(int current_channel) {
    if (vis) return;
    vis = 1;
    g_cur_ch = current_channel;
    view_grid = 0;
    win_start = (long) time(NULL) / 1800 * 1800;
    keep_sel = 0;
    popup_hide();
    /* old data until the response, already with the current channel selected */
    if (g_msg) {
        int idx = find_row(current_channel, last_group);
        if (idx >= 0) {
            sel = idx;
            scroll = idx - ROWS / 2;
            clamp_scroll();
        }
    }
    if (act.request) act.request(win_start, WIN_DUR);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    render();
}

void epg_close(void) {
    if (!vis) return;
    vis = 0;
    popup_hide();
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

int epg_visible(void) { return vis; }

void epg_on_grid(cJSON *msg) {
    /* selection before reloading: channel and group */
    int keep = keep_sel && g_msg && sel < g_nch;
    int keep_ch = 0, keep_top = sel - scroll;
    char keep_grp[64] = "";
    if (keep) {
        keep_ch = ch_id(sel);
        if (g_ngrp) snprintf(keep_grp, sizeof(keep_grp), "%s", g_grp[group_at(sel)].id);
    }
    keep_sel = 0;
    free_data();
    g_msg = msg;

    const cJSON *chs = jget(msg, "channels");
    int n = cJSON_GetArraySize(chs);
    g_ch = n ? calloc((size_t) n, sizeof(*g_ch)) : NULL;
    const cJSON *c;
    cJSON_ArrayForEach(c, chs) {
        if (g_ch && cJSON_IsObject(c)) g_ch[g_nch++] = c;
    }
    const cJSON *gs = jget(msg, "groups");
    int ng = cJSON_GetArraySize(gs);
    g_grp = ng ? calloc((size_t) ng, sizeof(*g_grp)) : NULL;
    cJSON_ArrayForEach(c, gs) {
        if (!g_grp) break;
        grp *g = &g_grp[g_ngrp++];
        jid(c, "id", g->id, sizeof(g->id));
        snprintf(g->label, sizeof(g->label), "%s", jstr(c, "label"));
        g->start = (int) jnum(c, "start");
    }
    if (jnum(msg, "currentChannel") > 0) g_cur_ch = (int) jnum(msg, "currentChannel");

    int idx = -1;
    if (keep) idx = find_row(keep_ch, keep_grp);
    else if (g_cur_ch) idx = find_row(g_cur_ch, last_group);
    if (idx >= 0) {
        sel = idx;
        scroll = keep ? idx - keep_top : idx - ROWS / 2;
    }
    clamp_scroll();
    mt_log("[epg] %d rows, %d groups, window from %ld", g_nch, g_ngrp, (long) jnum(msg, "start"));
    if (vis) {
        if (popup) popup_render();
        render();
    }
}

void epg_refresh(void) {
    if (!vis) return;
    keep_sel = 1;
    if (act.request) act.request(win_start, WIN_DUR);
}

int epg_key(int key) {
    if (!vis) return 0;
    if (popup) {
        switch (key) {
        case KEY_UP: popup_move(-2); break;
        case KEY_DOWN: popup_move(2); break;
        case KEY_LEFT: popup_move(-1); break;
        case KEY_RIGHT: popup_move(1); break;
        case KEY_OK:
            popup_hide();
            to_group(popup_idx);
            break;
        case KEY_BACK:
        case KEY_BLUE: popup_hide(); break;
        default: break;
        }
        return 1;
    }
    switch (key) {
    case KEY_UP: move_up(); break;
    case KEY_DOWN: move_down(); break;
    case KEY_LEFT: page_up(); break;
    case KEY_RIGHT: page_down(); break;
    /* CH+ = next group in tvh order, CH- = previous */
    case KEY_CH_UP:
        if (g_ngrp) to_group((group_at(sel) + 1) % g_ngrp);
        break;
    case KEY_CH_DOWN:
        if (g_ngrp) to_group((group_at(sel) - 1 + g_ngrp) % g_ngrp);
        break;
    case KEY_OK: select_channel(); break;
    case KEY_INFO:
    case KEY_RECORD:
        if (g_msg && sel < g_nch && act.record) act.record(ch_id(sel), 0);
        break;
    case KEY_BACK: epg_close(); break;
    case KEY_RED: shift_time(-SHIFT); break;
    case KEY_GREEN: shift_time(SHIFT); break;
    case KEY_YELLOW:
        view_grid = !view_grid;
        render();
        break;
    case KEY_BLUE: popup_show(); break;
    default: break;
    }
    return 1;
}

int epg_pointer(int ev, int x, int y) {
    if (!vis) return 0;
    if (popup) {
        int n = (int) lv_obj_get_child_count(pop_list);
        for (int i = 0; i < n; i++) {
            if (!ui_hit(lv_obj_get_child(pop_list, i), x, y)) continue;
            if (i != popup_idx) popup_move(i - popup_idx);
            if (ev == PTR_UP) {
                popup_hide();
                to_group(i);
            }
            break;
        }
        return 1;
    }
    if (!g_msg) return 1;
    int ri = -1;
    for (int i = 0; i < ROWS; i++)
        if (scroll + i < g_nch && ui_hit(rows[i].obj, x, y)) ri = i;
    if (ri < 0) return 1;
    int r = scroll + ri;
    if (r != sel) {
        int old = sel;
        sel = r;
        update_selection(old);
    }
    if (ev != PTR_UP) return 1;
    if (x < INFO_W) {
        select_channel();
        return 1;
    }
    /* programme under the pointer */
    double ev_id = 0;
    row_ui *u = &rows[ri];
    if (view_grid) {
        lv_obj_update_layout(u->grid); /* cells just recreated (selection change) */
        int n = (int) lv_obj_get_child_count(u->grid);
        for (int k = 0; k < n; k++) {
            lv_obj_t *c = lv_obj_get_child(u->grid, k);
            if (ui_hit(c, x, y) && lv_obj_get_user_data(c)) ev_id = (double) (intptr_t) lv_obj_get_user_data(c);
        }
    } else {
        lv_area_t a;
        lv_obj_get_coords(u->obj, &a);
        ev_id = y - a.y1 < ROW_H / 2 ? u->now_id : u->next_id;
    }
    if (ev_id > 0 && act.record) act.record(ch_id(sel), ev_id);
    else select_channel();
    return 1;
}

void epg_wheel(int dir) {
    if (!vis) return;
    if (popup) popup_move(dir > 0 ? -1 : 1); /* two columns: the wheel moves one at a time */
    else if (dir > 0) move_up();
    else move_down();
}

void epg_tick(void) {
    if (!vis) return;
    uint32_t t = lv_tick_get();
    if (t - last_tick < 30000) return;
    last_tick = t;
    /* progress, change of the current programme, now line */
    if (g_msg && !view_grid)
        for (int i = 0; i < ROWS; i++) fill_row(i);
    update_now_line();
}
