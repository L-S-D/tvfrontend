/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "saver.h"

#include "ui.h"


#define SAVER_DIM_OPA 153 /* black at 60 %: what is below shows at 40 % */

static lv_obj_t *veil;
static int stage;

void saver_init(void) {
    lv_obj_t *scr = lv_screen_active();
    veil = ui_box(scr, 0x000000, SAVER_DIM_OPA, 0);
    lv_obj_set_size(veil, lv_obj_get_width(scr), lv_obj_get_height(scr));
    lv_obj_set_pos(veil, 0, 0);
    lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);
}

void saver_set(int s) {
    if (!veil || s == stage) return;
    stage = s;
    if (!s) {
        lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_style_bg_opa(veil, s == 1 ? SAVER_DIM_OPA : LV_OPA_COVER, 0);
    lv_obj_remove_flag(veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(veil); /* above layers made later (dialogs) */
}

int saver_stage(void) { return stage; }

void saver_tick(void) {
    if (!stage) return;
    /* a layer that brought itself to the front (a dialog opening by itself) */
    lv_obj_t *scr = lv_obj_get_parent(veil);
    if (lv_obj_get_index(veil) != (int32_t) lv_obj_get_child_count(scr) - 1) lv_obj_move_foreground(veil);
}
