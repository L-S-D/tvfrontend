/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "osd.h"
#include "i18n.h"

#include "log.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define C_PANEL UI_PANEL
#define C_TEXT UI_TEXT
#define C_DIM UI_DIM
#define C_ACCENT UI_BLUE
#define C_BAR_BG UI_BAR_BG

static lv_obj_t *panel, *logo, *num, *name, *clock_l;
static lv_obj_t *now_time, *now_title, *now_sub, *now_plot, *bar, *now_left;
static lv_obj_t *next_time, *next_title;
static lv_obj_t *streams;
/* reception (tvh) below the logo: provider, bars for SNR and signal, box for the decryption */
static rxview *rx_view;
static osd_rx rx_cur;
static int rx_have;
static lv_obj_t *seek_box, *seek_label;
static lv_obj_t *ts_box, *ts_l1, *ts_l2;
static char streams_sig[1024];
static lv_obj_t *numbox, *numlabel;
static lv_obj_t *msgbox, *msglabel;

static uint32_t hide_at, msg_hide_at, last_tick;
static int rec_mode;
static epg_entry cur_now, cur_next;
static char logo_url[512];

static lv_obj_t *box(lv_obj_t *parent, int opa) { return ui_box(parent, C_PANEL, (lv_opa_t) opa, 18); }

static lv_obj_t *label(lv_obj_t *parent, int px, int bold, uint32_t color) { return ui_label(parent, px, bold, color); }

static void fmt_hm(long t, char *out, size_t n) { ui_fmt_hm(t, out, n); }

static void fmt_range(const epg_entry *e, char *out, size_t n) {
    char a[8], b[8];
    if (!e || !e->start) {
        out[0] = 0;
        return;
    }
    fmt_hm(e->start, a, sizeof(a));
    fmt_hm(e->stop, b, sizeof(b));
    snprintf(out, n, "%s - %s", a, b);
}

