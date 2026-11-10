/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "settings.h"

#include "i18n.h"
#include "keys.h"
#include "log.h"
#include "prefs.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPA(f) ((lv_opa_t) ((f) * 255 + 0.5))
#define HDR_H 100
#define FOOT_H 70
#define ROW_H 56
#define LIST_X 160
#define LIST_W 1600
#define MAX_ITEMS 32

typedef enum { T_HEADER, T_CHOICE, T_TEXT, T_NUMBER, T_PASSWORD, T_FIND, T_SAVE, T_CANCEL } item_type;

typedef struct {
    item_type type;
    const char *key;   /* prefs key */
    const char *label; /* msgid */
    const char *const *vals, *const *labels; /* T_CHOICE; labels are msgids or own names */
    int nopts;
    int off_empty;     /* empty value means "off" (optional components) */
    int need_value;    /* empty value: back to the default */
    int native;        /* T_CHOICE labels are not translated (language names) */
    char value[256], orig[256];
    lv_obj_t *row, *l_label, *l_value;
} item;

static settings_actions act;
static item items[MAX_ITEMS];
static int nitems, sel;
static lv_obj_t *root, *list, *foot, *note;

/* server search: servers found (-1 = not searched yet), the one in the
 * fields; apply_pending: found before the page opened */
#define FIND_KEY "tvh.find" /* pseudo key of the search row, not saved */
#define MAX_SERVERS 8
static settings_server servers[MAX_SERVERS];
static int nservers = -1, srv_sel, searching, apply_pending;
static char egress_orig[256]; /* hbbtv.egress as stored: rules, direct or proxy URL */

/* on-screen keyboard */
static lv_obj_t *osk, *osk_title, *osk_field, *osk_keys[6][10];
static int editing = -1, kr, kc, shift;
static char ebuf[256];
static const char *const KB[6][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", "-"},
    {"z", "x", "c", "v", "b", "n", "m", ".", "_", "@"},
    {":", "/", "?", "=", "&", "+", "!", "#", "%", "~"},
    {"\001shift", "\001space", "\001del", "\001clear", "\001done", NULL, NULL, NULL, NULL, NULL},
};

/* languages: automatic + what res/lang has (i18n_languages) */
#define MAX_LANGS 16
static const char *lang_v[MAX_LANGS + 1], *lang_l[MAX_LANGS + 1];
static const char *const EG_V[] = {"rules", "direct", "proxy"};
static const char *const KL_V[] = {"classic", "hbbtv"};
static const char *const KL_L[] = {N_("Standard"), N_("HbbTV first")};
static const char *const EG_L[] = {N_("Rules per channel"), N_("Direct"), N_("Fixed proxy")};

#define EGRESS_PROXY "hbbtv.egress_proxy" /* pseudo key: fixed proxy of the egress choice */

