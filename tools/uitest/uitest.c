/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC test of the TVF UI (tools/uitest/build.sh): LVGL renders into a buffer that is saved as
 * PNG (over a stand-in "video"). Keys from argv.
 *   uitest <res> <grid.json> <out-prefix> [step ...]
 * Step: key name (UP DOWN LEFT RIGHT OK BACK RED GREEN YELLOW BLUE CH+ CH-),
 * "open", "osd", "settings", "shot" (write PNG), D0..D9 digits. */
#include "choice.h"
#include "epg.h"
#include "recdlg.h"
#include "recbrowser.h"
#include "settings.h"
#include "keys.h"
#include "log.h"
#include "osd.h"
#include "ui.h"
#include "lodepng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 1920
#define H 1080
static uint8_t *fb;
static int shots;
static const char *prefix;

static uint32_t tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) { (void) a; (void) px; lv_display_flush_ready(d); }
static void req(long s, long d) { printf("-> grid request start=%ld dur=%ld\n", s, d); }
static void tune(int ch) { printf("-> tune %d\n", ch); }
static void rec(int ch, double ev) { printf("-> record %d %.0f\n", ch, ev); }
static void restart(void) { printf("-> restart\n"); }
static void current(const char *key, char *out, size_t n) {
    static const char *const v[][2] = {
        {"tvh.host", "192.0.2.10"},    {"tvh.http_port", "9981"}, {"tvh.htsp_port", "9982"},
        {"tvh.user", "tvf"},           {"tvh.pass", "example-pass"},
        {"hbbtv.egress", "rules"},
        {"ui.lang", "auto"},
        {"kore.name", "TVF"},
        {"kore.http_port", "8080"}, {"kore.tcp_port", "0"}};
    out[0] = 0;
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        if (!strcmp(v[i][0], key)) snprintf(out, n, "%s", v[i][1]);
}

static void pump(int ms) {
    uint32_t end = tick() + (uint32_t) ms;
    while ((int32_t) (end - tick()) > 0) {
        ui_poll();
        lv_timer_handler();
        usleep(5000);
    }
}

