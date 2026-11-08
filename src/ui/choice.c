/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "choice.h"

#include "keys.h"
#include "ui.h"

#include <stdio.h>

#define OPA(f) ((lv_opa_t) ((f) * 255 + 0.5))

static lv_obj_t *box, *title_l, *list;
static lv_obj_t *rows[CHOICE_MAX];
static int n_opt, idx, vis;
static choice_cb on_pick;
static void *pick_user;

void choice_init(void) {
    box = ui_box(lv_screen_active(), 0x141414, OPA(0.97), 8);
    lv_obj_set_width(box, 760);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, 918, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(box, OPA(0.15), 0);
    lv_obj_set_style_pad_ver(box, 24, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);

    title_l = ui_label(box, 24, 1, UI_TEXT);
    lv_obj_set_style_text_opa(title_l, OPA(0.6), 0);
    lv_obj_set_style_pad_hor(title_l, 32, 0);
    lv_obj_set_style_pad_bottom(title_l, 16, 0);
    lv_obj_t *line = ui_box(box, 0xFFFFFF, OPA(0.1), 0);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_margin_bottom(line, 8, 0);

    list = ui_box(box, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
}

static void mark(int i, int on) {
    if (i >= 0 && i < n_opt) lv_obj_set_style_bg_opa(rows[i], on ? OPA(0.25) : LV_OPA_TRANSP, 0);
}

void choice_open(const char *title, const char *const *labels, int n, int checked, int sel, choice_cb cb, void *user) {
    int marks[CHOICE_MAX] = {0};
    if (checked >= 0 && checked < CHOICE_MAX) marks[checked] = 1;
    choice_open_multi(title, labels, n, marks, sel, cb, user);
}

void choice_open_multi(const char *title, const char *const *labels, int n, const int *checked, int sel, choice_cb cb,
                       void *user) {
    if (n > CHOICE_MAX) n = CHOICE_MAX;
    lv_obj_clean(list);
    lv_label_set_text(title_l, title ? title : "");
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = ui_box(list, UI_BLUE, LV_OPA_TRANSP, 0);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(r, 32, 0);
        lv_obj_set_style_pad_ver(r, 14, 0);
        lv_obj_t *l = ui_label(r, 28, 0, UI_TEXT);
        lv_obj_set_width(l, 640);
        ui_label_dots(l);
        lv_label_set_text(l, labels[i] ? labels[i] : "");
        if (checked && checked[i]) {
            lv_obj_t *c = lv_label_create(r);
            lv_obj_set_style_text_font(c, &lv_font_montserrat_22, 0);
            lv_obj_set_style_text_color(c, lv_color_hex(UI_BLUE), 0);
            lv_label_set_text(c, LV_SYMBOL_OK);
            lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        rows[i] = r;
    }
    n_opt = n;
    idx = sel >= 0 && sel < n ? sel : 0;
    on_pick = cb;
    pick_user = user;
    vis = 1;
    mark(idx, 1);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(box);
    lv_obj_update_layout(box);
    if (n_opt) lv_obj_scroll_to_view(rows[idx], LV_ANIM_OFF);
}

void choice_close(void) {
    vis = 0;
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
}

int choice_visible(void) { return vis; }

static void move(int d) {
    if (!n_opt) return;
    mark(idx, 0);
    idx = (idx + d + n_opt) % n_opt;
    mark(idx, 1);
    lv_obj_scroll_to_view(rows[idx], LV_ANIM_OFF);
}

int choice_pointer(int ev, int x, int y) {
    if (!vis) return 0;
    int hit = -1;
    for (int i = 0; i < n_opt; i++)
        if (ui_hit(rows[i], x, y)) hit = i;
    if (hit >= 0 && hit != idx) {
        mark(idx, 0);
        idx = hit;
        mark(idx, 1);
    }
    if (ev == PTR_UP && hit >= 0) {
        choice_close();
        if (on_pick) on_pick(pick_user, hit);
    }
    return 1;
}

int choice_key(int key) {
    if (!vis) return 0;
    switch (key) {
    case KEY_UP: move(-1); break;
    case KEY_DOWN: move(1); break;
    case KEY_OK: {
        choice_close();
        if (on_pick && n_opt) on_pick(pick_user, idx);
        break;
    }
    case KEY_BACK: choice_close(); break;
    default: break;
    }
    return 1;
}