void osd_init(void) {
    lv_obj_t *scr = lv_screen_active();

    /* banner: 1760 x 362 at the bottom */
    panel = box(scr, 225);
    lv_obj_set_size(panel, 1760, 362);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -50);

    logo = lv_image_create(panel);
    lv_obj_set_pos(logo, 28, 28);
    lv_obj_set_size(logo, 210, 118);
    lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_CONTAIN);

    /* reception (tvhstat) below the logo; bar colour = quality according to tvh */
    rx_view = rxview_create(panel, 28, 156, 0);

    num = label(panel, 46, 1, C_ACCENT);
    lv_obj_set_pos(num, 270, 22);
    name = label(panel, 46, 1, C_TEXT);
    clock_l = label(panel, 40, 0, C_TEXT);
    lv_obj_align(clock_l, LV_ALIGN_TOP_RIGHT, -32, 26);

    now_time = label(panel, 30, 0, C_DIM);
    lv_obj_set_pos(now_time, 270, 92);
    now_title = label(panel, 34, 1, C_TEXT);
    lv_obj_set_pos(now_title, 470, 88);
    lv_obj_set_width(now_title, 1000);
    /* too long: scroll back and forth instead of "..." (like the HTML5 app) */
    lv_label_set_long_mode(now_title, LV_LABEL_LONG_MODE_SCROLL);
    lv_obj_set_height(now_title, lv_font_get_line_height(ui_font(34, 1)));
    now_left = label(panel, 28, 0, C_DIM);
    lv_obj_align(now_left, LV_ALIGN_TOP_RIGHT, -32, 92);

    now_sub = label(panel, 26, 0, C_DIM);
    lv_obj_set_pos(now_sub, 470, 130);
    lv_obj_set_width(now_sub, 1000);
    ui_label_dots(now_sub);

    bar = lv_bar_create(panel);
    lv_obj_set_pos(bar, 270, 168);
    lv_obj_set_size(bar, 1458, 8);
    lv_bar_set_range(bar, 0, 1000);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);

    /* synopsis: two lines */
    now_plot = label(panel, 22, 0, C_DIM);
    lv_obj_set_pos(now_plot, 270, 186);
    lv_obj_set_width(now_plot, 1458);
    lv_label_set_long_mode(now_plot, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(now_plot, 2 * lv_font_get_line_height(ui_font(22, 0)));
    lv_obj_set_style_text_opa(now_plot, 200, 0);

    next_time = label(panel, 28, 0, C_DIM);
    lv_obj_set_pos(next_time, 270, 256);
    next_title = label(panel, 30, 0, C_DIM);
    lv_obj_set_pos(next_title, 470, 253);
    lv_obj_set_width(next_title, 1250);
    ui_label_dots(next_title);

    /* stream info: groups of tag and entries */
    streams = ui_box(panel, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(streams, 270, 306);
    lv_obj_set_size(streams, 1460, 40);
    lv_obj_set_flex_flow(streams, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(streams, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(streams, 8, 0);

    /* target time of the pointer above the bar (recording) */
    seek_box = ui_box(panel, UI_BLUE, LV_OPA_COVER, 4);
    lv_obj_set_size(seek_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(seek_box, 10, 0);
    lv_obj_set_style_pad_ver(seek_box, 2, 0);
    seek_label = label(seek_box, 24, 1, 0xFFFFFF);
    lv_obj_add_flag(seek_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    /* digit entry top right */
    numbox = box(scr, 225);
    lv_obj_set_size(numbox, 300, 150);
    lv_obj_align(numbox, LV_ALIGN_TOP_RIGHT, -60, 60);
    numlabel = label(numbox, 96, 1, C_ACCENT);
    lv_obj_center(numlabel);
    lv_obj_add_flag(numbox, LV_OBJ_FLAG_HIDDEN);

    /* timeshift top right */
    ts_box = box(scr, 225);
    lv_obj_set_size(ts_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(ts_box, 28, 0);
    lv_obj_set_style_pad_ver(ts_box, 16, 0);
    lv_obj_set_flex_flow(ts_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ts_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(ts_box, 6, 0);
    lv_obj_align(ts_box, LV_ALIGN_TOP_RIGHT, -60, 60);
    ts_l1 = label(ts_box, 34, 1, C_TEXT);
    ts_l2 = label(ts_box, 20, 0, C_DIM);
    lv_obj_add_flag(ts_box, LV_OBJ_FLAG_HIDDEN);

    /* short message top centre */
    msgbox = ui_box(scr, 0x141414, 242, 4);
    lv_obj_set_size(msgbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(msgbox, 28, 0);
    lv_obj_set_style_pad_ver(msgbox, 14, 0);
    lv_obj_set_style_border_side(msgbox, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(msgbox, 5, 0);
    lv_obj_set_style_border_color(msgbox, lv_color_hex(UI_GREEN), 0);
    lv_obj_align(msgbox, LV_ALIGN_TOP_MID, 0, 40);
    msglabel = label(msgbox, 26, 0, C_TEXT);
    lv_obj_add_flag(msgbox, LV_OBJ_FLAG_HIDDEN);
}

static void chip(lv_obj_t *parent, const char *text, int kind) {
    /* kind 0: tag (VIDEO/AUDIO), 1: entry, 2: current entry */
    lv_obj_t *c = ui_box(parent, kind == 0 ? 0xFFFFFF : 0, kind == 0 ? 38 : LV_OPA_TRANSP, kind == 0 ? 3 : 4);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 2, 0);
    if (kind) {
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(kind == 2 ? UI_BLUE : 0xFFFFFF), 0);
        lv_obj_set_style_border_opa(c, kind == 2 ? LV_OPA_COVER : 51, 0);
    }
    lv_obj_t *l = label(c, kind == 0 ? 18 : 20, kind == 0, C_TEXT);
    if (kind == 0) lv_obj_set_style_text_letter_space(l, 1, 0);
    if (kind == 1) lv_obj_set_style_text_opa(l, 140, 0);
    lv_label_set_text(l, text);
    if (kind == 0) lv_obj_set_style_margin_left(c, lv_obj_get_child_count(parent) > 1 ? 20 : 0, 0);
}

void osd_set_streams(const char *video, const char *const *audio, int n, int current) {
    char sig[sizeof(streams_sig)];
    int o = snprintf(sig, sizeof(sig), "%s|%d", video ? video : "", current);
    for (int i = 0; i < n && o > 0 && (size_t) o < sizeof(sig); i++)
        o += snprintf(sig + o, sizeof(sig) - (size_t) o, "|%s", audio[i]);
    if (!strcmp(sig, streams_sig)) return;
    snprintf(streams_sig, sizeof(streams_sig), "%s", sig);
    lv_obj_clean(streams);
    if (video && *video) {
        chip(streams, _("VIDEO"), 0);
        chip(streams, video, 2);
    }
    if (n > 0) {
        chip(streams, _("AUDIO"), 0);
        for (int i = 0; i < n; i++) chip(streams, audio[i], i == current ? 2 : 1);
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
}

static void rx_render(void) {
    rxview_show(rx_view, !rec_mode); /* reception only applies live */
    rxview_set(rx_view, rx_have ? &rx_cur : NULL, NULL);
}

void osd_set_rx(const osd_rx *rx) {
    if (rx) rx_cur = *rx;
    rx_have = rx != NULL;
    rx_render();
}

void osd_set_channel(int number, const char *nm, const char *icon_url) {
    rec_mode = 0;
    osd_set_rx(NULL);
    lv_label_set_text_fmt(num, "%d", number);
    lv_label_set_text(name, nm ? nm : "");
    lv_obj_align_to(name, num, LV_ALIGN_OUT_RIGHT_MID, 22, 0);
    snprintf(logo_url, sizeof(logo_url), "%s", icon_url ? icon_url : "");
    lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
    ui_image_fetch(logo_url, on_logo, NULL);
    /* clear Now/Next until the new info arrives */
    epg_entry none;
    memset(&none, 0, sizeof(none));
    osd_set_nownext(&none, &none);
}

static void update_progress(void) {
    long now = (long) time(NULL);
    if (rec_mode) { /* progress comes from osd_rec_progress */
        char hm[8];
        fmt_hm(now, hm, sizeof(hm));
        lv_label_set_text(clock_l, hm);
        return;
    }
    if (cur_now.start && cur_now.stop > cur_now.start) {
        long total = cur_now.stop - cur_now.start, done = now - cur_now.start;
        if (done < 0) done = 0;
        if (done > total) done = total;
        lv_bar_set_value(bar, (int32_t) (done * 1000 / total), LV_ANIM_OFF);
        lv_label_set_text_fmt(now_left, _("%ld min left"), (total - done + 59) / 60);
    } else {
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_label_set_text(now_left, "");
    }
    char hm[8];
    fmt_hm(now, hm, sizeof(hm));
    lv_label_set_text(clock_l, hm);
}

void osd_get_nownext(epg_entry *now, epg_entry *next) {
    *now = cur_now;
    *next = cur_next;
}

void osd_set_nownext(const epg_entry *now, const epg_entry *next) {
    char r[32];
    cur_now = *now;
    cur_next = *next;
    fmt_range(now, r, sizeof(r));
    lv_label_set_text(now_time, r);
    char t[256];
    ui_oneline(t, sizeof(t), now->title, sizeof(t) - 1);
    lv_label_set_text(now_title, t);
    ui_oneline(t, sizeof(t), now->subtitle, sizeof(t) - 1);
    lv_label_set_text(now_sub, t);
    {
        char pl[1024];
        ui_oneline(pl, sizeof(pl), now->plot, sizeof(pl) - 1);
        /* if the subtitle is at the start of the synopsis, do not show it twice */
        const char *pp = pl;
        size_t sl = strlen(t);
        if (sl && !strncmp(pl, t, sl)) pp = pl + sl;
        while (*pp == ' ' || *pp == '-' || *pp == ':' || *pp == '.') pp++;
        lv_label_set_text(now_plot, pp);
    }
    fmt_range(next, r, sizeof(r));
    lv_label_set_text(next_time, r);
    ui_oneline(t, sizeof(t), next->title, sizeof(t) - 1);
    lv_label_set_text(next_title, t);
    update_progress();
}

void osd_rec_mode(const char *title, const char *subtitle, const char *plot, const char *channel, long start,
                  const char *icon_url) {
    rec_mode = 1;
    rx_render(); /* reception only applies live */
    lv_label_set_text(num, "");
    lv_label_set_text(name, channel ? channel : "");
    lv_obj_align_to(name, num, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    snprintf(logo_url, sizeof(logo_url), "%s", icon_url ? icon_url : "");
    lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
    ui_image_fetch(logo_url, on_logo, NULL);
    char t[256];
    if (start) {
        time_t tt = (time_t) start;
        struct tm tm;
        localtime_r(&tt, &tm);
        snprintf(t, sizeof(t), _("%s %02d/%02d"), ui_weekday(tm.tm_wday), tm.tm_mday, tm.tm_mon + 1);
    } else {
        t[0] = 0;
    }
    lv_label_set_text(now_time, t);
    ui_oneline(t, sizeof(t), title, sizeof(t) - 1);
    lv_label_set_text(now_title, t);
    ui_oneline(t, sizeof(t), subtitle, sizeof(t) - 1);
    lv_label_set_text(now_sub, t);
    {
        char pl[1024];
        ui_oneline(pl, sizeof(pl), plot, sizeof(pl) - 1);
        lv_label_set_text(now_plot, pl);
    }
    /* only characters that Lato has (no arrow symbols) */
    lv_label_set_text(next_title, _("OK pause   Left \xE2\x88\x92" "10 s   Right +30 s   Down \xE2\x88\x92" "5 min   "
                                    "Up +5 min   Back list"));
    osd_rec_progress(0, 0, "");
}

void osd_rec_progress(double pos, double dur, const char *state) {
    char a[32], b[32];
    long p = pos > 0 ? (long) pos : 0, d = dur > 0 ? (long) dur : 0;
    if (p / 3600) snprintf(a, sizeof(a), "%ld:%02ld:%02ld", p / 3600, p % 3600 / 60, p % 60);
    else snprintf(a, sizeof(a), "%ld:%02ld", p / 60, p % 60);
    long r = d > p ? d - p : 0;
    if (r / 3600) snprintf(b, sizeof(b), "\xE2\x88\x92%ld:%02ld:%02ld", r / 3600, r % 3600 / 60, r % 60);
    else snprintf(b, sizeof(b), "\xE2\x88\x92%ld:%02ld", r / 60, r % 60);
    lv_label_set_text(next_time, a);
    lv_label_set_text(now_left, d ? b : "");
    lv_bar_set_value(bar, d ? (int32_t) (p * 1000 / d > 1000 ? 1000 : p * 1000 / d) : 0, LV_ANIM_OFF);
    lv_label_set_text(num, state && *state ? state : "");
    lv_obj_align_to(name, num, LV_ALIGN_OUT_RIGHT_MID, state && *state ? 22 : 0, 0);
}

void osd_live_mode(void) {
    rec_mode = 0;
    rx_render();
    lv_obj_add_flag(seek_box, LV_OBJ_FLAG_HIDDEN);
}

void osd_tshift(const char *l1, const char *l2) {
    if (!l1) {
        lv_obj_add_flag(ts_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(ts_l1, l1);
    lv_label_set_text(ts_l2, l2 ? l2 : "");
    lv_obj_remove_flag(ts_box, LV_OBJ_FLAG_HIDDEN);
}

int osd_bar_frac(int x, int y, int any_y, double *frac) {
    if (lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) return 0;
    lv_area_t a;
    lv_obj_get_coords(bar, &a);
    int w = a.x2 - a.x1 + 1;
    if (w <= 0) return 0;
    if (!any_y && (y < a.y1 - 24 || y > a.y2 + 24 || x < a.x1 - 12 || x > a.x2 + 12)) return 0;
    double f = (double) (x - a.x1) / w;
    *frac = f < 0 ? 0 : f > 1 ? 1 : f;
    return 1;
}

void osd_seek_hover(double frac, const char *text) {
    if (frac < 0 || !rec_mode) {
        lv_obj_add_flag(seek_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(seek_label, text ? text : "");
    lv_obj_remove_flag(seek_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(seek_box);
    int bw = lv_obj_get_width(seek_box);
    int bx = lv_obj_get_x(bar) + (int) (frac * lv_obj_get_width(bar)) - bw / 2;
    lv_obj_set_pos(seek_box, bx, lv_obj_get_y(bar) - lv_obj_get_height(seek_box) - 6);
}

void osd_show(int ms) {
    update_progress();
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    hide_at = ms > 0 ? lv_tick_get() + (uint32_t) ms : 0;
}

void osd_hide(void) {
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    hide_at = 0;
}

int osd_visible(void) { return !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN); }

void osd_number(const char *digits) {
    if (!digits || !*digits) {
        lv_obj_add_flag(numbox, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(numlabel, digits);
    lv_obj_center(numlabel);
    lv_obj_remove_flag(numbox, LV_OBJ_FLAG_HIDDEN);
}

void osd_message(const char *text, int ms, int error) {
    if (!text || !*text) {
        lv_obj_add_flag(msgbox, LV_OBJ_FLAG_HIDDEN);
        msg_hide_at = 0;
        return;
    }
    lv_label_set_text(msglabel, text);
    lv_obj_set_style_border_color(msgbox, lv_color_hex(error ? UI_RED : UI_GREEN), 0);
    lv_obj_remove_flag(msgbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(msgbox);
    msg_hide_at = ms > 0 ? lv_tick_get() + (uint32_t) ms : 0;
}

int osd_message_is(const char *text) {
    return text && *text && !lv_obj_has_flag(msgbox, LV_OBJ_FLAG_HIDDEN) &&
           !strcmp(lv_label_get_text(msglabel), text);
}

void osd_tick(void) {
    uint32_t t = lv_tick_get();
    if (hide_at && (int32_t) (t - hide_at) >= 0) osd_hide();
    if (msg_hide_at && (int32_t) (t - msg_hide_at) >= 0) osd_message(NULL, 0, 0);
    if (t - last_tick >= 1000) {
        last_tick = t;
        if (osd_visible()) update_progress();
    }
}