static lv_obj_t *lbl(lv_obj_t *p, int px, int bold, uint32_t color, lv_opa_t opa) {
    lv_obj_t *l = ui_label(p, px, bold, color);
    if (opa != LV_OPA_COVER) lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

static void show(lv_obj_t *o, int on) {
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- values ---------------- */

static void pref_str(const char *key, char *out, size_t n) {
    if (act.current) {
        act.current(key, out, n);
        return;
    }
    const cJSON *v = prefs_get(key);
    if (cJSON_IsString(v)) snprintf(out, n, "%s", v->valuestring);
    else if (cJSON_IsNumber(v)) snprintf(out, n, "%d", (int) v->valuedouble);
    else out[0] = 0;
}

static item *add(item_type t, const char *key, const char *label) {
    if (nitems >= MAX_ITEMS) return NULL;
    item *it = &items[nitems++];
    memset(it, 0, sizeof(*it));
    it->type = t;
    it->key = key;
    it->label = label;
    return it;
}

static item *add_choice(const char *key, const char *label, const char *const *v, const char *const *l, int n) {
    item *it = add(T_CHOICE, key, label);
    if (it) {
        it->vals = v;
        it->labels = l;
        it->nopts = n;
    }
    return it;
}

static void build_items(void) {
    item *it;
    nitems = 0;
    add(T_HEADER, NULL, N_("General"));
    lang_v[0] = "auto";
    lang_l[0] = N_("Automatic (TV)");
    int nl = 1 + i18n_languages(lang_v + 1, MAX_LANGS);
    for (int i = 1; i < nl; i++) lang_l[i] = i18n_language_name(lang_v[i]);
    it = add_choice("ui.lang", N_("Language"), lang_v, lang_l, nl);
    if (it) it->native = 1;
    add_choice("keys.layout", N_("Key layout"), KL_V, KL_L, 2);
    add(T_HEADER, NULL, N_("TVheadend"));
    if (act.find) add(T_FIND, FIND_KEY, N_("Search for servers"));
    it = add(T_TEXT, "tvh.host", N_("Server address"));
    if (it) it->need_value = 1;
    add(T_NUMBER, "tvh.http_port", N_("HTTP port"));
    add(T_NUMBER, "tvh.htsp_port", N_("HTSP port"));
    add(T_TEXT, "tvh.user", N_("User"));
    add(T_PASSWORD, "tvh.pass", N_("Password"));
    add(T_HEADER, NULL, N_("HbbTV"));
    add_choice("hbbtv.egress", N_("Internet access"), EG_V, EG_L, 3);
    add(T_TEXT, EGRESS_PROXY, N_("Fixed proxy"));
    add(T_HEADER, NULL, N_("Kodi remote (Kore)"));
    it = add(T_TEXT, "kore.name", N_("Name"));
    if (it) it->need_value = 1;
    it = add(T_NUMBER, "kore.http_port", N_("HTTP port"));
    if (it) it->off_empty = 1;
    it = add(T_NUMBER, "kore.tcp_port", N_("TCP port"));
    if (it) it->off_empty = 1;
    it = add(T_TEXT, "kore.user", N_("User"));
    if (it) it->need_value = 1; /* empty = without login (entry removed) */
    it = add(T_PASSWORD, "kore.pass", N_("Password"));
    if (it) it->need_value = 1;
    add(T_HEADER, NULL, "");
    add(T_SAVE, NULL, N_("Save and restart"));
    add(T_CANCEL, NULL, N_("Cancel"));

    /* current values */
    for (int i = 0; i < nitems; i++) {
        item *t = &items[i];
        switch (t->type) {
        case T_CHOICE:
        case T_TEXT:
        case T_NUMBER:
        case T_PASSWORD:
            if (strcmp(t->key, EGRESS_PROXY)) pref_str(t->key, t->value, sizeof(t->value));
            break;
        default: break;
        }
    }
    /* egress: "rules", "direct" or the proxy address itself */
    for (int i = 0; i < nitems; i++) {
        if (items[i].type != T_CHOICE || strcmp(items[i].key, "hbbtv.egress")) continue;
        char *v = items[i].value;
        snprintf(egress_orig, sizeof(egress_orig), "%s", v);
        if (!v[0]) snprintf(items[i].value, sizeof(items[i].value), "rules");
        else if (strcmp(v, "rules") && strcmp(v, "direct")) {
            for (int k = 0; k < nitems; k++)
                if (items[k].key && !strcmp(items[k].key, EGRESS_PROXY))
                    snprintf(items[k].value, sizeof(items[k].value), "%s", egress_orig);
            snprintf(items[i].value, sizeof(items[i].value), "proxy");
        }
    }
    for (int i = 0; i < nitems; i++) {
        item *t = &items[i];
        if (t->type == T_CHOICE && !t->value[0]) snprintf(t->value, sizeof(t->value), "%s", t->vals[0]);
        memcpy(t->orig, t->value, sizeof(t->orig));
    }
}

static int selectable(int i) { return i >= 0 && i < nitems && items[i].type != T_HEADER; }

static const char *choice_label(const item *t) {
    for (int k = 0; k < t->nopts; k++)
        if (!strcmp(t->value, t->vals[k])) return t->native && k ? t->labels[k] : _(t->labels[k]);
    return t->value;
}

static void value_text(const item *t, char *out, size_t n) {
    out[0] = 0;
    switch (t->type) {
    case T_CHOICE: snprintf(out, n, "%s", choice_label(t)); break;
    case T_PASSWORD:
        if (t->value[0]) {
            size_t o = 0;
            for (size_t i = 0; t->value[i] && o + 4 < n && i < 16; i++) o += (size_t) snprintf(out + o, n - o, "\xE2\x80\xA2");
        } else {
            snprintf(out, n, "\xE2\x80\x94");
        }
        break;
    case T_TEXT:
    case T_NUMBER:
        if (t->value[0] && !(t->type == T_NUMBER && t->off_empty && !strcmp(t->value, "0")))
            snprintf(out, n, "%s", t->value);
        else snprintf(out, n, "%s", t->off_empty ? _("off") : "\xE2\x80\x94");
        break;
    case T_FIND:
        if (searching) snprintf(out, n, "%s", _("Searching\xE2\x80\xA6"));
        else if (nservers < 0) snprintf(out, n, "%s", _("OK starts the search"));
        else if (!nservers) snprintf(out, n, "%s", _("No server found"));
        else if (nservers == 1) snprintf(out, n, "%s", servers[srv_sel].label);
        else snprintf(out, n, "%s  (%d/%d)", servers[srv_sel].label, srv_sel + 1, nservers);
        break;
    default: break;
    }
}

/* a server found: its address and ports into the fields (not saved yet) */
static void apply_server(int k) {
    const settings_server *s = &servers[k];
    for (int i = 0; i < nitems; i++) {
        item *t = &items[i];
        if (!t->key) continue;
        if (!strcmp(t->key, "tvh.host")) snprintf(t->value, sizeof(t->value), "%s", s->host);
        else if (!strcmp(t->key, "tvh.http_port") && s->http_port > 0)
            snprintf(t->value, sizeof(t->value), "%d", s->http_port);
        else if (!strcmp(t->key, "tvh.htsp_port") && s->htsp_port > 0)
            snprintf(t->value, sizeof(t->value), "%d", s->htsp_port);
    }
}

/* fixed proxy row only matters with egress "proxy" */
static int egress_is_proxy(void) {
    for (int i = 0; i < nitems; i++)
        if (items[i].type == T_CHOICE && !strcmp(items[i].key, "hbbtv.egress")) return !strcmp(items[i].value, "proxy");
    return 0;
}

/* ---------------- list ---------------- */

static void render(void) {
    for (int i = 0; i < nitems; i++) {
        item *t = &items[i];
        if (!t->row) continue;
        lv_label_set_text(t->l_label, _(t->label));
        if (t->l_value) {
            char v[300];
            value_text(t, v, sizeof(v));
            /* changed (not saved yet): yellow */
            int changed = strcmp(t->value, t->orig) != 0;
            lv_label_set_text(t->l_value, v);
            lv_obj_set_style_text_color(t->l_value, lv_color_hex(changed ? UI_YELLOW : UI_TEXT), 0);
            int dim = t->key && !strcmp(t->key, EGRESS_PROXY) && !egress_is_proxy();
            lv_obj_set_style_text_opa(t->l_value, dim ? OPA(0.35) : LV_OPA_COVER, 0);
        }
        int on = i == sel;
        lv_obj_set_style_bg_opa(t->row, on ? OPA(0.25) : LV_OPA_TRANSP, 0);
        lv_obj_set_style_outline_width(t->row, on ? 2 : 0, 0);
    }
    if (selectable(sel) && items[sel].row) {
        /* first item: also show the header above it */
        int first = 1;
        for (int i = 0; i < sel; i++)
            if (selectable(i)) first = 0;
        if (first) lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
        else lv_obj_scroll_to_view(items[sel].row, LV_ANIM_OFF);
    }
    lv_label_set_text(foot, editing >= 0 ? _("Arrows choose a key  \xC2\xB7  OK type  \xC2\xB7  Digits direct  \xC2\xB7  Back cancel")
                                         : _("Up/down select  \xC2\xB7  OK or left/right change  \xC2\xB7  Back close"));
}

static void build_rows(void) {
    lv_obj_clean(list);
    for (int i = 0; i < nitems; i++) {
        item *t = &items[i];
        t->row = ui_box(list, UI_BLUE, LV_OPA_TRANSP, 6);
        lv_obj_set_size(t->row, LIST_W, t->type == T_HEADER ? ROW_H - 6 : ROW_H);
        lv_obj_set_style_outline_color(t->row, lv_color_hex(UI_BLUE), 0);
        lv_obj_set_style_outline_pad(t->row, -2, 0);
        lv_obj_remove_flag(t->row, LV_OBJ_FLAG_SCROLLABLE);
        if (t->type == T_HEADER) {
            t->l_label = lbl(t->row, 22, 1, UI_HINT_BLUE, LV_OPA_COVER);
            lv_obj_align(t->l_label, LV_ALIGN_BOTTOM_LEFT, 16, -4);
            t->l_value = NULL;
            continue;
        }
        int action = t->type == T_SAVE || t->type == T_CANCEL;
        t->l_label = lbl(t->row, 26, action, action ? UI_TEXT : UI_DIM, LV_OPA_COVER);
        lv_obj_align(t->l_label, LV_ALIGN_LEFT_MID, 24, 0);
        if (action) {
            t->l_value = NULL;
            continue;
        }
        t->l_value = lbl(t->row, 26, 0, UI_TEXT, LV_OPA_COVER);
        lv_obj_set_width(t->l_value, LIST_W - 560);
        ui_label_dots(t->l_value);
        lv_obj_align(t->l_value, LV_ALIGN_LEFT_MID, 520, 0);
    }
}

/* ---------------- keyboard ---------------- */

static int krow_len(int r) {
    int n = 0;
    while (n < 10 && KB[r][n]) n++;
    return n;
}

static const char *key_text(int r, int c) {
    const char *k = KB[r][c];
    if (!k) return "";
    if (k[0] == '\001') {
        const char *w = k + 1;
        return !strcmp(w, "shift")   ? (shift ? _("abc") : _("ABC"))
               : !strcmp(w, "space") ? _("Space")
               : !strcmp(w, "del")   ? _("Delete")
               : !strcmp(w, "clear") ? _("Clear")
                                     : _("Apply");
    }
    static char up[4];
    if (shift && k[0] >= 'a' && k[0] <= 'z' && !k[1]) {
        up[0] = (char) (k[0] - 'a' + 'A');
        up[1] = 0;
        return up;
    }
    return k;
}

static void osk_render(void) {
    item *t = editing >= 0 ? &items[editing] : NULL;
    if (!t) return;
    lv_label_set_text(osk_title, _(t->label));
    if (t->type == T_PASSWORD) {
        /* hidden, only the last character to check the input */
        char m[300];
        size_t o = 0, l = strlen(ebuf);
        for (size_t i = 0; i + 1 < l && o + 4 < sizeof(m); i++) o += (size_t) snprintf(m + o, sizeof(m) - o, "\xE2\x80\xA2");
        snprintf(m + o, sizeof(m) - o, "%s|", l ? ebuf + l - 1 : "");
        lv_label_set_text(osk_field, m);
    } else {
        lv_label_set_text_fmt(osk_field, "%s|", ebuf);
    }
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 10; c++) {
            lv_obj_t *b = osk_keys[r][c];
            if (!b) continue;
            lv_obj_t *l = lv_obj_get_child(b, 0);
            lv_label_set_text(l, key_text(r, c));
            int on = r == kr && c == kc;
            lv_obj_set_style_bg_color(b, lv_color_hex(on ? UI_BLUE : 0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : OPA(0.08), 0);
        }
}

static void edit_start(int i) {
    editing = i;
    snprintf(ebuf, sizeof(ebuf), "%s", items[i].type == T_NUMBER && !strcmp(items[i].value, "0") ? "" : items[i].value);
    kr = items[i].type == T_NUMBER ? 0 : 1;
    kc = 0;
    shift = 0;
    show(osk, 1);
    lv_obj_move_foreground(osk);
    /* move the row above the keyboard */
    lv_obj_update_layout(root);
    lv_area_t ra, oa;
    lv_obj_get_coords(items[i].row, &ra);
    lv_obj_get_coords(osk, &oa);
    if (ra.y2 > oa.y1 - 12) lv_obj_scroll_to_y(list, lv_obj_get_scroll_y(list) + (ra.y2 - (oa.y1 - 12)), LV_ANIM_OFF);
    osk_render();
    render();
}

static void edit_end(int keep) {
    if (editing < 0) return;
    if (keep) {
        item *t = &items[editing];
        /* port empty = off (0) */
        snprintf(t->value, sizeof(t->value), "%s", t->type == T_NUMBER && t->off_empty && !ebuf[0] ? "0" : ebuf);
        /* a fixed proxy entered: that is the choice */
        if (!strcmp(t->key, EGRESS_PROXY) && ebuf[0])
            for (int i = 0; i < nitems; i++)
                if (items[i].type == T_CHOICE && !strcmp(items[i].key, "hbbtv.egress"))
                    snprintf(items[i].value, sizeof(items[i].value), "proxy");
    }
    editing = -1;
    show(osk, 0);
    lv_obj_readjust_scroll(list, LV_ANIM_OFF);
    render();
}

static void type_char(const char *s) {
    size_t l = strlen(ebuf);
    if (items[editing].type == T_NUMBER && (s[0] < '0' || s[0] > '9' || l >= 5)) return;
    if (l + strlen(s) < sizeof(ebuf) - 1) strcat(ebuf, s);
}

static void press_key(int r, int c) {
    const char *k = KB[r][c];
    if (!k) return;
    if (k[0] == '\001') {
        const char *w = k + 1;
        if (!strcmp(w, "shift")) shift = !shift;
        else if (!strcmp(w, "space")) type_char(" ");
        else if (!strcmp(w, "del")) {
            size_t l = strlen(ebuf);
            if (l) ebuf[l - 1] = 0;
        } else if (!strcmp(w, "clear")) ebuf[0] = 0;
        else {
            edit_end(1);
            return;
        }
    } else {
        type_char(key_text(r, c));
    }
    osk_render();
}

static void build_osk(void) {
    osk = ui_box(root, 0x141414, LV_OPA_COVER, 10);
    lv_obj_set_size(osk, 1180, 560);
    lv_obj_align(osk, LV_ALIGN_BOTTOM_MID, 0, -FOOT_H); /* down to the end of the list */
    lv_obj_remove_flag(osk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(osk, 1, 0);
    lv_obj_set_style_border_color(osk, lv_color_hex(UI_BLUE), 0);
    osk_title = lbl(osk, 22, 1, UI_HINT_BLUE, LV_OPA_COVER);
    lv_obj_set_pos(osk_title, 30, 18);
    lv_obj_t *fb = ui_box(osk, 0xFFFFFF, OPA(0.06), 6);
    lv_obj_set_pos(fb, 30, 54);
    lv_obj_set_size(fb, 1120, 60);
    osk_field = lbl(fb, 28, 0, UI_TEXT, LV_OPA_COVER);
    lv_obj_set_width(osk_field, 1090);
    lv_label_set_long_mode(osk_field, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_align(osk_field, LV_ALIGN_LEFT_MID, 14, 0);
    for (int r = 0; r < 6; r++) {
        int n = krow_len(r), kw = r == 5 ? 216 : 104, gap = 8;
        int x0 = (1180 - (n * kw + (n - 1) * gap)) / 2;
        for (int c = 0; c < n; c++) {
            lv_obj_t *b = ui_box(osk, 0xFFFFFF, OPA(0.08), 6);
            lv_obj_set_pos(b, x0 + c * (kw + gap), 136 + r * 68);
            lv_obj_set_size(b, kw, 60);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *l = lbl(b, 26, 0, UI_TEXT, LV_OPA_COVER);
            lv_obj_center(l);
            osk_keys[r][c] = b;
        }
    }
    show(osk, 0);
}

static int osk_key(int key) {
    if (key >= KEY_DIGIT0 && key <= KEY_DIGIT0 + 9) {
        char d[2] = {(char) ('0' + key - KEY_DIGIT0), 0};
        type_char(d);
        osk_render();
        return 1;
    }
    switch (key) {
    case KEY_LEFT: kc = kc > 0 ? kc - 1 : krow_len(kr) - 1; break;
    case KEY_RIGHT: kc = kc + 1 < krow_len(kr) ? kc + 1 : 0; break;
    case KEY_UP:
        kr = kr > 0 ? kr - 1 : 5;
        if (kc >= krow_len(kr)) kc = krow_len(kr) - 1;
        break;
    case KEY_DOWN:
        kr = kr < 5 ? kr + 1 : 0;
        if (kc >= krow_len(kr)) kc = krow_len(kr) - 1;
        break;
    case KEY_OK: press_key(kr, kc); return 1;
    case KEY_BACK:
    case KEY_EXIT: edit_end(0); return 1;
    default: return 1;
    }
    osk_render();
    return 1;
}

/* ---------------- actions ---------------- */

static void cycle(item *t, int dir) {
    int k = 0;
    for (int i = 0; i < t->nopts; i++)
        if (!strcmp(t->value, t->vals[i])) k = i;
    k = (k + dir + t->nopts) % t->nopts;
    snprintf(t->value, sizeof(t->value), "%s", t->vals[k]);
}

static void save(void) {
    int n = 0;
    const char *fixed = "";
    for (int i = 0; i < nitems; i++)
        if (items[i].key && !strcmp(items[i].key, EGRESS_PROXY)) fixed = items[i].value;
    for (int i = 0; i < nitems; i++) {
        item *t = &items[i];
        if (!t->key || !strcmp(t->key, EGRESS_PROXY) || t->type == T_FIND) continue;
        if (t->type == T_CHOICE && !strcmp(t->key, "hbbtv.egress")) {
            /* the fixed proxy is part of this choice; without an address it stays as it was */
            const char *v = strcmp(t->value, "proxy") ? t->value : fixed;
            if (v[0] && strcmp(v, egress_orig[0] ? egress_orig : "rules")) {
                prefs_set("hbbtv.egress", cJSON_CreateString(v));
                n++;
            }
            continue;
        }
        if (!strcmp(t->value, t->orig)) continue;
        n++;
        switch (t->type) {
        case T_NUMBER: {
            int v = atoi(t->value);
            if (t->off_empty && v == 0) prefs_set(t->key, cJSON_CreateNumber(0)); /* off */
            else prefs_set(t->key, v > 0 && v < 65536 ? cJSON_CreateNumber(v) : NULL); /* invalid: default */
            break;
        }
        default:
            prefs_set(t->key, t->need_value && !t->value[0] ? NULL : cJSON_CreateString(t->value));
            break;
        }
    }
    mt_log("[settings] saved, %d changed", n);
    settings_close();
    if (n && act.restart) act.restart();
}

static void activate(int dir) {
    item *t = &items[sel];
    switch (t->type) {
    case T_CHOICE: cycle(t, dir ? dir : 1); break;
    case T_TEXT:
    case T_NUMBER:
    case T_PASSWORD:
        if (!dir) edit_start(sel);
        return;
    case T_FIND:
        if (!dir && act.find && !searching) {
            searching = 1;
            act.find();
        } else if (dir && nservers > 0) {
            srv_sel = (srv_sel + dir + nservers) % nservers;
            apply_server(srv_sel);
        }
        break;
    case T_SAVE:
        if (!dir) save();
        return;
    case T_CANCEL:
        if (!dir) settings_close();
        return;
    default: return;
    }
    render();
}

static void move(int d) {
    int i = sel;
    do {
        i += d;
    } while (i >= 0 && i < nitems && !selectable(i));
    if (selectable(i)) sel = i;
    render();
}

/* ---------------- public ---------------- */

void settings_init(const settings_actions *a) {
    act = *a;
    root = ui_box(lv_screen_active(), 0x000000, OPA(0.95), 0);
    lv_obj_set_size(root, 1920, 1080);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lbl(root, 34, 1, UI_BLUE, LV_OPA_COVER);
    lv_label_set_text(t, _("Settings"));
    lv_obj_set_pos(t, LIST_X, (HDR_H - lv_font_get_line_height(ui_font(34, 1))) / 2);
    list = ui_box(root, 0, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(list, LIST_X, HDR_H);
    lv_obj_set_size(list, LIST_W, 1080 - HDR_H - FOOT_H);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    foot = lbl(root, 20, 0, UI_TEXT, OPA(0.55));
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -24);
    /* why the page opened by itself (no server, login refused) */
    note = lbl(root, 24, 0, UI_YELLOW, LV_OPA_COVER);
    lv_obj_set_width(note, LIST_W - 420);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_RIGHT, 0);
    ui_label_dots(note);
    lv_obj_align(note, LV_ALIGN_TOP_RIGHT, -LIST_X, (HDR_H - lv_font_get_line_height(ui_font(24, 0))) / 2);
    build_osk();
    show(root, 0);
}

void settings_open(void) { settings_open_at(NULL, NULL); }

void settings_open_at(const char *key, const char *text) {
    build_items();
    build_rows();
    /* a result that came while the page was closed goes into the fields;
     * an older one is forgotten (the fields show the stored values again) */
    if (apply_pending && nservers > 0) apply_server(srv_sel);
    else if (!apply_pending) nservers = -1;
    apply_pending = 0;
    sel = 0;
    move(1);
    for (int i = 0; key && i < nitems; i++)
        if (items[i].key && !strcmp(items[i].key, key) && selectable(i)) sel = i;
    editing = -1;
    show(osk, 0);
    lv_label_set_text(note, text ? text : "");
    show(note, text && *text);
    show(root, 1);
    lv_obj_move_foreground(root);
    render();
    mt_log("[settings] open%s%s", key ? " at " : "", key ? key : "");
}

void settings_servers(const settings_server *s, int n) {
    if (n > MAX_SERVERS) n = MAX_SERVERS;
    if (n < 0) n = 0;
    memcpy(servers, s, (size_t) n * sizeof(*s));
    nservers = n;
    srv_sel = 0;
    searching = 0;
    if (!settings_visible()) {
        apply_pending = 1;
        return;
    }
    if (n > 0) apply_server(0);
    render();
}

void settings_close(void) {
    if (!settings_visible()) return;
    editing = -1;
    show(osk, 0);
    show(root, 0);
    mt_log("[settings] closed");
}

int settings_visible(void) { return !lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN); }

int settings_key(int key) {
    if (!settings_visible()) return 0;
    if (editing >= 0) return osk_key(key);
    switch (key) {
    case KEY_UP: move(-1); break;
    case KEY_DOWN: move(1); break;
    case KEY_LEFT: activate(-1); break;
    case KEY_RIGHT: activate(1); break;
    case KEY_OK: activate(0); break;
    case KEY_BACK:
    case KEY_EXIT: settings_close(); break;
    default: break;
    }
    return 1;
}

int settings_pointer(int ev, int x, int y) {
    if (!settings_visible()) return 0;
    if (editing >= 0) {
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 10; c++)
                if (osk_keys[r][c] && ui_hit(osk_keys[r][c], x, y)) {
                    kr = r;
                    kc = c;
                    if (ev == PTR_UP) press_key(r, c);
                    else osk_render();
                    return 1;
                }
        return 1;
    }
    for (int i = 0; i < nitems; i++) {
        if (!selectable(i) || !items[i].row || !ui_hit(items[i].row, x, y)) continue;
        if (sel != i) {
            sel = i;
            render();
        }
        if (ev == PTR_UP) activate(0);
        return 1;
    }
    return 1;
}

void settings_wheel(int dir) {
    if (!settings_visible() || editing >= 0) return;
    move(dir > 0 ? -1 : 1);
}
