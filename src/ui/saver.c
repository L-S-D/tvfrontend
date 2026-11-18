/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "saver.h"
#include "i18n.h"

#include "ui.h"

#include <time.h>

#define SAVER_DIM_OPA 153       /* black at 60 %: what is below shows at 40 % */
#define SAVER_MOVE_MS 30000     /* the clock of stage 2 moves this often */
#define SAVER_MARGIN 80

static lv_obj_t *veil, *box, *clock_l, *hint;
static int stage;
static uint32_t moved_at, ticked_at, seed;

void saver_init(void) {
    lv_obj_t *scr = lv_screen_active();
    veil = ui_box(scr, 0x000000, SAVER_DIM_OPA, 0);
    lv_obj_set_size(veil, lv_obj_get_width(scr), lv_obj_get_height(scr));
    lv_obj_set_pos(veil, 0, 0);
    lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);

    box = ui_box(veil, 0, LV_OPA_TRANSP, 0);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 6, 0);
    clock_l = ui_label(box, 46, 0, UI_DIM);
    hint = ui_label(box, 22, 0, UI_DIM);
    lv_label_set_text(hint, _("Screen protection \xE2\x80\x93 press any key"));
    lv_obj_set_style_opa(box, 110, 0); /* dim: a still text all the same */
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    seed = lv_tick_get() | 1;
}

static void update_clock(void) {
    char hm[8];
    ui_fmt_hm((long) time(NULL), hm, sizeof(hm));
    lv_label_set_text(clock_l, hm);
}

/* another place for the clock, within the margins of the screen */
static void move_box(void) {
    lv_obj_update_layout(box);
    int32_t w = lv_obj_get_width(veil) - lv_obj_get_width(box) - 2 * SAVER_MARGIN;
    int32_t h = lv_obj_get_height(veil) - lv_obj_get_height(box) - 2 * SAVER_MARGIN;
    seed = seed * 1103515245u + 12345u;
    int32_t x = SAVER_MARGIN + (w > 0 ? (int32_t) ((seed >> 8) % (uint32_t) w) : 0);
    seed = seed * 1103515245u + 12345u;
    int32_t y = SAVER_MARGIN + (h > 0 ? (int32_t) ((seed >> 8) % (uint32_t) h) : 0);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, x, y);
}

void saver_set(int s) {
    if (!veil || s == stage) return;
    stage = s;
    if (!s) {
        lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_style_bg_opa(veil, s == 1 ? SAVER_DIM_OPA : LV_OPA_COVER, 0);
    if (s == 2) {
        update_clock();
        move_box();
        moved_at = ticked_at = lv_tick_get();
        lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(veil); /* above layers made later (dialogs) */
}

int saver_stage(void) { return stage; }

void saver_tick(void) {
    if (!stage) return;
    /* a layer that brought itself to the front (a dialog opening by itself) */
    lv_obj_t *scr = lv_obj_get_parent(veil);
    if (lv_obj_get_index(veil) != (int32_t) lv_obj_get_child_count(scr) - 1) lv_obj_move_foreground(veil);
    if (stage != 2) return;
    uint32_t t = lv_tick_get();
    if (t - ticked_at >= 1000) {
        ticked_at = t;
        update_clock();
    }
    if (t - moved_at >= SAVER_MOVE_MS) {
        moved_at = t;
        move_box();
    }
}
