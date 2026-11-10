/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "info.h"
#include "i18n.h"

#include "keys.h"
#include "log.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 1760
#define H 640
#define COL_X 270            /* text column as in the banner */
#define COL_W_FULL 1458
#define IMG_W 480            /* image 16:9 on the right */
#define IMG_H 270
#define COL_W_IMG (W - 28 - IMG_W - 32 - COL_X)

static lv_obj_t *panel, *logo, *num, *name, *clock_l;
static lv_obj_t *times, *left, *bar;
static lv_obj_t *col, *title, *subtitle, *meta, *txt_box, *txt;
static lv_obj_t *img, *foot, *hint;
static rxview *rx_view; /* below the logo: reception and DVB ids */
static info_data D;
static int showing_next, open_;
static char logo_url[512], img_url[512];

static lv_obj_t *label(lv_obj_t *parent, int px, int bold, uint32_t color) { return ui_label(parent, px, bold, color); }

static void on_logo(void *user, const char *url, const lv_image_dsc_t *dsc) {
    (void) user;
    if (strcmp(url, logo_url) != 0) return;
    if (dsc) {
        lv_image_set_src(logo, dsc);
        lv_obj_remove_flag(logo, LV_OBJ_FLAG_HIDDEN);
    }
}

static void layout(int with_image) {
    int w = with_image ? COL_W_IMG : COL_W_FULL;
    lv_obj_set_width(col, w);
    if (with_image) lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
}

static void on_img(void *user, const char *url, const lv_image_dsc_t *dsc) {
    (void) user;
    if (strcmp(url, img_url) != 0) return; /* a different programme by now */
    if (!dsc) {
        mt_log("[info] image not loadable: %s", url);
        layout(0);
        return;
    }
    lv_image_set_src(img, dsc);
    layout(1);
}