static void shot(void) {
    lv_refr_now(NULL);
    uint8_t *rgba = malloc((size_t) W * H * 4);
    for (size_t i = 0; i < (size_t) W * H; i++) {
        /* stand-in video: gradient, the UI on top of it (ARGB8888 = B,G,R,A) */
        int x = (int) (i % W), y = (int) (i / W);
        int vr = 40 + x * 120 / W, vg = 70 + y * 100 / H, vb = 110;
        int a = fb[i * 4 + 3];
        rgba[i * 4 + 0] = (uint8_t) ((fb[i * 4 + 2] * a + vr * (255 - a)) / 255);
        rgba[i * 4 + 1] = (uint8_t) ((fb[i * 4 + 1] * a + vg * (255 - a)) / 255);
        rgba[i * 4 + 2] = (uint8_t) ((fb[i * 4 + 0] * a + vb * (255 - a)) / 255);
        rgba[i * 4 + 3] = 255;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s%02d.png", prefix, shots++);
    unsigned char *png = NULL;
    size_t plen = 0;
    unsigned err = lodepng_encode32(&png, &plen, rgba, W, H);
    FILE *f = err ? NULL : fopen(path, "wb");
    if (f) {
        fwrite(png, 1, plen, f);
        fclose(f);
    }
    printf("PNG %s %s\n", path, err ? lodepng_error_text(err) : "ok");
    lv_free(png);
    free(rgba);
}

static int keyval(const char *s) {
    static const struct { const char *n; int k; } m[] = {
        {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT}, {"OK", KEY_OK},
        {"BACK", KEY_BACK}, {"RED", KEY_RED}, {"GREEN", KEY_GREEN}, {"YELLOW", KEY_YELLOW},
        {"BLUE", KEY_BLUE}, {"CH+", KEY_CH_UP}, {"CH-", KEY_CH_DOWN}, {"INFO", KEY_INFO}};
    if (s[0] == 'D' && s[1] >= '0' && s[1] <= '9' && !s[2]) return KEY_DIGIT0 + (s[1] - '0');
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++)
        if (!strcmp(m[i].n, s)) return m[i].k;
    return KEY_NONE;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    prefix = argv[3];
    lv_init();
    lv_tick_set_cb(tick);
    fb = calloc((size_t) W * H, 4);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(d, fb, NULL, W * H * 4, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, 0);
    ui_res_init(argv[1], NULL);
    osd_init();
    epg_actions a = {req, tune, rec};
    epg_init(&a);
    recdlg_actions ra = {NULL, NULL};
    recdlg_init(&ra);
    recbrowser_actions rba = {NULL, NULL, NULL, NULL};
    recbrowser_init(&rba);
    choice_init();
    settings_actions sta = {restart, current};
    settings_init(&sta);

    FILE *f = fopen(argv[2], "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t) n + 1);
    fread(buf, 1, (size_t) n, f);
    buf[n] = 0;
    fclose(f);
    cJSON *msg = cJSON_Parse(buf);
    int cur = (int) cJSON_GetObjectItem(msg, "currentChannel")->valuedouble;

    for (int i = 4; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "open")) {
            epg_open(cur);
            uint32_t t0 = tick();
            epg_on_grid(cJSON_Duplicate(msg, 1));
            printf("epg_on_grid %u ms\n", tick() - t0);
        } else if (!strcmp(s, "choice")) {
            const char *l[] = {"Deutsch (Dolby Digital)", "Englisch (Dolby Digital)", "Audiodeskription (Stereo)"};
            choice_open("Tonspur", l, 3, 0, 0, NULL, NULL);
        } else if (!strcmp(s, "recs")) {
            FILE *rf = fopen("recs.json", "rb");
            static char rb[4 * 1024 * 1024];
            size_t rn = fread(rb, 1, sizeof(rb) - 1, rf);
            fclose(rf);
            rb[rn] = 0;
            cJSON *rj = cJSON_Parse(rb);
            recbrowser_open(NULL);
            recbrowser_on_recordings(cJSON_DetachItemFromObject(rj, "recordings"));
            recbrowser_on_timers(cJSON_DetachItemFromObject(rj, "timers"));
            cJSON_Delete(rj);
        } else if (!strcmp(s, "rec")) {
            cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItem(msg, "channels"), 1);
            cJSON *m = cJSON_CreateObject();
            cJSON_AddNumberToObject(m, "channel", cJSON_GetObjectItem(ch, "id")->valuedouble);
            cJSON_AddStringToObject(m, "name", cJSON_GetObjectItem(ch, "name")->valuestring);
            cJSON_AddStringToObject(m, "icon", cJSON_GetObjectItem(ch, "icon")->valuestring);
            cJSON_AddItemToObject(m, "broadcasts", cJSON_Duplicate(cJSON_GetObjectItem(ch, "broadcasts"), 1));
            recdlg_open((int) cJSON_GetObjectItem(ch, "id")->valuedouble, 0);
            recdlg_on_schedule(m);
        } else if (!strcmp(s, "settings")) {
            settings_open();
        } else if (!strcmp(s, "toast")) {
            osd_message("Englisch (Dolby Digital)", 3000, 0);
        } else if (!strcmp(s, "osd")) {
            osd_set_channel(210, "Sky Documentaries", "");
            epg_entry nw, nx;
            memset(&nw, 0, sizeof(nw));
            memset(&nx, 0, sizeof(nx));
            snprintf(nw.title, sizeof(nw.title), "Life Below Zero - \xC3\x9C" "berleben in Alaska - Die gro\xC3\x9F" "e Sonderfolge zum Winteranfang");
            snprintf(nw.subtitle, sizeof(nw.subtitle), "Ende der Finsternis");
            snprintf(nw.plot, sizeof(nw.plot), "Ende der Finsternis - 2. Staffel, Folge 12: Der Fr\xC3\xBC" "hling naht, doch in Kaktovik "
                     "liegt noch meterhoch Schnee. Sue Aikens muss ihr Camp gegen hungrige B\xC3\xA4ren verteidigen, w\xC3\xA4hrend "
                     "Andy und Kate Bassich ihre Hundeschlitten f\xC3\xBC" "r die letzte gro\xC3\x9F" "e Tour vorbereiten. Glenn Villeneuve "
                     "zieht mit dem Boot los, um Vorr\xC3\xA4te zu holen.");
            nw.start = time(NULL) - 1200;
            nw.stop = time(NULL) + 1800;
            snprintf(nx.title, sizeof(nx.title), "Life Below Zero - \xC3\x9C" "berleben in Alaska");
            nx.start = nw.stop;
            nx.stop = nw.stop + 3000;
            osd_set_nownext(&nw, &nx);
            const char *au[] = {"Deutsch (Dolby Digital)", "Englisch (Dolby Digital)"};
            osd_set_streams("HD \xC2\xB7 H.264 \xC2\xB7 1920\xC3\x97" "1080 \xC2\xB7 25 fps", au, 2, 0);
            osd_show(0);
        } else if (s[0] == 'p' && (s[1] == 'm' || s[1] == 'd' || s[1] == 'u') && s[2] == ':') {
            /* pointer: pm:x:y (move), pd:x:y (OK down), pu:x:y (OK up) */
            int x = 0, y = 0;
            sscanf(s + 3, "%d:%d", &x, &y);
            int ev = s[1] == 'm' ? PTR_MOVE : s[1] == 'd' ? PTR_DOWN : PTR_UP;
            if (settings_visible()) settings_pointer(ev, x, y);
            else if (choice_visible()) choice_pointer(ev, x, y);
            else if (recdlg_visible()) recdlg_pointer(ev, x, y);
            else if (recbrowser_visible()) recbrowser_pointer(ev, x, y);
            else epg_pointer(ev, x, y);
            printf("pointer %s\n", s);
        } else if (!strcmp(s, "shot")) {
            pump(1500);
            shot();
        } else {
            uint32_t t0 = tick();
            int k = keyval(s);
            if (!(settings_visible() && settings_key(k)) && !(choice_visible() && choice_key(k)) && !(recdlg_visible() && recdlg_key(k)) &&
                !(recbrowser_visible() && recbrowser_key(k)))
                epg_key(k);
            lv_refr_now(NULL);
            printf("key %s: %u ms\n", s, tick() - t0);
        }
    }
    return 0;
}
