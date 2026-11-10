/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rxview.h"

#include "i18n.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_W 222

struct rxview {
    int detail;
    lv_obj_t *box, *prov, *name[2], *bar[2], *val[2], *quality, *ca, *ca_l, *via, *ids_cap, *ids;
};

static void show(lv_obj_t *o, int on) {
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static uint32_t rx_color(int level) {
    return level == 1 ? UI_GREEN : level == 2 ? UI_YELLOW : level == 3 ? UI_RED : UI_BLUE;
}

rxview *rxview_create(lv_obj_t *parent, int x, int y, int detail) {
    rxview *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->detail = detail;
    /* the detail view has the quality line below the bars and the ids at the end */
    int q = detail ? 26 : 0;
    v->box = ui_box(parent, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(v->box, x, y);
    lv_obj_set_size(v->box, RX_W, detail ? 300 : 196);
    lv_obj_remove_flag(v->box, LV_OBJ_FLAG_SCROLLABLE);
    v->prov = ui_label(v->box, 22, 1, UI_TEXT);
    lv_obj_set_pos(v->prov, 0, 0);
    lv_obj_set_width(v->prov, RX_W);
    ui_label_dots(v->prov);
    for (int k = 0; k < 2; k++) {
        int yy = 38 + k * 30;
        v->name[k] = ui_label(v->box, 18, 0, UI_DIM);
        lv_obj_set_pos(v->name[k], 0, yy + 2);
        lv_label_set_text(v->name[k], k ? _("Signal") : "SNR");
        v->bar[k] = lv_bar_create(v->box);
        lv_obj_set_pos(v->bar[k], 62, yy + 10);
        lv_obj_set_size(v->bar[k], 64, 8);
        lv_bar_set_range(v->bar[k], 0, 1000);
        lv_obj_set_style_bg_color(v->bar[k], lv_color_hex(UI_BAR_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(v->bar[k], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(v->bar[k], 4, LV_PART_MAIN);
        lv_obj_set_style_radius(v->bar[k], 4, LV_PART_INDICATOR);
        v->val[k] = ui_label(v->box, 20, 0, UI_TEXT);
        lv_obj_set_pos(v->val[k], 134, yy);
    }
    if (detail) {
        v->quality = ui_label(v->box, 18, 0, UI_DIM);
        lv_obj_set_pos(v->quality, 0, 100);
        lv_obj_set_width(v->quality, RX_W);
        ui_label_dots(v->quality);
    }
    v->ca = ui_box(v->box, 0, LV_OPA_TRANSP, 4);
    lv_obj_set_pos(v->ca, 0, 106 + q);
    lv_obj_set_size(v->ca, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(v->ca, 8, 0);
    lv_obj_set_style_pad_ver(v->ca, 2, 0);
    lv_obj_set_style_border_width(v->ca, 1, 0);
    v->ca_l = ui_label(v->ca, 18, 1, UI_TEXT);
    lv_obj_set_style_text_letter_space(v->ca_l, 1, 0);
    v->via = ui_label(v->box, 18, 0, UI_DIM);
    lv_obj_set_pos(v->via, 0, 144 + q);
    lv_obj_set_width(v->via, RX_W);
    lv_label_set_long_mode(v->via, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_height(v->via, 2 * lv_font_get_line_height(ui_font(18, 0)));
    if (detail) {
        int iy = 144 + q + 2 * lv_font_get_line_height(ui_font(18, 0)) + 14;
        v->ids_cap = ui_label(v->box, 18, 0, UI_DIM);
        lv_obj_set_pos(v->ids_cap, 0, iy);
        lv_label_set_text(v->ids_cap, "ONID \xC2\xB7 TSID \xC2\xB7 SID");
        v->ids = ui_label(v->box, 20, 0, UI_TEXT);
        lv_obj_set_pos(v->ids, 0, iy + 24);
        lv_obj_set_width(v->ids, RX_W);
        ui_label_dots(v->ids);
    }
    return v;
}

void rxview_show(rxview *v, int on) {
    if (v) show(v->box, on);
}

void rxview_set(rxview *v, const osd_rx *r, const char *ids) {
    if (!v) return;
    lv_label_set_text(v->prov, r ? r->provider : "");
    const char *val[2] = {r ? r->snr : "", r ? r->signal : ""};
    float frac[2] = {r ? r->snr_frac : 0, r ? r->signal_frac : 0};
    for (int k = 0; k < 2; k++) {
        int on = val[k][0] != 0;
        show(v->name[k], on);
        show(v->bar[k], on);
        show(v->val[k], on);
        lv_label_set_text(v->val[k], val[k]);
        int x = (int) (frac[k] * 1000);
        lv_bar_set_value(v->bar[k], x < 0 ? 0 : x > 1000 ? 1000 : x, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(v->bar[k], lv_color_hex(rx_color(r ? r->level : 0)), LV_PART_INDICATOR);
    }
    if (v->quality) {
        /* status in the colour of the bars, BER/UNC */
        lv_label_set_text(v->quality, r ? r->quality : "");
        lv_obj_set_style_text_color(v->quality, lv_color_hex(r && r->level ? rx_color(r->level) : UI_DIM), 0);
        show(v->quality, r && r->quality[0]);
    }
    char t[96] = "";
    if (r && r->ca_failed) snprintf(t, sizeof(t), "%s", _("DESCRAMBLING FAILED"));
    else if (r && r->ca[0]) {
        snprintf(t, sizeof(t), "%s", r->ca);
        for (char *p = t; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char) (*p - 'a' + 'A');
    }
    show(v->ca, t[0] != 0);
    lv_label_set_text(v->ca_l, t);
    lv_obj_set_style_border_color(v->ca, lv_color_hex(r && r->ca_failed ? UI_RED : 0xFFFFFF), 0);
    lv_obj_set_style_border_opa(v->ca, r && r->ca_failed ? LV_OPA_COVER : 90, 0);
    char w[160] = "";
    if (r && r->ca[0] && !r->ca_failed)
        snprintf(w, sizeof(w), "%s%s%s", r->ca_time, r->ca_time[0] && r->ca_via[0] ? " \xC2\xB7 " : "", r->ca_via);
    if (r && r->error[0]) snprintf(w + strlen(w), sizeof(w) - strlen(w), "%stvh: %s", w[0] ? "\n" : "", r->error);
    /* BER/UNC: in the detail view in the quality line */
    if (r && r->errs[0] && !r->error[0] && !v->detail)
        snprintf(w + strlen(w), sizeof(w) - strlen(w), "%s%s", w[0] ? "\n" : "", r->errs);
    lv_label_set_text(v->via, w);
    if (v->ids) {
        const char *s = ids ? ids : "";
        lv_label_set_text(v->ids, s);
        show(v->ids, s[0] != 0);
        show(v->ids_cap, s[0] != 0);
    }
}
