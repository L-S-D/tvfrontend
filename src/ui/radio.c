/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "radio.h"

#include "ui.h"
#include "imgtrim.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define RADIO_W 1040
#define RADIO_H 500         /* to start with; layout() makes it as high as its contents */
#define RADIO_MARGIN 60
#define RADIO_MOVE_MS 60000 /* the card moves this often */
#define BANNER_TOP 668      /* the channel banner: 362 high, 50 above the bottom of 1080 */

static lv_obj_t *bg, *card, *logo, *num, *name, *clock_l;
static lv_obj_t *name_row, *rt_l, *now_row, *now_time, *now_title, *bar, *next_l;
static char logo_url[512];
static char sig[2048];
static epg_entry cur_now;
static uint32_t moved_at, ticked_at, seed;
static int have_logo, have_rt, have_now, have_next, centered; /* centered: at its place after tuning, not moved yet */
static int cur_number;
/* the slide of the station's slideshow that is on: in the place of the logo */
static lv_image_dsc_t slide_dsc;
static uint8_t *slide_px;
static unsigned slide_serial;
static int have_slide;

#define LOGO_W 420
#define LOGO_H 236
#define SLIDE_W 480 /* a slide has 320 x 240: one and a half times that */
#define SLIDE_H 360

static void layout(void);

/* A text too long for its line: two lines, then a smaller font, then one line
 * more; what is longer still ends in dots. Nothing scrolls. The steps go from
 * the font of the label down; fixed = the label keeps the width (its text is
 * centred in it), else it is as wide as its text. Returns the height. */
typedef struct {
    int px, lines;
} fit_step;

static int32_t fit_text(lv_obj_t *l, const char *text, int32_t width, int bold, int fixed, const fit_step *st, int n) {
    int32_t ls = lv_obj_get_style_text_line_space(l, 0), sp = lv_obj_get_style_text_letter_space(l, 0);
    for (int i = 0; i < n; i++) {
        const lv_font_t *f = ui_font(st[i].px, bold);
        int32_t lh = lv_font_get_line_height(f);
        lv_point_t sz;
        lv_text_get_size(&sz, text, f, sp, ls, width, LV_TEXT_FLAG_NONE);
        int lines = (int) ((sz.y + ls) / (lh + ls));
        if (lines > st[i].lines && i < n - 1) continue;
        int show = lines > st[i].lines ? st[i].lines : lines < 1 ? 1 : lines;
        int32_t h = show * lh + (show - 1) * ls;
        lv_obj_set_style_text_font(l, f, 0);
        lv_label_set_long_mode(l, lines > st[i].lines ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, fixed || lines > 1 ? width : LV_SIZE_CONTENT);
        lv_obj_set_height(l, h);
        lv_label_set_text(l, text);
        return h;
    }
    return 0;
}

static const fit_step RT_FIT[] = {{34, 1}, {34, 2}, {28, 2}, {28, 3}};
static const fit_step NOW_FIT[] = {{30, 1}, {30, 2}, {26, 2}};
#define RT_W (RADIO_W - 120)
#define NOW_W (RADIO_W - 320)
static int32_t rt_h, now_h;

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

    /* black like the screen: on an OLED only the texts and the picture are lit */
    card = ui_box(bg, 0x000000, LV_OPA_COVER, 24);
    lv_obj_set_size(card, RADIO_W, RADIO_H);

    logo = lv_image_create(card);
    lv_obj_set_size(logo, LOGO_W, LOGO_H);
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
    lv_obj_set_style_text_align(rt_l, LV_TEXT_ALIGN_CENTER, 0);
    rt_h = fit_text(rt_l, "", RT_W, 1, 1, RT_FIT, 4); /* too long: see fit_text */

    /* the programme from the guide */
    now_row = row(card, 358);
    now_time = ui_label(now_row, 28, 0, UI_DIM);
    now_title = ui_label(now_row, 30, 0, UI_TEXT);
    now_h = fit_text(now_title, "", NOW_W, 0, 0, NOW_FIT, 3);

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
    int32_t y = have_slide ? 30 + SLIDE_H + 20 : have_logo ? 286 : 30;
    lv_obj_set_size(logo, have_slide ? SLIDE_W : LOGO_W, have_slide ? SLIDE_H : LOGO_H);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_align(name_row, LV_ALIGN_TOP_MID, 0, y);
    int32_t bottom = y + 56;
    y += 72;
    if (have_rt) {
        lv_obj_align(rt_l, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_remove_flag(rt_l, LV_OBJ_FLAG_HIDDEN);
        bottom = y + rt_h;
        y += rt_h + 18;
    } else {
        lv_obj_add_flag(rt_l, LV_OBJ_FLAG_HIDDEN);
    }
    if (have_now) {
        /* a title of more lines than one moves what is below it down */
        int32_t more = now_h - lv_font_get_line_height(ui_font(NOW_FIT[0].px, 0));
        if (more < 0) more = 0;
        lv_obj_align(now_row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, y + 54 + more);
        lv_obj_remove_flag(now_row, LV_OBJ_FLAG_HIDDEN);
        bottom = y + 62 + more;
        y += 76 + more;
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
        /* after tuning to a station: at the top, clear of the banner at the
         * bottom (it begins at BANNER_TOP); a card too high for that goes up
         * as far as it can */
        int32_t y = RADIO_MARGIN;
        if (y + h > BANNER_TOP - 10) y = BANNER_TOP - 10 - h;
        if (y < 16) y = 16;
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    } else if (lv_obj_get_y(card) + h > lv_obj_get_height(bg) - RADIO_MARGIN) {
        /* grew: stay on the screen */
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, lv_obj_get_x(card), lv_obj_get_height(bg) - RADIO_MARGIN - h);
    }
}