void info_init(void) {
    lv_obj_t *scr = lv_screen_active();
    panel = ui_box(scr, UI_PANEL, 235, 18);
    lv_obj_set_size(panel, W, H);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    logo = lv_image_create(panel);
    lv_obj_set_pos(logo, 28, 28);
    lv_obj_set_size(logo, 210, 118);
    lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_CONTAIN);

    num = label(panel, 46, 1, UI_BLUE);
    lv_obj_set_pos(num, COL_X, 22);
    name = label(panel, 46, 1, UI_TEXT);
    clock_l = label(panel, 40, 0, UI_TEXT);
    lv_obj_align(clock_l, LV_ALIGN_TOP_RIGHT, -32, 26);

    times = label(panel, 30, 0, UI_DIM);
    lv_obj_set_pos(times, COL_X, 92);
    left = label(panel, 28, 0, UI_DIM);
    lv_obj_align(left, LV_ALIGN_TOP_RIGHT, -32, 92);

    bar = lv_bar_create(panel);
    lv_obj_set_pos(bar, COL_X, 140);
    lv_obj_set_size(bar, COL_W_FULL, 8);
    lv_bar_set_range(bar, 0, 1000);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);

    /* column: title, subtitle (up to 2 lines), additional details, long text (scrollable) */
    col = ui_box(panel, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(col, COL_X, 166);
    lv_obj_set_size(col, COL_W_FULL, 410);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);

    title = label(col, 40, 1, UI_TEXT);
    lv_obj_set_width(title, LV_PCT(100));
    ui_label_dots(title);
    subtitle = label(col, 28, 0, UI_DIM);
    lv_obj_set_width(subtitle, LV_PCT(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_MODE_DOTS);
    /* subtitle up to 4 lines: the private broadcasters send genre, year, age rating in it */
    lv_obj_set_style_max_height(subtitle, 4 * lv_font_get_line_height(ui_font(28, 0)), 0);
    meta = label(col, 24, 0, UI_HINT_BLUE);
    lv_obj_set_width(meta, LV_PCT(100));
    ui_label_dots(meta);

    txt_box = ui_box(col, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_width(txt_box, LV_PCT(100));
    lv_obj_set_flex_grow(txt_box, 1);
    lv_obj_set_scroll_dir(txt_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(txt_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_top(txt_box, 6, 0);
    txt = label(txt_box, 26, 0, UI_TEXT);
    lv_obj_set_width(txt, LV_PCT(100));
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(txt, 4, 0);

    img = lv_image_create(panel);
    lv_obj_set_pos(img, W - 28 - IMG_W, 172);
    lv_obj_set_size(img, IMG_W, IMG_H);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    /* reception below the logo as in the banner, in detail */
    rx_view = rxview_create(panel, 28, 172, 1);
    rxview_set(rx_view, NULL, NULL);

    foot = label(panel, 26, 0, UI_DIM);
    lv_obj_set_pos(foot, COL_X, H - 52);
    lv_obj_set_width(foot, 1000);
    ui_label_dots(foot);
    hint = label(panel, 20, 0, UI_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -32, -20);

    rxview_set(rx_view, NULL, NULL);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

static void fmt_range(const info_event *e, char *out, size_t n) {
    char a[16], b[16];
    ui_fmt_hm(e->start, a, sizeof(a));
    ui_fmt_hm(e->stop, b, sizeof(b));
    snprintf(out, n, "%s - %s", a, b);
}

static void update_time(void) {
    if (!open_) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    lv_label_set_text_fmt(clock_l, "%02d:%02d", tm.tm_hour, tm.tm_min);
    const info_event *e = showing_next ? &D.next : &D.now;
    if (!showing_next && e->stop > e->start) {
        long d = e->stop - e->start, p = (long) now - e->start;
        if (p < 0) p = 0;
        if (p > d) p = d;
        lv_bar_set_value(bar, (int32_t) (p * 1000 / d), LV_ANIM_OFF);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
        long r = (e->stop - (long) now + 59) / 60;
        lv_label_set_text_fmt(left, _("%ld min left"), r > 0 ? r : 0);
    } else {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        long r = (e->start - (long) now + 59) / 60;
        if (r >= 60) lv_label_set_text_fmt(left, _("in %ld h %ld min"), r / 60, r % 60);
        else if (r > 0) lv_label_set_text_fmt(left, _("in %ld min"), r);
        else lv_label_set_text(left, "");
    }
}

static void show_event(int next) {
    showing_next = next && D.have_next;
    const info_event *e = showing_next ? &D.next : &D.now;
    char t[64];
    fmt_range(e, t, sizeof(t));
    lv_label_set_text(times, t);
    lv_label_set_text(title, e->title[0] ? e->title : _("No programme information"));
    lv_label_set_text(subtitle, e->subtitle);
    if (e->subtitle[0]) lv_obj_remove_flag(subtitle, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(meta, e->meta);
    if (e->meta[0]) lv_obj_remove_flag(meta, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(meta, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(txt, e->text);
    lv_obj_scroll_to_y(txt_box, 0, LV_ANIM_OFF);
    /* footer: the respective other programme */
    const info_event *o = showing_next ? &D.now : &D.next;
    int have_o = showing_next ? D.have_now : D.have_next;
    if (have_o) {
        char a[16];
        ui_fmt_hm(o->start, a, sizeof(a));
        lv_label_set_text_fmt(foot, showing_next ? _("Now: %s  %s") : _("Next: %s  %s"), a, o->title);
    } else {
        /* next one shown because no programme is known for the current time */
        lv_label_set_text(foot, showing_next ? _("Now: no programme information") : "");
    }
    /* only characters that Lato has (no arrows) */
    lv_label_set_text(hint, have_o ? (showing_next ? _("Left now  \xC2\xB7  Up/down text  \xC2\xB7  Back close")
                                                   : _("Right next  \xC2\xB7  Up/down text  \xC2\xB7  Back close"))
                                   : _("Up/down text  \xC2\xB7  Back close"));
    /* image: first without, once loaded beside the text */
    snprintf(img_url, sizeof(img_url), "%s", e->image);
    layout(0);
    lv_image_set_src(img, NULL); /* the previous image may drop out of the cache */
    if (img_url[0]) ui_image_fetch_transient(img_url, on_img, NULL);
    update_time();
}

void info_open(const info_data *d) {
    D = *d;
    open_ = 1;
    lv_label_set_text_fmt(num, "%d", D.number);
    lv_label_set_text(name, D.channel);
    lv_obj_align_to(name, num, LV_ALIGN_OUT_RIGHT_MID, 22, 0);
    snprintf(logo_url, sizeof(logo_url), "%s", D.logo);
    lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
    ui_image_fetch(logo_url, on_logo, NULL);
    show_event(!D.have_now && D.have_next);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel);
    mt_log("[info] %s: %s (%zu chars of text%s)", D.channel, (showing_next ? D.next : D.now).title,
           strlen((showing_next ? D.next : D.now).text), img_url[0] ? ", image" : "");
}

void info_set_rx(const osd_rx *rx, const char *ids) { rxview_set(rx_view, rx, ids); }

void info_close(void) {
    if (!open_) return;
    open_ = 0;
    img_url[0] = 0;
    lv_image_set_src(img, NULL);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

int info_visible(void) { return open_; }

uint32_t info_event_id(void) { return open_ ? (showing_next ? D.next.id : D.now.id) : 0; }

static void scroll(int dir) {
    int step = 3 * lv_font_get_line_height(ui_font(26, 0));
    lv_obj_scroll_by_bounded(txt_box, 0, dir > 0 ? step : -step, LV_ANIM_ON);
}

void info_wheel(int dir) {
    if (open_) scroll(dir);
}

int info_key(int key) {
    if (!open_) return 0;
    switch (key) {
    case KEY_BACK:
    case KEY_OK:
    case KEY_YELLOW:
    case KEY_EXIT:
    case KEY_INFO:
        info_close();
        return 1;
    case KEY_RIGHT:
        if (!showing_next && D.have_next) show_event(1);
        return 1;
    case KEY_LEFT:
        if (showing_next && D.have_now) show_event(0);
        return 1;
    case KEY_UP: scroll(1); return 1;
    case KEY_DOWN: scroll(-1); return 1;
    case KEY_CH_UP:
    case KEY_CH_DOWN:
        info_close(); /* channel change is up to TVF */
        return 0;
    default: return 1; /* open: swallow everything else */
    }
}

int info_pointer(int ev, int x, int y) {
    (void) x;
    (void) y;
    if (!open_) return 0;
    if (ev == PTR_UP) info_close();
    return 1;
}

void info_tick(void) {
    static uint32_t last;
    if (!open_) return;
    uint32_t now = lv_tick_get();
    if (now - last < 1000) return;
    last = now;
    update_time();
}
