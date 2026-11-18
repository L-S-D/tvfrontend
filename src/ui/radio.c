/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "radio.h"

#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define RADIO_W 1040
#define RADIO_H 500         /* to start with; layout() makes it as high as its contents */
#define RADIO_MARGIN 60
#define RADIO_MOVE_MS 60000 /* the card moves this often */

static lv_obj_t *bg, *card, *logo, *num, *name, *clock_l;
static lv_obj_t *name_row, *rt_l, *now_row, *now_time, *now_title, *bar, *next_l;
static char logo_url[512];
static char sig[2048];
static epg_entry cur_now;
static uint32_t moved_at, ticked_at, seed;
static int have_logo, have_rt, have_now, have_next, centered;

static void layout(void);

static lv_obj_t *row(lv_obj_t *parent, int y) {
    lv_obj_t *r = ui_box(parent, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(r, RADIO_W - 80, LV_SIZE_CONTENT);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 22, 0);
    return r;
}

void radio_init(void) {
    lv_obj_t *scr = lv_screen_active();

    /* black instead of the empty video plane */
    bg = ui_box(scr, 0x000000, LV_OPA_COVER, 0);
    lv_obj_set_size(bg, lv_obj_get_width(scr), lv_obj_get_height(scr));
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_HIDDEN);

    card = ui_box(bg, UI_PANEL, LV_OPA_COVER, 24);
    lv_obj_set_size(card, RADIO_W, RADIO_H);

    logo = lv_image_create(card);
    lv_obj_set_size(logo, 420, 236);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 30);
    lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_CONTAIN);

    clock_l = ui_label(card, 40, 0, UI_DIM);
    lv_obj_align(clock_l, LV_ALIGN_TOP_RIGHT, -28, 22);

    name_row = row(card, 286);
    num = ui_label(name_row, 46, 1, UI_BLUE);
    name = ui_label(name_row, 46, 1, UI_TEXT);
    lv_obj_set_style_max_width(name, RADIO_W - 360, 0); /* clear of the clock */
    ui_label_dots(name);

    /* the RadioText (DAB: the dynamic label): what is on now, a song or the like */
    rt_l = ui_label(card, 34, 1, UI_TEXT);
    lv_obj_set_width(rt_l, RADIO_W - 120);
    lv_obj_set_style_text_align(rt_l, LV_TEXT_ALIGN_CENTER, 0);
    /* too long: scroll back and forth instead of "..." (like the banner) */
    lv_label_set_long_mode(rt_l, LV_LABEL_LONG_MODE_SCROLL);
    lv_obj_set_height(rt_l, lv_font_get_line_height(ui_font(34, 1)));

    /* the programme from the guide */
    now_row = row(card, 358);
    now_time = ui_label(now_row, 28, 0, UI_DIM);
    now_title = ui_label(now_row, 30, 0, UI_TEXT);
    lv_obj_set_style_max_width(now_title, RADIO_W - 320, 0);
    lv_label_set_long_mode(now_title, LV_LABEL_LONG_MODE_SCROLL);
    lv_obj_set_height(now_title, lv_font_get_line_height(ui_font(30, 0)));

    bar = lv_bar_create(card);
    lv_obj_set_size(bar, RADIO_W - 180, 8);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 418);
    lv_bar_set_range(bar, 0, 1000);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);

    next_l = ui_label(card, 28, 0, UI_DIM);
    lv_obj_set_width(next_l, RADIO_W - 180);
    lv_obj_align(next_l, LV_ALIGN_TOP_MID, 0, 440);
    lv_obj_set_style_text_align(next_l, LV_TEXT_ALIGN_CENTER, 0);
    ui_label_dots(next_l);

    centered = 1;
    layout();
    seed = lv_tick_get() | 1;
}

/* the card as high as what it shows: the logo, the name, the RadioText and the
 * programme, each when there is one */