static void on_logo(void *user, const char *url, const lv_image_dsc_t *dsc) {
    (void) user;
    if (strcmp(url, logo_url) != 0) return; /* switched in the meantime */
    have_logo = dsc != NULL;
    if (have_slide) return; /* a slide is in its place */
    if (dsc) {
        lv_image_set_src(logo, dsc);
        lv_obj_remove_flag(logo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
    }
    layout();
}

void radio_set_slide(unsigned serial, const uint8_t *data, size_t len) {
    if (serial == slide_serial) return;
    slide_serial = serial;
    uint8_t *px = NULL;
    int w = 0, h = 0;
    if (serial && data && !img_decode(data, len, 960, 540, &px, &w, &h)) px = NULL;
    if (have_slide) {
        /* LVGL may have the old pixels in its cache under this descriptor */
        lv_image_cache_drop(&slide_dsc);
    }
    free(slide_px);
    slide_px = px;
    if (px) {
        memset(&slide_dsc, 0, sizeof(slide_dsc));
        slide_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        slide_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        slide_dsc.header.w = (uint32_t) w;
        slide_dsc.header.h = (uint32_t) h;
        slide_dsc.header.stride = (uint32_t) w * 4;
        slide_dsc.data = px;
        slide_dsc.data_size = (uint32_t) w * (uint32_t) h * 4;
        have_slide = 1;
        lv_image_set_src(logo, &slide_dsc);
        lv_obj_remove_flag(logo, LV_OBJ_FLAG_HIDDEN);
        layout();
    } else if (have_slide) {
        /* none any more (another channel, or one that was not readable): the logo again */
        have_slide = 0;
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
        layout();
        ui_image_fetch(logo_url, on_logo, NULL);
    }
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
    if (number != cur_number) {
        /* another station: the card back to the top (the banner shows now), and it stays a while */
        cur_number = number;
        centered = 1;
        moved_at = lv_tick_get();
    }
    if (strcmp(logo_url, icon_url ? icon_url : "") != 0) {
        snprintf(logo_url, sizeof(logo_url), "%s", icon_url ? icon_url : "");
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
        have_logo = 0;
        ui_image_fetch(logo_url, on_logo, NULL); /* from the cache: right away */
    }
    char rt[256];
    ui_oneline(rt, sizeof(rt), text ? text : "", sizeof(rt) - 1);
    rt_h = fit_text(rt_l, rt, RT_W, 1, 1, RT_FIT, 4);
    have_rt = rt[0] != 0;
    memset(&cur_now, 0, sizeof(cur_now));
    if (now) cur_now = *now;
    char r[32], t[256], l[320];
    fmt_range(now, r, sizeof(r));
    lv_label_set_text(now_time, r);
    ui_oneline(t, sizeof(t), now ? now->title : "", sizeof(t) - 1);
    now_h = fit_text(now_title, t, NOW_W, 0, 0, NOW_FIT, 3);
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
        centered = 1; /* a new radio session starts at the top */
        layout();
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