static void layout(void) {
    int32_t y = have_logo ? 286 : 30;
    lv_obj_align(name_row, LV_ALIGN_TOP_MID, 0, y);
    int32_t bottom = y + 56;
    y += 72;
    if (have_rt) {
        lv_obj_align(rt_l, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_remove_flag(rt_l, LV_OBJ_FLAG_HIDDEN);
        bottom = y + 42;
        y += 60;
    } else {
        lv_obj_add_flag(rt_l, LV_OBJ_FLAG_HIDDEN);
    }
    if (have_now) {
        lv_obj_align(now_row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, y + 54);
        lv_obj_remove_flag(now_row, LV_OBJ_FLAG_HIDDEN);
        bottom = y + 62;
        y += 76;
    } else {
        lv_obj_add_flag(now_row, LV_OBJ_FLAG_HIDDEN);
    }
    if (have_next) {
        lv_obj_align(next_l, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_remove_flag(next_l, LV_OBJ_FLAG_HIDDEN);
        bottom = y + 36;
    } else {
        lv_obj_add_flag(next_l, LV_OBJ_FLAG_HIDDEN);
    }
    int32_t h = bottom + 28;
    lv_obj_set_height(card, h);
    if (centered) {
        lv_obj_center(card);
    } else if (lv_obj_get_y(card) + h > lv_obj_get_height(bg) - RADIO_MARGIN) {
        /* grew: stay on the screen */
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, lv_obj_get_x(card), lv_obj_get_height(bg) - RADIO_MARGIN - h);
    }
}

static void on_logo(void *user, const char *url, const lv_image_dsc_t *dsc) {
    (void) user;
    if (strcmp(url, logo_url) != 0) return; /* switched in the meantime */
    if (dsc) {
        lv_image_set_src(logo, dsc);
        lv_obj_remove_flag(logo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
    }
    have_logo = dsc != NULL;
    layout();
}

static void fmt_range(const epg_entry *e, char *out, size_t n) {
    char a[8], b[8];
    if (!e || !e->start) {
        out[0] = 0;
        return;
    }
    ui_fmt_hm(e->start, a, sizeof(a));
    ui_fmt_hm(e->stop, b, sizeof(b));
    snprintf(out, n, "%s - %s", a, b);
}

static void update_time(void) {
    long now = (long) time(NULL);
    char hm[8];
    ui_fmt_hm(now, hm, sizeof(hm));
    lv_label_set_text(clock_l, hm);
    if (cur_now.start && cur_now.stop > cur_now.start) {
        long total = cur_now.stop - cur_now.start, done = now - cur_now.start;
        if (done < 0) done = 0;
        if (done > total) done = total;
        lv_bar_set_value(bar, (int32_t) (done * 1000 / total), LV_ANIM_OFF);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }
}

void radio_set(int number, const char *nm, const char *icon_url, const char *text, const epg_entry *now,
               const epg_entry *next) {
    char s[sizeof(sig)];
    snprintf(s, sizeof(s), "%d|%s|%s|%s|%ld|%s|%ld|%s", number, nm ? nm : "", icon_url ? icon_url : "",
             text ? text : "", now ? now->start : 0, now ? now->title : "", next ? next->start : 0,
             next ? next->title : "");
    if (!strcmp(s, sig)) return;
    snprintf(sig, sizeof(sig), "%s", s);

    lv_label_set_text_fmt(num, "%d", number);
    lv_label_set_text(name, nm ? nm : "");
    if (strcmp(logo_url, icon_url ? icon_url : "") != 0) {
        snprintf(logo_url, sizeof(logo_url), "%s", icon_url ? icon_url : "");
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
        have_logo = 0;
        ui_image_fetch(logo_url, on_logo, NULL); /* from the cache: right away */
    }
    char rt[256];
    ui_oneline(rt, sizeof(rt), text ? text : "", sizeof(rt) - 1);
    lv_label_set_text(rt_l, rt);
    have_rt = rt[0] != 0;
    memset(&cur_now, 0, sizeof(cur_now));
    if (now) cur_now = *now;
    char r[32], t[256], l[320];
    fmt_range(now, r, sizeof(r));
    lv_label_set_text(now_time, r);
    ui_oneline(t, sizeof(t), now ? now->title : "", sizeof(t) - 1);
    lv_label_set_text(now_title, t);
    /* the next one: its start and title */
    ui_oneline(t, sizeof(t), next ? next->title : "", sizeof(t) - 1);
    if (next && next->start && t[0]) {
        ui_fmt_hm(next->start, r, sizeof(r));
        snprintf(l, sizeof(l), "%s  %s", r, t);
    } else {
        snprintf(l, sizeof(l), "%s", t);
    }
    lv_label_set_text(next_l, l);
    have_now = now && now->title[0];
    have_next = next && next->title[0];
    layout();
    update_time();
}

/* another place for the card, within the margins of the screen */
static void move_card(void) {
    seed = seed * 1103515245u + 12345u;
    int32_t w = lv_obj_get_width(bg) - RADIO_W - 2 * RADIO_MARGIN;
    int32_t h = lv_obj_get_height(bg) - lv_obj_get_height(card) - 2 * RADIO_MARGIN;
    int32_t x = RADIO_MARGIN + (w > 0 ? (int32_t) ((seed >> 8) % (uint32_t) w) : 0);
    seed = seed * 1103515245u + 12345u;
    int32_t y = RADIO_MARGIN + (h > 0 ? (int32_t) ((seed >> 8) % (uint32_t) h) : 0);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y); /* not set_pos: that keeps the centring */
    centered = 0;
}

void radio_show(int on) {
    if (on == radio_visible()) return;
    if (on) {
        centered = 1; /* a new radio session starts in the middle */
        lv_obj_center(card);
        moved_at = ticked_at = lv_tick_get();
        update_time();
        lv_obj_remove_flag(bg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bg, LV_OBJ_FLAG_HIDDEN);
    }
}

int radio_visible(void) { return bg && !lv_obj_has_flag(bg, LV_OBJ_FLAG_HIDDEN); }

void radio_tick(void) {
    if (!radio_visible()) return;
    uint32_t t = lv_tick_get();
    if (t - ticked_at >= 1000) {
        ticked_at = t;
        update_time();
    }
    if (t - moved_at >= RADIO_MOVE_MS) {
        moved_at = t;
        move_card();
    }
}
