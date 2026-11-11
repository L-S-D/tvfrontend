/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * TVF (TV Frontend) - native webOS client for TVHeadend.
 *
 * Channels, EPG, recordings and the live picture come directly from tvh via HTSP
 * (tvsrc.c, tvrec.c); the picture is played by our own player (src/player,
 * Starfish BUFFERSTREAM/LIVE), the UI is LVGL. Kore and Home
 * Assistant control TVF via its Kodi JSON-RPC (kodi.c).
 *
 * Launch (development):
 *   ares-launch -d <tv> com.lsd.tvf -p '{"log":"<pc>:5555","stats":2000}'
 * Keys: "log":"host:port" (UDP log), "debug":port (debug port),
 * player tuning knobs see player.c. The basic settings (tvh host,
 * credentials, extra services, language ...) live in the settings file and on
 * the settings page (Green long); launch parameters for them are remembered.
 */
#include "i18n.h"
#include "cJSON.h"
#include "choice.h"
#include "debugctl.h"
#include "epg.h"
#include "fetch.h"
#include "hbbtv.h"
#include "egress.h"
#include "htsp.h"
#include "hbbdrm.h"
#include "hbbmedia.h"
#include "webmedia.h"
#include "cdmtest.h"
#include "hbbweb.h"
#include "webr.h"
#include "http.h"
#include "keys_sdl.h"
#include "kodi.h"
#include "log.h"
#include "luna.h"
#include "mdns.h"
#include "osd.h"
#include "player.h"
#include "prefs.h"
#include "recbrowser.h"
#include "relay.h"
#include "recdlg.h"
#include "recplay.h"
#include "settings.h"
#include "subs.h"
#include "sidedata.h"
#include "pmtinfo.h"
#include "tshift.h"
#include "ttx.h"
#include "info.h"
#include "tvhstat.h"
#include "tvsrc.h"
#include "ui_sdl.h"

#include <SDL.h>
#include <pthread.h>
#include <SDL_webOS.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <execinfo.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SCREEN_W 1920
#define SCREEN_H 1080
#define DIGIT_TIMEOUT_MS 2000 /* like the HTML5 app */
#define DIGIT_LONG_MS 600     /* held longer: webOS quick access, not a digit */
#define SCANCODE_RED_LONG 453 /* webOS: Red held long (instead of the red key) */
#define COLOR_LONG_MS 600     /* colour key held this long with HbbTV open: TVF instead of the app */
#define OK_LONG_MS 600        /* OK held this long in the live picture: info window for the event */
#define CLICK_DEDUP_MS 150
#define OSD_ZAP_MS 6000
#define HBB_PRELOAD_MS 5000 /* this long on a channel: preload its HbbTV app */
/* the page's stop() takes effect this late: Anixe calls stop() and
 * bindToCurrentChannel() 40-70 ms apart whenever it moves the picture -
 * stopping and restarting the subscription each time was ~0.4 s without picture */
#define HBB_BC_OFF_MS 500
#define OSD_INFO_MS 8000
#define STALL_MS 10000     /* this long without a new frame although the stream is flowing: reconnect */
#define RETRY_HINT_MS 3000 /* this long without a reply from tvh: hint "not responding" */

typedef struct {
    player *pl;
    int channel;
    char url[1024];
    char digits[8];
    uint32_t digit_at;
    int held_digit;      /* pressed digit key (SDL keycode), 0 = none */
    uint32_t held_at;
    int held_long;
    int held_color;      /* colour key pressed with HbbTV open (KEY_*), 0 = none */
    uint32_t held_color_at;
    int held_color_long; /* already handled as long (repeat) */
    int held_ok;         /* OK pressed in the live picture (short/long undecided) */
    uint32_t held_ok_at;
    int held_ok_long;    /* already handled as long */
    int key_to_tvf;      /* on_key: key belongs to TVF, not to the HbbTV app */
    long now_stop;       /* end of the current event (Unix s), 0 = unknown */
    int nn_requested;    /* now/next for now_stop already requested */
    int background;      /* playback stopped because of background */
    player_track menu_tracks[PLAYER_MAX_TRACKS]; /* audio tracks of the open selection */
    int menu_ntracks, menu_cur;
    int side_local; /* HTSP subscription: teletext, subtitles, AIT from sidedata.c (IPTV: none of these) */
    uint32_t hbb_pre_at; /* preload the channel's HbbTV app from then on (dwelling), 0 = done */
    int hbb_auto_done;   /* "HbbTV first": channel's app already started automatically */
    /* Magic Remote: pointer visible (webOS reports show/hide as a
     * scancode). Then the click at the pointer position counts; an OK key that
     * belongs to the same press (within CLICK_DEDUP_MS) is dropped. */
    int timeshift; /* launch parameter "timeshift" (default 1) */
    uint32_t retry_at; /* broken live stream: new attempt */
    int retries;
    char chname[64];      /* name of the current channel (for hints) */
    char icon[512];       /* logo URL of the current channel (info window) */
    char retry_text[160]; /* own hint for the retry, "" = none */
    int stalled;          /* retry because of a frozen picture (data kept coming) */
    int hbbtv_live_off;   /* live picture stopped: HbbTV media playing instead of live */
    int hbb_media_on;     /* ... because the page's media are playing */
    int hbb_bc_off;       /* ... because the page stopped the TV picture */
    uint32_t hbb_bc_off_at; /* the page's stop() pending until then (HBB_BC_OFF_MS), 0 = none */
    int hbb_prestarted;
    int pointer;
    uint32_t mouse_at, ok_pending;
    int wheel_logged, click_logged, motion_logged;
    uint32_t streams_at;
} app_state;

static app_state app;
static volatile sig_atomic_t g_quit;
static int g_restart; /* settings saved: restart after cleaning up */
/* key layout "HbbTV first" (keys.layout = hbbtv): the channel's HbbTV app
 * starts visibly, short colour keys belong to it, long ones to TVF; settings
 * with Red long when HbbTV is not visible. Otherwise "Standard" (classic) */
static int g_keys_hbbtv;
/* tvh server search (mDNS): running; started at the start because no server
 * is set (then the result decides); the settings opened once for the login */
static int g_finding, g_find_first, g_noaccess_shown;
/* test: tvh server and login of this run only, not saved (launch parameter
 * "tvhTest": {"host", "user", "pass"}) */
static cJSON *g_tvh_test;

static void on_signal(int s) {
    (void) s;
    g_quit = 1;
}

/* Crash: location to the log (also via UDP) - we cannot get at the TVs'
 * system logs. pc/lr including the library from /proc/self/maps; addresses in
 * tvf (no PIE) are resolved with addr2line on the PC. */
static void on_crash(int sig, siginfo_t *si, void *ucv) {
    unsigned long pc = 0, lr = 0;
#ifdef __arm__
    const ucontext_t *uc = ucv;
    pc = uc->uc_mcontext.arm_pc;
    lr = uc->uc_mcontext.arm_lr;
#else
    (void) ucv;
#endif
    mt_log("[app] CRASH: signal %d, address %p, pc %08lx, lr %08lx", sig, si ? si->si_addr : NULL, pc, lr);
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            unsigned long a, b, off;
            char path[300] = "";
            if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %299s", &a, &b, &off, path) < 3) continue;
            int in_pc = pc >= a && pc < b, in_lr = lr >= a && lr < b;
            if (in_pc) mt_log("[app]   pc in %s (offset %08lx)", path[0] ? path : "?", pc - a + off);
            if (in_lr) mt_log("[app]   lr in %s (offset %08lx)", path[0] ? path : "?", lr - a + off);
        }
        fclose(f);
    }
    void *bt[32];
    int n = backtrace(bt, 32);
    char **s = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++) mt_log("[app]   #%d %s", i, s ? s[i] : "?");
    raise(sig); /* SA_RESETHAND: now with the normal effect */
}

static void crash_handler_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}

/* ---------------- Messages from tvsrc/tvrec ---------------- */

#if TVF_PMT_FALLBACK
/* the app thread's side of the PMT fallback: the player found no HTSP details in the stream list;
 * the lookup (thread) hands the PMT back, to be merged into the session's tracks */
static void on_pmt_request(void *user, unsigned long channel) {
    (void) user;
    pmtinfo_lookup(channel);
}

static void on_pmtinfo(unsigned long channel, const ts_pmt *pmt) {
    for (int i = 0; i < pmt->n; i++)
        if (pmt->es[i].codec == ES_TELETEXT && pmt->es[i].ttx_initial) {
            sidedata_set_ttx_initial(pmt->es[i].ttx_initial);
            break;
        }
    player_merge_pmt(app.pl, channel, pmt);
}
#endif

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : 0;
}

/* ---------------- Audio tracks ---------------- */

/* audio track: codes with a special meaning in broadcasting, otherwise the language name */
static const char *lang_name(const char *l) {
    static const struct {
        const char *code, *name;
    } m[] = {{"mul", N_("Original audio")}, {"qaa", N_("Original audio")}, {"qks", N_("Clear speech")},
             {"mis", N_("Audio description")}};
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++)
        if (!strcmp(m[i].code, l)) return _(m[i].name);
    return i18n_iso639(l);
}

/* Audio track for the selection: the kind (audio_type: audio description, clear
 * speech or for the hearing impaired) takes precedence over the language - tvh takes both from
 * the supplementary audio descriptor (ORF: "qks" arrives as German with kind 2). A
 * supplementary track for mixing (receiver mix) is on its own only the narrator's voice. */
static void track_label(const player_track *all, int count, int i, char *out, size_t n) {
    const player_track *t = &all[i];
    static const struct {
        const char *code, *name;
    } c[] = {{"ac3", "Dolby Digital"}, {"eac3", "Dolby Digital Plus"}, {"mp2", "Stereo"},
             {"aac", "AAC"},           {"latm", "AAC"},                {"dts", "DTS"}};
    char name[64];
    (void) count;
    const char *lang = t->lang;
    const char *ln = lang_name(lang);
    if (t->type == 3) snprintf(name, sizeof(name), "%s", _("Audio description"));
    else if (t->type == 2) snprintf(name, sizeof(name), "%s", _("Clear speech"));
    else if (ln) snprintf(name, sizeof(name), "%s", ln);
    else if (lang[0]) snprintf(name, sizeof(name), "%s", lang);
    else snprintf(name, sizeof(name), _("Track %d"), i + 1);
    if (t->receiver_mix) {
        size_t l = strlen(name);
        snprintf(name + l, sizeof(name) - l, "%s", _(" (voice only)"));
    }
    const char *cn = NULL;
    for (size_t k = 0; k < sizeof(c) / sizeof(c[0]); k++)
        if (!strcmp(c[k].code, t->codec)) cn = c[k].name;
    if (cn) snprintf(out, n, "%s (%s)", name, cn);
    else snprintf(out, n, "%s", name);
}

/* remembered choice for the channel (language, kind, codec) for the next starts */
static void apply_audio_pref(int ch) {
    char key[32];
    snprintf(key, sizeof(key), "audio.%d", ch);
    const cJSON *p = prefs_get(key);
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(p, "lang"), *t = cJSON_GetObjectItemCaseSensitive(p, "type"),
                *c = cJSON_GetObjectItemCaseSensitive(p, "codec");
    player_set_audio_pref(app.pl, cJSON_IsString(l) ? l->valuestring : NULL, cJSON_IsNumber(t) ? t->valueint : 0,
                          cJSON_IsString(c) ? c->valuestring : NULL);
}

/* Sound offset to the picture (like the subtitle offset): + = sound later,
 * remembered per channel, without its own value the last one set applies.
 * Adjusted with Left/Right (25 ms), OK/Back done; stored when done (the
 * settings file is written with fsync: not with every key). Recordings: channel 0 */
#define AOFF_STEP 25
#define AOFF_MAX 2000
static int aoff_ms, aoff_channel, aoff_adjusting, aoff_dirty;

/* the offset of the channel and as the last one set; clear: remove both */
static void aoff_store(int ch, int ms, int clear) {
    char key[40];
    snprintf(key, sizeof(key), "audio.offset.%d", ch);
    prefs_set(key, clear ? NULL : cJSON_CreateNumber(ms));
    prefs_set("audio.offset", clear ? NULL : cJSON_CreateNumber(ms));
}

static void aoff_done(void) {
    if (aoff_dirty) aoff_store(aoff_channel, aoff_ms, 0);
    aoff_dirty = 0;
    if (aoff_adjusting) {
        aoff_adjusting = 0;
        osd_message(NULL, 0, 0);
    }
}

static void aoff_load(int ch) {
    aoff_done();
    char key[40];
    snprintf(key, sizeof(key), "audio.offset.%d", ch);
    const cJSON *v = prefs_get(key);
    if (!cJSON_IsNumber(v)) v = prefs_get("audio.offset");
    aoff_channel = ch;
    aoff_ms = cJSON_IsNumber(v) ? v->valueint : 0;
    player_set_audio_offset(app.pl, aoff_ms);
}

static void aoff_label(char *out, size_t n) {
    int a = aoff_ms < 0 ? -aoff_ms : aoff_ms;
    snprintf(out, n, "%s%d ms", aoff_ms > 0 ? "+" : aoff_ms < 0 ? "\xE2\x88\x92" : "\xC2\xB1", a);
}

static void aoff_show(void) {
    char l[32], t[160];
    aoff_label(l, sizeof(l));
    snprintf(t, sizeof(t), _("Audio offset %s     Left earlier \xC2\xB7 Right later \xC2\xB7 OK done"), l);
    osd_message(t, 0, 0);
}

static int aoff_key(int key) {
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        aoff_ms += key == KEY_RIGHT ? AOFF_STEP : -AOFF_STEP;
        if (aoff_ms > AOFF_MAX) aoff_ms = AOFF_MAX;
        if (aoff_ms < -AOFF_MAX) aoff_ms = -AOFF_MAX;
        aoff_dirty = 1;
        player_set_audio_offset(app.pl, aoff_ms);
        aoff_show();
    } else if (key == KEY_OK || key == KEY_BACK) {
        aoff_done();
    }
    return 1;
}

/* Yellow selection: first the audio tracks (from two on) and the sound offset,
 * then "Subtitles off" and the channel's subtitles (like audio.openMenu of the
 * HTML5 app) */
static subs_option menu_subs[SUBS_MAX];
static int menu_nsubs, menu_naudio_shown, menu_offset_idx = -1, menu_aoff_idx = -1, menu_subs_base;

static void on_audio_pick(void *user, int i) {
    (void) user;
    if (i == menu_offset_idx) {
        subs_adjust_start();
        return;
    }
    if (i == menu_aoff_idx) {
        aoff_adjusting = 1;
        aoff_show();
        return;
    }
    if (i >= menu_subs_base) { /* subtitles */
        int k = i - menu_subs_base;
        if (k == 0) {
            subs_choose(NULL);
            osd_message(_("Subtitles off"), 3000, 0);
        } else if (k - 1 < menu_nsubs) {
            subs_choose(menu_subs[k - 1].id);
            osd_message(menu_subs[k - 1].label, 3000, 0);
        }
        return;
    }
    if (i < 0 || i >= app.menu_ntracks) return;
    const player_track *t = &app.menu_tracks[i];
    char label[96];
    track_label(app.menu_tracks, app.menu_ntracks, i, label, sizeof(label));
    osd_message(label, 3000, 0);
    if (i == app.menu_cur) return;
    if (recplay_active()) { /* recording: only for this playback */
        recplay_select_audio(t->pid);
        return;
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "lang", t->lang);
    cJSON_AddNumberToObject(p, "type", t->type);
    cJSON_AddStringToObject(p, "codec", t->codec);
    char key[32];
    snprintf(key, sizeof(key), "audio.%d", app.channel);
    prefs_set(key, p);
    apply_audio_pref(app.channel);
    mt_log("[app] audio track %s (PID %d) for channel %d", label, t->pid, app.channel);
    player_select_audio(app.pl, t->pid);
}

/* stream info in the banner: video (codec, size and frame rate from the player)
 * and audio tracks, the current one highlighted */
static void update_streams(void) {
    player_status st;
    player_status_get(app.pl, &st);
    int w = st.video_w, h = st.video_h;
    const char *codec = NULL;
    if (st.vcodec[0] && strcmp(st.vcodec, "-")) codec = !strcmp(st.vcodec, "MPEG-2 Video") ? "MPEG-2" : st.vcodec;
    char v[128] = "";
    size_t o = 0;
#define ADD(...)                                                                                                       \
    do {                                                                                                               \
        if (o < sizeof(v)) o += (size_t) snprintf(v + o, sizeof(v) - o, __VA_ARGS__);                                \
    } while (0)
    if (w && h) ADD("%s", w >= 3800 ? "UHD" : (w >= 1280 || h >= 720) ? "HD" : "SD");
    if (codec) ADD("%s%s", o ? " \xC2\xB7 " : "", codec);
    if (w && h) ADD("%s%d\xC3\x97%d", o ? " \xC2\xB7 " : "", w, h);
    /* frame rate from the decoder (VIDEO_INFO) */
    double fps = st.video_fps;
    if (fps > 0) {
        char f[16];
        snprintf(f, sizeof(f), "%g", fps);
        char *dot = strchr(f, '.');
        if (dot) *dot = i18n_decimal();
        ADD("%s%s fps", o ? " \xC2\xB7 " : "", f);
    }
#undef ADD
    player_track tr[PLAYER_MAX_TRACKS];
    int cur;
    int n = player_audio_tracks(app.pl, tr, PLAYER_MAX_TRACKS, &cur);
    char labels[PLAYER_MAX_TRACKS][96];
    const char *lp[PLAYER_MAX_TRACKS];
    for (int i = 0; i < n; i++) {
        track_label(tr, n, i, labels[i], sizeof(labels[i]));
        lp[i] = labels[i];
    }
    osd_set_streams(v, lp, n, cur);
}

static void open_audio_menu(void) {
    app.menu_ntracks = player_audio_tracks(app.pl, app.menu_tracks, PLAYER_MAX_TRACKS, &app.menu_cur);
    int cur_sub = -1;
    menu_nsubs = recplay_active() ? 0 : subs_options(menu_subs, SUBS_MAX, &cur_sub);
    if (!app.menu_ntracks && !menu_nsubs) {
        osd_message(_("No audio tracks"), 3000, 0);
        return;
    }
    char labels[PLAYER_MAX_TRACKS + SUBS_MAX + 2][96];
    const char *lp[PLAYER_MAX_TRACKS + SUBS_MAX + 2];
    int marks[PLAYER_MAX_TRACKS + SUBS_MAX + 2] = {0};
    int n = 0;
    menu_naudio_shown = app.menu_ntracks > 1 ? app.menu_ntracks : 0;
    for (int i = 0; i < menu_naudio_shown; i++, n++) {
        track_label(app.menu_tracks, app.menu_ntracks, i, labels[n], sizeof(labels[n]));
        marks[n] = i == app.menu_cur;
    }
    menu_aoff_idx = -1;
    if (app.menu_ntracks) {
        char ol[32];
        aoff_label(ol, sizeof(ol));
        snprintf(labels[n], sizeof(labels[n]), _("Audio offset: %s"), ol);
        menu_aoff_idx = n++;
    }
    menu_subs_base = n;
    menu_offset_idx = -1;
    if (menu_nsubs) {
        snprintf(labels[n], sizeof(labels[n]), "%s", _("Subtitles off"));
        marks[n++] = cur_sub < 0;
        for (int k = 0; k < menu_nsubs; k++, n++) {
            snprintf(labels[n], sizeof(labels[n]), "%s", menu_subs[k].label);
            marks[n] = k == cur_sub;
        }
        if (subs_enabled()) {
            char ol[32];
            subs_offset_label(ol, sizeof(ol));
            snprintf(labels[n], sizeof(labels[n]), _("Subtitle offset: %s"), ol);
            menu_offset_idx = n++;
        }
    }
    for (int i = 0; i < n; i++) lp[i] = labels[i];
    osd_hide();
    int sel = menu_naudio_shown && app.menu_cur >= 0 ? app.menu_cur : 0;
    choice_open_multi(app.menu_ntracks ? (menu_nsubs ? _("Audio / subtitles") : _("Audio track")) : _("Subtitles"), lp, n,
                      marks, sel, on_audio_pick, NULL);
}

/* ---------------- Hint while the live stream is not running ---------------- */

/* remove our own hint; another message that replaced it stays */
static void retry_clear(void) {
    if (!app.retry_text[0]) return;
    if (osd_message_is(app.retry_text)) osd_message(NULL, 0, 0);
    app.retry_text[0] = 0;
}

/* Channel not responding, not available or broken off: hint with a
 * countdown to the next attempt, during the attempt "retrying",
 * on the first attempt already after RETRY_HINT_MS without a reply.
 * Gone as soon as a frame arrives. */
static void retry_status(const player_status *rs, uint32_t nowt) {
    const char *name = app.chname[0] ? app.chname : _("Channel");
    char msg[sizeof(app.retry_text)] = "";
    if (app.retry_at) {
        int left = ((int32_t) (app.retry_at - nowt) + 999) / 1000;
        if (left < 1) left = 1;
        /* whole sentences per case: word order differs per language */
        if (app.stalled && rs->first_frame)
            snprintf(msg, sizeof(msg), _("%s: picture frozen \xE2\x80\x93 retrying in %d s"), name, left);
        else if (rs->first_frame)
            snprintf(msg, sizeof(msg), _("%s: connection lost \xE2\x80\x93 retrying in %d s"), name, left);
        else if (rs->http_status == 0)
            snprintf(msg, sizeof(msg), _("%s is not responding \xE2\x80\x93 retrying in %d s"), name, left);
        else if (rs->http_status >= 300)
            snprintf(msg, sizeof(msg), _("%s not available (HTTP %d) \xE2\x80\x93 retrying in %d s"), name,
                     rs->http_status, left);
        else snprintf(msg, sizeof(msg), _("%s delivers no picture \xE2\x80\x93 retrying in %d s"), name, left);
    } else if (app.retries > 0 && !rs->first_frame) {
        snprintf(msg, sizeof(msg), _("%s \xE2\x80\x93 retrying \xE2\x80\xA6"), name);
    } else if (rs->http_wait_ms >= RETRY_HINT_MS) {
        snprintf(msg, sizeof(msg), _("%s is not responding \xE2\x80\xA6"), name);
    }
    if (!strcmp(msg, app.retry_text)) return;
    if (!msg[0]) {
        retry_clear();
        return;
    }
    snprintf(app.retry_text, sizeof(app.retry_text), "%s", msg);
    osd_message(msg, 0, 1);
}

/* ---------------- Reception (tvh, banner below the logo) ---------------- */

static void num_de(char *out, size_t n, double v) {
    /* one decimal place, the language's decimal separator, a real minus sign */
    char t[32];
    snprintf(t, sizeof(t), "%.1f", v < 0 ? -v : v);
    char *d = strchr(t, '.');
    if (d) *d = i18n_decimal();
    snprintf(out, n, "%s%s", v < 0 ? "\xE2\x88\x92" : "", t);
}

static float frac(double v, double lo, double hi) {
    double f = (v - lo) / (hi - lo);
    return (float) (f < 0 ? 0 : f > 1 ? 1 : f);
}

/* the reception of the current channel, preformatted (banner, info window); 0 = none */
static int rx_build(const tvhstat_info *i, osd_rx *out) {
    if (!i->channel_id || i->channel_id != tvsrc_status_channel(app.channel)) return 0;
    osd_rx r;
    memset(&r, 0, sizeof(r));
    snprintf(r.provider, sizeof(r.provider), "%s", i->provider);
    char t[40];
    if (i->has_snr) {
        /* bars: SNR 0..20 dB, signal -90..-30 dBm (relative values directly) */
        if (i->snr_abs) {
            num_de(t, sizeof(t), i->snr / 1000.0);
            snprintf(r.snr, sizeof(r.snr), "%.16s dB", t);
            r.snr_frac = frac(i->snr / 1000.0, 0, 20);
        } else {
            snprintf(r.snr, sizeof(r.snr), "%d %%", (int) (i->snr * 100 / 65535));
            r.snr_frac = (float) i->snr / 65535;
        }
    }
    if (i->has_signal) {
        if (i->signal_abs) {
            num_de(t, sizeof(t), i->signal / 1000.0);
            char *c = strchr(t, i18n_decimal()); /* whole dBm are enough */
            if (c) *c = 0;
            snprintf(r.signal, sizeof(r.signal), "%.16s dBm", t);
            r.signal_frac = frac(i->signal / 1000.0, -90, -30);
        } else {
            snprintf(r.signal, sizeof(r.signal), "%d %%", (int) (i->signal * 100 / 65535));
            r.signal_frac = (float) i->signal / 65535;
        }
    }
    if (i->status[0]) {
        r.level = !strcmp(i->status, "GOOD") ? 1 : !strcmp(i->status, "FAINT") ? 2 : 3; /* BAD, NONE */
    }
    if (i->has_unc && i->unc) snprintf(r.errs, sizeof(r.errs), "UNC %u", i->unc);
    else if (i->has_ber && i->ber) snprintf(r.errs, sizeof(r.errs), "BER %u", i->ber);
    if (i->has_descr) {
        if (!i->caid && !i->ecmtime) {
            r.ca_failed = 1;
        } else {
            char cs[sizeof(i->cardsystem)];
            snprintf(cs, sizeof(cs), "%s", i->cardsystem[0] ? i->cardsystem : "CA");
            if (cs[0] >= 'a' && cs[0] <= 'z') cs[0] = (char) (cs[0] - 'a' + 'A'); /* cryptoworks -> Cryptoworks */
            snprintf(r.ca, sizeof(r.ca), "%.24s %04X", cs, i->caid);
            snprintf(r.ca_time, sizeof(r.ca_time), "%u ms", i->ecmtime);
            /* reader (OSCam), otherwise the path (newcamd, capmt ...) */
            if (i->reader[0]) snprintf(r.ca_via, sizeof(r.ca_via), "%.20s", i->reader);
            else if (i->protocol[0]) snprintf(r.ca_via, sizeof(r.ca_via), "via %.16s", i->protocol);
        }
    }
    snprintf(r.error, sizeof(r.error), "%.40s", i->error);
    *out = r;
    return 1;
}

static tvhstat_info rx_last; /* the last reception state (also for the info window) */

static void show_reception(const tvhstat_info *i) {
    osd_rx r;
    rx_last = *i;
    osd_set_rx(rx_build(i, &r) ? &r : NULL);
}

/* info window: reception (as in the banner, with a quality line) and DVB ids below the logo */
static void info_rx_update(void) {
    const tvhstat_info *i = &rx_last;
    osd_rx r;
    int have = rx_build(i, &r);
    if (have) {
        const char *q = !i->status[0]                 ? ""
                        : !strcmp(i->status, "GOOD")  ? _("Good")
                        : !strcmp(i->status, "FAINT") ? _("Weak")
                        : !strcmp(i->status, "BAD")   ? _("Bad")
                                                      : _("No signal");
        size_t o = (size_t) snprintf(r.quality, sizeof(r.quality), "%s", q);
        if (i->has_ber && o < sizeof(r.quality))
            o += (size_t) snprintf(r.quality + o, sizeof(r.quality) - o, "%sBER %u", o ? " \xC2\xB7 " : "", i->ber);
        if (i->has_unc && o < sizeof(r.quality))
            snprintf(r.quality + o, sizeof(r.quality) - o, "%sUNC %u", o ? " \xC2\xB7 " : "", i->unc);
    }
    char ids[48] = "";
    int onid, tsid, sid;
    if (player_dvb_ids(app.pl, &onid, &tsid, &sid))
        snprintf(ids, sizeof(ids), "%d \xC2\xB7 %d \xC2\xB7 %d", onid, tsid, sid);
    info_set_rx(have ? &r : NULL, ids);
}

static void reception_tick(void) {
    uint32_t want = 0;
    if (!app.background && !recplay_active() && app.url[0] && !app.hbbtv_live_off) {
        player_status st;
        player_status_get(app.pl, &st);
        if (st.first_frame) want = tvsrc_status_channel(app.channel);
    }
    tvhstat_watch(want);
    tvhstat_info ti;
    int changed = tvhstat_get(&ti);
    if (changed) show_reception(&ti);
    /* info window: with the reception, and once a second (the DVB ids of the PMT lookup come later) */
    static uint32_t info_at;
    if (info_visible() && (changed || SDL_GetTicks() - info_at >= 1000)) {
        info_at = SDL_GetTicks();
        info_rx_update();
    }
}

static void on_play(const tvsrc_play_info *p) {
    int ch = p->channel;
    const char *url = p->url;
    const char *name = p->name;
    int initial = p->initial;
    const epg_entry *now = &p->now, *next = &p->next;
    mt_log("[app] play %d \"%s\" %s%s", ch, name, url, initial ? " (initial)" : "");

    player_status st;
    player_status_get(app.pl, &st);
    /* reconnecting to the current channel: do not restart */
    if (initial && ch == app.channel && !strcmp(url, app.url) && st.active) {
        osd_set_nownext(now, next);
        app.now_stop = now->stop;
        app.nn_requested = 0;
        return;
    }
    if (ch != app.channel) hbbweb_close("channel change"); /* also a preloaded app */
    if (ch != app.channel) app.hbb_auto_done = 0;
    if (ch != app.channel || !app.hbb_pre_at) app.hbb_pre_at = SDL_GetTicks() + HBB_PRELOAD_MS;
    if (ch != app.channel) info_close(); /* was showing the old channel */
    app.channel = ch;
    snprintf(app.url, sizeof(app.url), "%s", url);
    snprintf(app.chname, sizeof(app.chname), "%s", name);
    /* new channel: retries of the old one, including the hint, are over */
    app.retries = 0;
    app.retry_at = 0;
    app.stalled = 0;
    retry_clear();
    /* teletext and subtitles from our own store (filled with whatever
     * tvh delivers in the HTSP subscription - live or timeshift); IPTV channels have none */
    int side_local = !strncmp(url, "htsp://", 7);
    app.side_local = side_local;
    sidedata_reset();
    ttx_set_source(side_local);
    subs_set_source(side_local, ch);
    if (ch != aoff_channel) aoff_load(ch);
    tshift_on_play(url, app.timeshift);
    app.now_stop = now->stop;
    app.nn_requested = 0;
    epg_close();
    osd_set_channel(ch, name, p->icon);
    snprintf(app.icon, sizeof(app.icon), "%s", p->icon ? p->icon : "");
    osd_set_nownext(now, next);
    osd_show(OSD_ZAP_MS);
    if (!*url) {
        osd_message(_("No stream for this channel"), 4000, 1);
        player_stop(app.pl);
        return;
    }
    apply_audio_pref(ch);
    if (!app.background) player_play(app.pl, url);
}

/* ---------------- From tvsrc/tvrec ---------------- */

static void app_play(const tvsrc_play_info *p) {
    if (recplay_active()) {
        /* tvh reconnected during a recording: keep playing, report the state */
        if (p->initial) {
            recplay_report();
            return;
        }
        recplay_exit();
    }
    recbrowser_close();
    on_play(p);
}

static void app_now_next(const epg_entry *now, const epg_entry *next) {
    osd_set_nownext(now, next);
    app.now_stop = now->stop;
    app.nn_requested = 0;
}

static void app_groups_changed(void) { epg_refresh(); }
static void app_grid(cJSON *grid) { epg_on_grid(grid); }
static void app_schedule(cJSON *schedule) { recdlg_on_schedule(schedule); }

static void app_connected(int up) {
    if (up) osd_message(NULL, 0, 0);
    else
        osd_message(g_keys_hbbtv ? _("No connection to the TVH server - hold red for settings")
                                 : _("No connection to the TVH server - hold green for settings"),
                    0, 1);
}

static void app_recordings(cJSON *list) {
    mt_log("[app] %d recordings", cJSON_GetArraySize(list));
    recbrowser_on_recordings(list);
}

static void app_timers(cJSON *list) {
    recbrowser_on_timers(list);
    epg_refresh();
}

static void app_play_recording(cJSON *item) {
    hbbweb_close("recording");
    tshift_stop();
    subs_set_source(0, 0);
    aoff_load(0);
    ttx_set_source(0);
    epg_close();
    recbrowser_close();
    recdlg_close();
    choice_close();
    recplay_start(item);
    cJSON_Delete(item);
}

static void app_timer_result(const char *action, int ok, const char *err) {
    char t[160];
    int failed = recdlg_result_text(action, ok, err, t, sizeof(t));
    osd_message(t, failed ? 5000 : 3000, failed);
}

/* ---------------- EPG actions ---------------- */

static void epg_request(long start, long duration) { tvsrc_grid(start, duration); }

/* Selecting the channel that is already playing (list, EPG, digits): no restart
 * as long as the live stream plays cleanly - only the banner (nothing with HbbTV
 * open). It is still restarted if it has broken off, has no picture yet,
 * or a recording or HbbTV media are currently playing instead of live. In timeshift
 * it stays time-shifted (back to live: Back) */
static int tune_is_noop(int channel) {
    if (channel != app.channel || !app.url[0] || recplay_active() || app.hbbtv_live_off) return 0;
    player_status st;
    player_status_get(app.pl, &st);
    if (!st.active || !st.first_frame || st.ended || app.retry_at || app.retries) return 0;
    mt_log("[app] channel %d already playing - no restart", channel);
    if (!hbbweb_active()) {
        tvsrc_refresh_now_next();
        osd_show(OSD_INFO_MS);
    }
    return 1;
}

static void tune(int channel) {
    if (!tune_is_noop(channel)) tvsrc_tune(channel);
}

static void epg_tune(int channel) { tune(channel); }

static void epg_record(int channel, double event_id) { recdlg_open(channel, event_id); }

/* ---------------- Recording dialog ---------------- */

static void rd_request(int channel) { tvsrc_schedule(channel); }

/* timer action for an event (EPG/schedule: id = event) or a
 * timer (recordings: eventId, timerId, autorecId); result: app_timer_result */
static void rd_action(const char *action, const cJSON *b, int channel) {
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(b, "eventId");
    if (!cJSON_IsNumber(ev)) ev = cJSON_GetObjectItemCaseSensitive(b, "id");
    uint32_t event = cJSON_IsNumber(ev) ? (uint32_t) ev->valuedouble : 0;
    uint32_t timer = (uint32_t) jnum(b, "timerId");
    mt_log("[app] Timer: %s event %u timer %u series %s channel %d", action, event, timer, jstr(b, "autorecId"),
           channel);
    tvrec_action(action, event, timer, jstr(b, "autorecId"), channel, NULL, NULL);
}

/* ---------------- Recordings ---------------- */

static void rb_request(void) {
    tvrec_request_list(0);
    tvrec_request_list(1);
}

static void rb_play(const char *id, int from_start) { tvrec_play_uuid(id, from_start); }

/* browser closed at the top level: if nothing is playing (recording finished),
 * back to live TV */
static void rb_closed(void) {
    player_status st;
    player_status_get(app.pl, &st);
    if (!recplay_active() && !st.active) tvsrc_tune_current();
}

static void rp_exited(const char *id) { recbrowser_open(id); }

static long long subs_now_pts(void) { return player_stream_pts(app.pl); }

static void ttx_zap(int dir) { tvsrc_step(dir > 0 ? 1 : -1); }

static uint32_t fetch_sdl_event = (uint32_t) -1;
static void fetch_wake(void) {
    if (fetch_sdl_event == (uint32_t) -1) return; /* still before fetch_init */
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = fetch_sdl_event;
    SDL_PushEvent(&e);
}

/* ---------------- Keys ---------------- */

static void tune_digits(void) {
    int n = atoi(app.digits);
    app.digits[0] = 0;
    osd_number("");
    if (n > 0) tune(n);
}

static void digit(int d) {
    /* 0 without input: back to the previous channel */
    if (d == 0 && !app.digits[0]) {
        tvsrc_tune_last();
        return;
    }
    size_t n = strlen(app.digits);
    if (n < 4) {
        app.digits[n] = (char) ('0' + d);
        app.digits[n + 1] = 0;
    }
    osd_number(app.digits);
    app.digit_at = SDL_GetTicks();
    if (strlen(app.digits) >= 4) tune_digits();
}

/* a TVF window is open and gets the keys - also on top of an HbbTV app
 * (e.g. teletext via Blue long: arrows and Fastext belong to it) */
/* ---------------- Info window (OK long in the live picture) ---------------- */

/* DVB genre (EN 300 468, content_nibble 1/2) as tvh reports it as contentType */
static const char *genre_name(unsigned g) {
    static const struct {
        unsigned code;
        const char *name;
    } tab[] = {
        {0x10, N_("Movie")}, {0x11, N_("Crime")}, {0x12, N_("Adventure")}, {0x13, N_("Science fiction")},
        {0x14, N_("Comedy")}, {0x15, N_("Soap")}, {0x16, N_("Romance")}, {0x17, N_("Historical")},
        {0x18, N_("Adult")}, {0x20, N_("News")}, {0x21, N_("News/weather")}, {0x22, N_("Magazine")},
        {0x23, N_("Documentary")}, {0x24, N_("Discussion")}, {0x30, N_("Show")}, {0x31, N_("Quiz")},
        {0x32, N_("Variety")}, {0x33, N_("Talk show")}, {0x40, N_("Sport")}, {0x41, N_("Sports event")},
        {0x42, N_("Sports magazine")}, {0x43, N_("Football")}, {0x44, N_("Tennis")}, {0x45, N_("Team sports")},
        {0x46, N_("Athletics")}, {0x47, N_("Motor sport")}, {0x48, N_("Water sport")}, {0x49, N_("Winter sports")},
        {0x4A, N_("Equestrian")}, {0x4B, N_("Martial arts")}, {0x50, N_("Children")}, {0x51, N_("Pre-school")},
        {0x55, N_("Cartoons")}, {0x60, N_("Music")}, {0x61, N_("Rock/pop")}, {0x62, N_("Classical music")},
        {0x63, N_("Folk music")}, {0x64, N_("Jazz")}, {0x65, N_("Musical/opera")}, {0x66, N_("Ballet")},
        {0x70, N_("Arts/culture")}, {0x80, N_("Society")}, {0x81, N_("Report")}, {0x82, N_("Economics")},
        {0x90, N_("Knowledge")}, {0x91, N_("Nature")}, {0x92, N_("Technology")}, {0x93, N_("Medicine")},
        {0x94, N_("Travel")}, {0xA0, N_("Leisure")}, {0xA1, N_("Travel")}, {0xA5, N_("Cooking")},
        {0xA6, N_("Shopping")}, {0xA7, N_("Gardening")},
    };
    static const char *main_names[16] = {NULL, N_("Movie"), N_("News"), N_("Show"), N_("Sport"), N_("Children"),
                                         N_("Music"), N_("Arts/culture"), N_("Society"), N_("Knowledge"), N_("Leisure"),
                                         NULL, NULL, NULL, NULL, NULL};
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (tab[i].code == g) return _(tab[i].name);
    const char *m = main_names[(g >> 4) & 0xF];
    return m ? _(m) : NULL;
}

static void info_from_tvsrc(const tvsrc_event *e, info_event *o) {
    o->id = e->id;
    o->start = (long) e->start;
    o->stop = (long) e->stop;
    snprintf(o->title, sizeof(o->title), "%s", e->title);
    snprintf(o->subtitle, sizeof(o->subtitle), "%s", e->subtitle);
    size_t m = 0;
#define META(...)                                                                                                      \
    do {                                                                                                               \
        if (m < sizeof(o->meta))                                                                                       \
            m += (size_t) snprintf(o->meta + m, sizeof(o->meta) - m, "%s", m ? "  \xC2\xB7  " : "");                  \
        if (m < sizeof(o->meta)) m += (size_t) snprintf(o->meta + m, sizeof(o->meta) - m, __VA_ARGS__);               \
    } while (0)
    const char *g = e->genre ? genre_name(e->genre) : NULL;
    if (g) META("%s", g);
    if (e->season && e->episode) META(_("Season %u, episode %u"), e->season, e->episode);
    else if (e->episode) META(_("Episode %u"), e->episode);
    else if (e->onscreen[0]) META("%.20s", e->onscreen);
    if (e->year) META("%u", e->year);
    if (e->age) META(_("%u+"), e->age);
    if (e->stop > e->start) META("%ld min", (long) ((e->stop - e->start + 30) / 60));
#undef META
    /* short text before the long text if tvh has both and it is not already the
     * subtitle (DVB EIT of the private channels: short text = subtitle) */
    if (e->summary[0] && e->text[0] && strcmp(e->summary, e->subtitle))
        snprintf(o->text, sizeof(o->text), "%s\n\n%s", e->summary, e->text);
    else snprintf(o->text, sizeof(o->text), "%s", e->text[0] ? e->text : e->summary);
    snprintf(o->image, sizeof(o->image), "%s", e->image);
}

static void open_info(void) {
    info_data d;
    memset(&d, 0, sizeof(d));
    d.number = app.channel;
    snprintf(d.channel, sizeof(d.channel), "%s", app.chname);
    snprintf(d.logo, sizeof(d.logo), "%s", app.icon);
    tvsrc_event e;
    if (tvsrc_event_info(app.channel, 0, &e)) {
        d.have_now = 1;
        info_from_tvsrc(&e, &d.now);
    }
    if (tvsrc_event_info(app.channel, 1, &e)) {
        d.have_next = 1;
        info_from_tvsrc(&e, &d.next);
    }
    osd_hide();
    info_open(&d);
    info_rx_update();
}

static int tvf_overlay(void) {
    return settings_visible() || subs_adjusting() || aoff_adjusting || choice_visible() || ttx_visible() || recdlg_visible() || recbrowser_visible() ||
           epg_visible() || info_visible();
}

static void on_key(int key) {
    if (settings_visible()) {
        settings_key(key);
        return;
    }
    if (!app.key_to_tvf && !tvf_overlay() && hbbweb_active() && hbbweb_key(key)) return;
    if (subs_adjusting() && subs_adjust_key(key)) return;
    if (aoff_adjusting && aoff_key(key)) return;
    if (choice_visible() && choice_key(key)) return;
    if (ttx_visible() && ttx_key(key)) return;
    if (recdlg_visible() && recdlg_key(key)) return;
    if (recbrowser_visible() && recbrowser_key(key)) return;
    if (epg_visible() && epg_key(key)) return;
    if (info_visible()) {
        if (key == KEY_RECORD) { /* record the event shown */
            uint32_t id = info_event_id();
            info_close();
            recdlg_open(app.channel, id);
            return;
        }
        if (info_key(key)) return;
    }
    if (recplay_active() && !KEY_IS_DIGIT(key) && recplay_key(key)) return;
    if (KEY_IS_DIGIT(key)) {
        digit(key - KEY_DIGIT0);
        return;
    }
    switch (key) {
    case KEY_CH_UP: tvsrc_step(1); break;
    case KEY_CH_DOWN: tvsrc_step(-1); break;
    case KEY_OK:
        /* like the HTML5 app: in timeshift or with the banner visible, pause/resume */
        if (app.digits[0]) tune_digits();
        else if (tshift_active() || (osd_visible() && tshift_available())) tshift_toggle_pause();
        else if (osd_visible()) osd_hide();
        else {
            tvsrc_refresh_now_next();
            osd_show(OSD_INFO_MS);
        }
        break;
    case KEY_LEFT: tshift_seek(-30); break; /* timeshift 30 s back */
    case KEY_RIGHT:
        if (tshift_active()) tshift_seek(30); /* at the end: live */
        break;
    case KEY_PLAY:
        if (tshift_active() && player_paused(app.pl)) tshift_toggle_pause();
        break;
    case KEY_PAUSE:
        if (!player_paused(app.pl)) tshift_toggle_pause();
        break;
    case KEY_PLAYPAUSE: tshift_toggle_pause(); break;
    case KEY_REWIND: tshift_seek(-30); break;
    case KEY_FFWD:
        if (tshift_active()) tshift_seek(30);
        break;
    case KEY_INFO:
        if (osd_visible()) osd_hide();
        else {
            tvsrc_refresh_now_next();
            osd_show(OSD_INFO_MS);
        }
        break;
    case KEY_BACK:
        /* in timeshift back to live, otherwise clear the input or the banner, otherwise
         * the channel list (like the HTML5 app) */
        if (tshift_active()) {
            tshift_live();
        } else if (app.digits[0]) {
            app.digits[0] = 0;
            osd_number("");
        } else if (osd_visible()) {
            osd_hide();
        } else {
            epg_open(app.channel);
        }
        break;
    case KEY_UP:
    case KEY_DOWN:
        osd_hide();
        epg_open(app.channel);
        break;
    case KEY_YELLOW: open_audio_menu(); break;
    case KEY_RED: /* the broadcaster's HbbTV in our own renderer */
        if (recplay_active()) break;
        if (app.side_local && hbbweb_open(app.channel, app.chname, hbbtv_rewrites())) osd_hide();
        else osd_message(_("No HbbTV for this channel"), 3000, 0);
        break;
    case KEY_GREEN:
        osd_hide();
        epg_close();
        recbrowser_open(NULL);
        break;
    case KEY_BLUE:
        if (recplay_active() || !ttx_available() ||
            (!strncmp(app.url, "htsp://", 7) && !sidedata_has_teletext())) {
            osd_message(_("No teletext for this channel"), 3000, 0);
        } else {
            osd_hide();
            ttx_open();
        }
        break;
    case KEY_RECORD:
        osd_hide();
        recdlg_open(app.channel, 0);
        break;
    case KEY_STOP: /* like the HTML5 app: in timeshift back to live, otherwise quit */
        if (tshift_active()) {
            tshift_live();
            break;
        }
        mt_log("[app] Stop: quitting app");
        g_quit = 1;
        break;
    default: break;
    }
}

/* ---------------- HbbTV in our own renderer (hbbweb.c) ---------------- */

/* HbbTV media playing instead of live (catch-up service): stop the live picture so the
 * decoder is free; afterwards live again */
/* Live picture with HbbTV: off while the page's media are playing (they need
 * the decoder) or the page has stopped the TV picture */
static void hbb_live_apply(void) {
    int off = app.hbb_media_on || app.hbb_bc_off;
    if (off && !app.hbbtv_live_off) {
        app.hbbtv_live_off = 1;
        player_stop(app.pl);
    } else if (!off && app.hbbtv_live_off) {
        app.hbbtv_live_off = 0;
        if (app.url[0] && !app.background) player_play(app.pl, app.url);
    }
}

static void hbb_media(int playing) {
    app.hbb_media_on = playing;
    hbb_live_apply();
}

static void hbb_broadcast(int on) {
    if (on && app.hbb_bc_off_at) {
        app.hbb_bc_off_at = 0; /* stopped and bound again at once: the picture keeps running */
        mt_log("[app] HbbTV: TV picture stopped and back at once - left running");
        return;
    }
    if (!on && !app.hbb_bc_off) {
        if (!app.hbb_bc_off_at) app.hbb_bc_off_at = (SDL_GetTicks() + HBB_BC_OFF_MS) | 1;
        return;
    }
    if (app.hbb_bc_off == !on) return;
    app.hbb_bc_off = !on;
    mt_log("[app] HbbTV: TV picture back");
    hbb_live_apply();
}

/* main loop: a pending stop() of the page takes effect */
static void hbb_broadcast_tick(void) {
    if (!app.hbb_bc_off_at || (int32_t) (SDL_GetTicks() - app.hbb_bc_off_at) < 0) return;
    app.hbb_bc_off_at = 0;
    app.hbb_bc_off = 1;
    mt_log("[app] HbbTV: TV picture stopped by the app");
    hbb_live_apply();
}

static void hbb_video(int x, int y, int w, int h) { player_set_video_rect(app.pl, x, y, w, h); }

static int hbb_dvb_ids(int *onid, int *tsid, int *sid) { return player_dvb_ids(app.pl, onid, tsid, sid); }

/* HbbTV getComponents(): video, audio and subtitles of the subscription with component tag
 * (tvh with HTSP extension); sub = subtitle ID as in subs.c */
static char *hbb_components(void) {
    player_component c[PLAYER_MAX_COMPONENTS];
    int n = player_components(app.pl, c, PLAYER_MAX_COMPONENTS);
    subs_option so[SUBS_MAX];
    int cur = -1;
    subs_options(so, SUBS_MAX, &cur);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char sub[24] = "";
        if (c[i].kind == 2 && c[i].ttx_page) snprintf(sub, sizeof(sub), "%03X", c[i].ttx_page);
        else if (c[i].kind == 2) snprintf(sub, sizeof(sub), "dvb:%d:%d", c[i].index, c[i].composition);
        cJSON_AddNumberToObject(o, "kind", c[i].kind);
        cJSON_AddNumberToObject(o, "index", c[i].index);
        if (c[i].tag >= 0) cJSON_AddNumberToObject(o, "tag", c[i].tag);
        cJSON_AddStringToObject(o, "enc", c[i].encoding);
        cJSON_AddStringToObject(o, "lang", c[i].lang);
        cJSON_AddBoolToObject(o, "ad", c[i].kind == 1 && c[i].audio_type == 3);
        cJSON_AddBoolToObject(o, "hi", c[i].kind == 2 ? c[i].hearing : c[i].audio_type == 2);
        if (c[i].channels) cJSON_AddNumberToObject(o, "ch", c[i].channels);
        if (sub[0]) cJSON_AddStringToObject(o, "sub", sub);
        cJSON_AddBoolToObject(o, "active", c[i].kind == 2 ? cur >= 0 && !strcmp(so[cur].id, sub) : c[i].active);
        cJSON_AddItemToArray(a, o);
    }
    char *s = cJSON_PrintUnformatted(a);
    cJSON_Delete(a);
    return s;
}

/* HbbTV selectComponent/unselectComponent: only for this session (no
 * remembered choice); unselecting audio keeps the current track */
static void hbb_component(int kind, int index, const char *sub, int on) {
    if (kind == 1 && on) {
        player_track t[PLAYER_MAX_TRACKS];
        int cur = -1, n = player_audio_tracks(app.pl, t, PLAYER_MAX_TRACKS, &cur);
        for (int i = 0; i < n; i++)
            if (t[i].pid == index && i != cur) {
                mt_log("[app] HbbTV selects audio track %d", index);
                player_select_audio(app.pl, index);
            }
    } else if (kind == 2) {
        if (on && sub && sub[0]) subs_select_session(sub);
        else if (!on) subs_select_session(NULL);
    }
}

/* catch-up service in our own renderer (hbbmedia.c): live picture off/on, video window */
static void hbb_live(int on) { hbb_media(!on); }
static const char *hbb_window(void) { return player_window_id(app.pl); }
static void hbb_source(int w, int h) { player_set_video_source(app.pl, w, h); }

static void hbb_closed(void) {
    app.hbb_bc_off = 0; /* without an app the TV picture applies again */
    app.hbb_bc_off_at = 0;
    app.hbb_pre_at = SDL_GetTicks() + HBB_PRELOAD_MS; /* if the user stays on the channel: preload again */
    /* WebKit was playing itself: the media server returns the live picture
     * once WebKit's pipeline is gone (otherwise both would fight over the decoder) */
    if (webmedia_active()) return;
    player_set_video_rect(app.pl, 0, 0, 0, 0);
    if (app.hbbtv_live_off) {
        hbb_media(0);
        return;
    }
    /* a page without a replacement (its own player, e.g. ORF ON) does not report its video,
     * but may have taken the decoder from the live picture: then restart
     * right away (not only via the retry) */
    player_status st;
    player_status_get(app.pl, &st);
    if (app.url[0] && !app.background && !recplay_active() && (st.ended || !st.active)) {
        mt_log("[app] after HbbTV: restarting live picture");
        app.retries = 0;
        app.retry_at = 0;
        retry_clear();
        player_play(app.pl, app.url);
    }
}

/* Magic Remote wheel: lists up/down, teletext page forward/back, recording
 * and timeshift +/-30 s, HbbTV see hbbweb_wheel, in the live picture channel +/-1
 * (like LG's own live TV) */
/* pointer to the topmost open view */
static void on_pointer(int ev, int x, int y) {
    if (subs_adjusting() || aoff_adjusting || ttx_visible()) return;
    if (settings_visible()) settings_pointer(ev, x, y);
    else if (choice_visible()) choice_pointer(ev, x, y);
    else if (recdlg_visible()) recdlg_pointer(ev, x, y);
    else if (recbrowser_visible()) recbrowser_pointer(ev, x, y);
    else if (epg_visible()) epg_pointer(ev, x, y);
    else if (info_visible()) info_pointer(ev, x, y);
    else if (hbbweb_pointer(ev, x, y)) return; /* HbbTV page with pointer control */
    else if (recplay_active()) recplay_pointer(ev, x, y);
    else if (ev == PTR_UP) on_key(KEY_OK); /* live picture, HbbTV without pointer: click like OK */
}

/* forget held keys (background, focus lost): no late "long" */
static void reset_keys(void) {
    app.held_ok = app.held_ok_long = 0;
    app.held_color = app.held_color_long = 0;
    app.held_digit = 0;
    app.ok_pending = 0;
}

static void pointer_visible(int vis) {
    if (vis == app.pointer) return;
    app.pointer = vis;
    if (vis) app.held_ok = 0; /* OK press with pointer movement: neither short nor long */
    mt_log("[app] pointer %s", vis ? "visible" : "hidden");
    if (!vis) {
        recplay_pointer_gone();
        hbbweb_pointer_gone();
    }
}

static void on_wheel(int dir) {
    int key = dir > 0 ? KEY_UP : KEY_DOWN;
    if (aoff_adjusting) aoff_key(dir > 0 ? KEY_RIGHT : KEY_LEFT); /* offsets: up = later */
    else if (subs_adjusting()) subs_adjust_key(dir > 0 ? KEY_RIGHT : KEY_LEFT);
    else if (settings_visible()) settings_wheel(dir);
    else if (choice_visible()) choice_key(key);
    else if (ttx_visible()) ttx_key(key); /* Up = next page */
    else if (recdlg_visible()) recdlg_key(key);
    else if (recbrowser_visible()) recbrowser_key(key);
    else if (epg_visible()) epg_wheel(dir);
    else if (info_visible()) info_wheel(dir);
    else if (recplay_active()) recplay_wheel(dir);
    else if (hbbweb_wheel(dir)) return; /* HbbTV open: wheel goes to the page, never a channel change */
    else if (tshift_active()) tshift_seek(dir > 0 ? 30 : -30);
    else tvsrc_step(dir > 0 ? 1 : -1);
}

static int key_is_color(int key) { return key == KEY_RED || key == KEY_GREEN || key == KEY_YELLOW || key == KEY_BLUE; }

/* OK in the live picture (no TVF window, no pointer, no partially entered
 * channel number; with HbbTV open only if the app does not take OK): short as
 * before, long the info window */
static int live_ok(void) {
    return !tvf_overlay() && !recplay_active() && !app.pointer && !app.digits[0] &&
           (!hbbweb_active() || !hbbweb_takes(KEY_OK));
}

/* Green without HbbTV (in the live picture, layout "Standard"): short recordings, long
 * settings. With "HbbTV first", Green without HbbTV is simply recordings */
static int live_green(int key) {
    return !g_keys_hbbtv && key == KEY_GREEN && !hbbweb_active() && !tvf_overlay() && !recplay_active();
}

/* ---- Basic settings (settings page) ----
 * Stored in prefs; whatever is missing comes from the defaults here. "" = off. */
#ifndef TVF_DEFAULT_TVH_HOST /* build option (CMake, local.env) */
#define TVF_DEFAULT_TVH_HOST ""
#endif
static void cfg_get(const char *key, char *out, size_t n) {
    const cJSON *v = prefs_get(key);
    if (cJSON_IsString(v)) {
        snprintf(out, n, "%s", v->valuestring);
        return;
    }
    if (cJSON_IsNumber(v)) {
        snprintf(out, n, "%d", (int) v->valuedouble);
        return;
    }
    if (!strcmp(key, "tvh.host")) {
        snprintf(out, n, "%s", TVF_DEFAULT_TVH_HOST);
        return;
    }
    if (!strcmp(key, "tvh.http_port")) snprintf(out, n, "9981");
    else if (!strcmp(key, "tvh.htsp_port")) snprintf(out, n, "9982");
    else if (!strcmp(key, "hbbtv.egress")) snprintf(out, n, "rules");
    else if (!strcmp(key, "keys.layout")) snprintf(out, n, "classic");
    else if (!strcmp(key, "ui.lang")) snprintf(out, n, "auto");
    else if (!strcmp(key, "kore.name")) snprintf(out, n, "TVF");
    else if (!strcmp(key, "kore.http_port")) snprintf(out, n, "8080");
    else if (!strcmp(key, "kore.tcp_port")) snprintf(out, n, "9090");
    else out[0] = 0;
}

static int cfg_int(const char *key) {
    char b[32];
    cfg_get(key, b, sizeof(b));
    return atoi(b);
}

static void settings_restart(void) {
    mt_log("[app] settings changed - restarting");
    g_restart = 1;
    g_quit = 1;
}

static void open_settings(void) {
    osd_hide();
    settings_open();
}

#define FIND_WAIT_MS 1500

/* search the network for tvh servers (settings page, start without a server) */
static void app_find(void) {
    if (mdns_find_tvh(FIND_WAIT_MS) == 0) g_finding = 1;
    else settings_servers(NULL, 0);
}

/* the result of the search: to the settings page; at the start without a
 * server the only one found is taken (TVF restarts with it), otherwise the
 * page opens to choose or enter one */
static void found_servers(void) {
    mdns_tvh f[MDNS_TVH_MAX];
    int n = mdns_find_tvh_result(f, MDNS_TVH_MAX);
    if (n < 0) return;
    g_finding = 0;
    settings_server s[MDNS_TVH_MAX];
    for (int i = 0; i < n; i++) {
        char hn[64];
        snprintf(hn, sizeof(hn), "%s", f[i].hostname);
        size_t l = strlen(hn);
        if (l > 6 && !strcmp(hn + l - 6, ".local")) hn[l - 6] = 0;
        snprintf(s[i].label, sizeof(s[i].label), "%s \xE2\x80\x93 %s (%s)", f[i].name, hn, f[i].addr);
        snprintf(s[i].host, sizeof(s[i].host), "%s", f[i].addr);
        s[i].http_port = f[i].http_port;
        s[i].htsp_port = f[i].htsp_port;
    }
    if (!g_find_first) {
        settings_servers(s, n);
        return;
    }
    g_find_first = 0;
    /* (with tvhTest nothing is saved: the page shows it instead) */
    if (n == 1 && !g_tvh_test) {
        mt_log("[app] the only tvh server in the network: %s - taken", s[0].label);
        prefs_set("tvh.host", cJSON_CreateString(s[0].host));
        if (s[0].http_port > 0) prefs_set("tvh.http_port", cJSON_CreateNumber(s[0].http_port));
        prefs_set("tvh.htsp_port", cJSON_CreateNumber(s[0].htsp_port));
        settings_restart();
        return;
    }
    osd_hide();
    settings_servers(s, n);
    settings_open_at(n ? "tvh.find" : "tvh.host",
                     n ? _("Choose the TVH server with left/right, then save")
                       : _("No TVH server found in the network - enter its address"));
}

/* tvh refuses the user: once the settings at the login */
static void app_noaccess(void) {
    if (g_noaccess_shown || settings_visible()) return;
    g_noaccess_shown = 1;
    osd_hide();
    settings_open_at("tvh.user", _("The TVH server refused the login - enter user and password"));
}

/* Colour key short with HbbTV open: to the app if its keyset requests it.
 * Otherwise not to TVF (that would differ depending on the keyset), only a
 * hint; Red without Red in the keyset restarts HbbTV as before */
static void color_short(int key) {
    if (live_green(key)) {
        on_key(key); /* recordings */
        return;
    }
    if (hbbweb_active() && !tvf_overlay()) {
        if (hbbweb_key(key)) return;
        if (key != KEY_RED) {
            osd_message(key == KEY_GREEN    ? _("Recordings: hold green")
                        : key == KEY_YELLOW ? _("Audio: hold yellow")
                                            : _("Teletext: hold blue"),
                        2000, 0);
            return;
        }
    }
    on_key(key);
}

/* Colour key long: TVF (recordings, audio, teletext); Red long closes HbbTV,
 * with "HbbTV first" and no visible HbbTV: settings */
static void color_long(int key) {
    if (live_green(key)) {
        open_settings();
        return;
    }
    if (key == KEY_RED && hbbweb_active()) {
        hbbweb_close("red long");
        osd_message(_("HbbTV closed"), 2000, 0);
        return;
    }
    if (key == KEY_RED && g_keys_hbbtv && !tvf_overlay()) {
        open_settings();
        return;
    }
    app.key_to_tvf = 1;
    on_key(key);
    app.key_to_tvf = 0;
}

/* Colour key held: long already after COLOR_LONG_MS, not only on
 * release (the remote does not repeat colour keys); OK in the live picture
 * after OK_LONG_MS (repeats of OK do not count) */
static void color_hold_tick(void) {
    if (app.held_ok && !app.held_ok_long && SDL_GetTicks() - app.held_ok_at >= OK_LONG_MS) {
        app.held_ok_long = 1;
        mt_log("[app] key OK long");
        open_info();
    }
    if (!app.held_color || app.held_color_long || SDL_GetTicks() - app.held_color_at < COLOR_LONG_MS) return;
    app.held_color_long = 1;
    mt_log("[app] key %s long", key_name(app.held_color));
    color_long(app.held_color);
}

/* Digit keys only take effect on release: when held long, webOS starts the key's
 * quick access, which must not trigger a channel change beforehand. */
static void on_sdl_key(const SDL_KeyboardEvent *k, int down) {
    int key = key_from_sdl(k);
    int kc = (int) k->keysym.sym;
    /* webOS decides Red itself: when held long no red key arrives, only
     * this code (and webOS shows the pointer) */
    if (k->keysym.scancode == (SDL_Scancode) SCANCODE_RED_LONG) {
        if (down && !k->repeat) {
            mt_log("[app] key RED long");
            color_long(KEY_RED);
        }
        return;
    }
    if (k->keysym.scancode == (SDL_Scancode) SDL_WEBOS_SCANCODE_CURSOR_SHOW ||
        k->keysym.scancode == (SDL_Scancode) SDL_WEBOS_SCANCODE_CURSOR_HIDE) {
        if (down) pointer_visible(k->keysym.scancode == (SDL_Scancode) SDL_WEBOS_SCANCODE_CURSOR_SHOW);
        return;
    }
    /* arrow keys hide the pointer - in case the notification is missing */
    if (down && (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT)) pointer_visible(0);
    if (key == KEY_OK && app.pointer) {
        /* pointer visible: the click decides; if none comes, the key counts */
        if (down && !k->repeat && SDL_GetTicks() - app.mouse_at > CLICK_DEDUP_MS) app.ok_pending = SDL_GetTicks();
        return;
    }
    /* OK in the live picture: short on release as before (banner, pause), long
     * after OK_LONG_MS (color_hold_tick) the info window; all events
     * of this press belong here (the info window does not get the
     * release) */
    if (key == KEY_OK && ((down && !k->repeat && live_ok()) || app.held_ok)) {
        if (down) {
            if (!k->repeat) {
                app.held_ok = 1;
                app.held_ok_at = SDL_GetTicks();
                app.held_ok_long = 0;
            }
        } else {
            int was_long = app.held_ok_long;
            app.held_ok = 0;
            if (!was_long) {
                mt_log("[app] key OK");
                on_key(KEY_OK);
            }
        }
        return;
    }
    if (KEY_IS_DIGIT(key)) {
        if (down) {
            if (app.held_digit == kc) {
                app.held_long = 1; /* repeat */
            } else {
                app.held_digit = kc;
                app.held_at = SDL_GetTicks();
                app.held_long = k->repeat != 0;
            }
        } else if (app.held_digit == kc) {
            int is_long = app.held_long || SDL_GetTicks() - app.held_at > DIGIT_LONG_MS;
            app.held_digit = 0;
            if (!is_long) on_key(key);
        }
        return;
    }
    /* Colour keys with HbbTV open (no TVF window): short = app, long = TVF;
     * Green in the live picture: short recordings, long settings. Red long: SCANCODE_RED_LONG.
     * Short on release, long after COLOR_LONG_MS (color_hold_tick) or on
     * the first repeat; whether the key belongs here is decided by the
     * first press */
    if (key_is_color(key) &&
        ((down && !k->repeat && ((hbbweb_active() && !tvf_overlay()) || live_green(key))) ||
         app.held_color == key)) {
        uint32_t now = SDL_GetTicks();
        if (down) {
            if (!k->repeat || app.held_color != key) {
                app.held_color = key;
                app.held_color_at = now;
                app.held_color_long = 0;
            } else if (!app.held_color_long) {
                app.held_color_long = 1;
                mt_log("[app] key %s long", key_name(key));
                color_long(key);
            }
        } else {
            int was_long = app.held_color_long, is_long = now - app.held_color_at >= COLOR_LONG_MS;
            app.held_color = 0;
            if (!was_long) {
                mt_log("[app] key %s%s", key_name(key), is_long ? " long" : "");
                if (is_long) color_long(key);
                else color_short(key);
            }
        }
        return;
    }
    if (!down || k->repeat) {
        /* repeat only for navigating lists */
        if (!down || !(key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT) ||
            !(epg_visible() || settings_visible()))
            return;
    }
    if (key == KEY_NONE) {
        mt_log("[app] unmapped key: scancode=%d keycode=%d (%s)", (int) k->keysym.scancode,
               (int) k->keysym.sym, SDL_GetKeyName(k->keysym.sym));
        return;
    }
    mt_log("[app] key %s", key_name(key));
    on_key(key);
}

/* reply to LUNA from the debug port (libhelpers thread): whole, long ones in
 * chunks "reply <offset>/<length>:" (a log line holds 2 KB) */
#define LUNA_DEBUG_CHUNK 1500
static void on_luna_debug_reply(const char *reply, void *user) {
    (void) user;
    size_t len = reply ? strlen(reply) : 0;
    if (len <= LUNA_DEBUG_CHUNK) {
        mt_log("[luna] reply: %s", reply ? reply : "(none)");
        return;
    }
    for (size_t off = 0; off < len; off += LUNA_DEBUG_CHUNK) {
        mt_log("[luna] reply %zu/%zu: %.1500s", off, len, reply + off);
        SDL_Delay(2); /* do not overrun the UDP log */
    }
}

/* development aid: commands from the debug port */
static void kore_tv_on_test(void);

/* debug LOCAL <path>: reply of our own teletext/subtitle store to the log */
static void on_local_debug(void *user, int status, const char *body, size_t len) {
    (void) user;
    mt_log("[debug] local -> %d (%zu bytes), picture pts %lld: %.900s", status, len, player_stream_pts(app.pl),
           body ? body : "");
}

static void on_debug(const char *c) {
    mt_log("[debug] %.200s", c);
    if (!strncmp(c, "LOCAL ", 6)) {
        char u[256];
        snprintf(u, sizeof(u), "local:%s", c + 6);
        fetch_get(u, on_local_debug, NULL);
        return;
    }
    if (!strncmp(c, "WEBJS ", 6)) {
        /* evaluate an expression in the HbbTV page; result line by line as
         * [web:js] lines prefixed with "WEBJS" */
        size_t n = strlen(c + 6) + 320;
        char *js = malloc(n);
        if (js) {
            snprintf(js, n,
                     "(function(){var r;try{r=(function(){return %.8000s;})();}catch(e){r='error '+e;}"
                     "r=typeof r==='string'?r:JSON.stringify(r);String(r).split('\\n').forEach("
                     "function(l){console.log('WEBJS '+l);});})()",
                     c + 6);
            webr_eval(js);
            free(js);
        }
        return;
    }
    if (!strncmp(c, "KEYSHORT ", 9) || !strncmp(c, "KEYLONG ", 8)) { /* colour key/OK short/long as if pressed */
        int lng = c[3] == 'L', off = lng ? 8 : 9;
        if (!strcmp(c + off, "OK")) {
            if (lng && live_ok()) open_info();
            else on_key(KEY_OK);
            return;
        }
        for (int k = 1; k <= KEY_FFWD; k++)
            if (key_is_color(k) && !strcmp(c + off, key_name(k))) {
                if (lng) color_long(k);
                else color_short(k);
            }
    } else if (!strncmp(c, "KEY ", 4)) {
        for (int k = 1; k <= KEY_FFWD; k++) {
            if (!strcmp(c + 4, key_name(k))) {
                on_key(k);
                return;
            }
        }
        if (c[4] >= '0' && c[4] <= '9' && !c[5]) on_key(KEY_DIGIT0 + (c[4] - '0'));
    } else if (!strncmp(c, "SHOT ", 5)) {
        ui_screenshot_send(c + 5);
    } else if (!strncmp(c, "AOFF ", 5)) { /* AOFF <ms|clear> [channel]: sound offset (test, cleanup) */
        char v[16] = "";
        int ch = aoff_channel;
        sscanf(c + 5, "%15s %d", v, &ch);
        int clear = !strcmp(v, "clear");
        aoff_store(ch, atoi(v), clear);
        if (ch == aoff_channel) {
            aoff_dirty = 0;
            aoff_load(ch);
        }
        mt_log("[app] audio offset of channel %d: %s", ch, clear ? "removed (and the last one set)" : v);
    } else if (!strcmp(c, "TVON")) { /* test: as if the TV had just been switched on (Kore rule) */
        kore_tv_on_test();
    } else if (!strncmp(c, "PTR ", 4)) { /* PTR MOVE|DOWN|UP x y */
        char what[8];
        int x, y;
        if (sscanf(c + 4, "%7s %d %d", what, &x, &y) == 3) {
            pointer_visible(1);
            on_pointer(!strcmp(what, "DOWN") ? PTR_DOWN : !strcmp(what, "UP") ? PTR_UP : PTR_MOVE, x, y);
        }
    } else if (!strncmp(c, "VDROP ", 6)) { /* VDROP <s>: discard video (test of the picture watchdog) */
        player_test_drop_video(app.pl, atoi(c + 6));
    } else if (!strncmp(c, "CUT", 3) && (!c[3] || c[3] == ' ')) { /* CUT [n]: cut the file connection (test) */
        player_test_cut(app.pl, c[3] ? atoi(c + 4) : 0);
    } else if (!strncmp(c, "WHEEL ", 6)) { /* WHEEL 1 / WHEEL -1 */
        on_wheel(atoi(c + 6) > 0 ? 1 : -1);
    } else if (!strncmp(c, "LUNA ", 5)) { /* LUNA <uri> <json>: call a system service */
        char uri[256];
        int n = 0;
        if (sscanf(c + 5, "%255s %n", uri, &n) == 1 && n > 0) {
            int r = luna_call(uri, c + 5 + n, on_luna_debug_reply, NULL);
            mt_log("[luna] %s -> %s", uri, r == 0 ? "sent" : "failed");
        }
    } else if (!strncmp(c, "MEDIA ", 6)) { /* MEDIA <URI|HLS|MPEG-DASH> <url>: media URL instead of live */
        char type[16];
        int n = 0;
        if (sscanf(c + 6, "%15s %n", type, &n) == 1 && n > 0) {
            app.hbbtv_live_off = 1;
            player_play_media(app.pl, type, c + 6 + n);
        }
    } else if (!strncmp(c, "HMEDIA ", 7)) { /* HMEDIA <url>: like an HbbTV page (uMediaServer via Luna) */
        hbbmedia_play(99, c + 7, "", 0);
    } else if (!strncmp(c, "HMEDIAT ", 8)) { /* HMEDIAT <URI|HLS|MPEG-DASH> <url> */
        char type[16];
        int n = 0;
        if (sscanf(c + 8, "%15s %n", type, &n) == 1 && n > 0) hbbmedia_play(99, c + 8 + n, type, 0);
    } else if (!strcmp(c, "DRMLOAD")) { /* load the PlayReady client (test T1) */
        hbbdrm_load();
    } else if (!strncmp(c, "DRMPROXY ", 9)) { /* DRMPROXY 0|1|2: licences direct / via the relay / fully logged */
        hbbdrm_proxy(atoi(c + 9));
    } else if (!strncmp(c, "DRMOVR ", 7)) { /* DRMOVR <la_url>: licence URL override (T2) */
        char x[2048];
        snprintf(x, sizeof(x),
                 "<PlayReadyInitiator xmlns=\"http://schemas.microsoft.com/DRM/2007/03/protocols/\">"
                 "<LicenseServerUriOverride><LA_URL>%s</LA_URL></LicenseServerUriOverride></PlayReadyInitiator>",
                 c + 7);
        hbbdrm_send(HBBDRM_INITIATOR, x, HBBDRM_PLAYREADY, NULL, NULL);
    } else if (!strncmp(c, "DRMMSG ", 7)) { /* DRMMSG <initiator XML> */
        hbbdrm_send(HBBDRM_INITIATOR, c + 7, HBBDRM_PLAYREADY, NULL, NULL);
    } else if (!strcmp(c, "DRMRESET")) {
        hbbdrm_reset();
    } else if (!strcmp(c, "DRMUNLOAD")) {
        hbbdrm_unload();
    } else if (!strncmp(c, "HMEDIADRM ", 10)) { /* HMEDIADRM 0|1: subsequent HMEDIA with option.drm */
        hbbmedia_drm(atoi(c + 10));
    } else if (!strncmp(c, "HMEDIAOPT ", 10)) { /* HMEDIAOPT 1: transport type in option (old way) */
        hbbmedia_transport_in_option(atoi(c + 10));
    } else if (!strcmp(c, "HMEDIASTOP")) {
        hbbmedia_stop_all();
    } else if (!strncmp(c, "WEBMEDIA ", 9)) { /* WEBMEDIA 0|1: WebKit plays media itself (bundle with "media") */
        int on = atoi(c + 9);
        prefs_set("web.media", cJSON_CreateBool(on));
        hbbweb_set_media(on ? webmedia_socket() : NULL);
    } else if (!strncmp(c, "WEBMEDIACT ", 11)) { /* WEBMEDIACT LIVE|none|...: contentsType of the web pipelines */
        prefs_set("web.mediactype", cJSON_CreateString(c + 11));
        webmedia_set_ctype(c + 11);
    } else if (!strncmp(c, "HMEDIASEEK ", 11)) {
        hbbmedia_seek(99, atof(c + 11));
    } else if (!strcmp(c, "MEDIASTOP")) {
        if (app.hbbtv_live_off) {
            app.hbbtv_live_off = 0;
            if (app.url[0]) player_play(app.pl, app.url);
        }
    } else if (!strncmp(c, "MEDIASEEK ", 10)) {
        player_media_seek(app.pl, atof(c + 10));
    } else if (!strncmp(c, "MEDIAPAUSE ", 11)) {
        player_set_paused(app.pl, atoi(c + 11));
    } else if (!strncmp(c, "CDMTEST ", 8)) { /* CDMTEST <system> <licence server> <PSSH>: check the TV's CDMs */
        cdmtest_start(c + 8);
    } else if (!strcmp(c, "MEDIASTAT")) {
        player_media m;
        player_media_get(app.pl, &m);
        mt_log("[app] media: active %d playing %d ended %d error %d pos %lld ms duration %lld ms", m.active, m.playing,
               m.ended, m.error, (long long) m.pos_ms, (long long) m.dur_ms);
    }
}

/* ---------------- Foreground/background ---------------- */

static void go_background(const char *why) {
    if (app.background) return;
    app.background = 1;
    reset_keys();
    mt_log("[app] background (%s): playback stopped", why);
    if (recplay_active()) recplay_suspend();
    else {
        tshift_stop();
        player_stop(app.pl);
    }
}

static void go_foreground(const char *why) {
    if (!app.background) return;
    app.background = 0;
    mt_log("[app] foreground (%s)", why);
    if (recplay_active()) recplay_resume();
    else if (app.url[0]) {
        tshift_on_play(app.url, app.timeshift);
        player_play(app.pl, app.url);
    }
}

static void on_window(const SDL_WindowEvent *w) {
    switch (w->event) {
    case SDL_WINDOWEVENT_HIDDEN:
    case SDL_WINDOWEVENT_MINIMIZED: go_background("window hidden"); break;
    case SDL_WINDOWEVENT_SHOWN:
    case SDL_WINDOWEVENT_RESTORED: go_foreground("window shown"); break;
    case SDL_WINDOWEVENT_FOCUS_LOST: reset_keys(); break;
    default: break;
    }
    mt_log("[app] window event %d", w->event);
}

/* ---------------- Start ---------------- */

/* menu language of the TV (webOS settings, "de-DE" -> "de"); waits at most
 * 1.5 s for the answer. The context stays static: a late answer finds it */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int done;
    char lang[16];
} g_tvlang = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, ""};

static void on_tv_lang(const char *reply, void *user) {
    (void) user;
    cJSON *j = reply ? cJSON_Parse(reply) : NULL;
    const cJSON *loc = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "settings"), "localeInfo"), "locales");
    const char *ui = jstr(loc, "UI");
    pthread_mutex_lock(&g_tvlang.lock);
    if (ui) snprintf(g_tvlang.lang, sizeof(g_tvlang.lang), "%s", ui);
    g_tvlang.done = 1;
    pthread_cond_signal(&g_tvlang.cond);
    pthread_mutex_unlock(&g_tvlang.lock);
    cJSON_Delete(j);
}

static void tv_ui_language(char *out, size_t n) {
    out[0] = 0;
    if (luna_call("luna://com.webos.settingsservice/getSystemSettings", "{\"keys\":[\"localeInfo\"]}", on_tv_lang,
                  NULL) != 0) {
        mt_log("[app] TV language: no Luna");
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 500000000L;
    ts.tv_sec += 1 + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&g_tvlang.lock);
    while (!g_tvlang.done)
        if (pthread_cond_timedwait(&g_tvlang.cond, &g_tvlang.lock, &ts) != 0) break;
    snprintf(out, n, "%s", g_tvlang.lang);
    pthread_mutex_unlock(&g_tvlang.lock);
    mt_log("[app] TV language: %s", out[0] ? out : "unknown (no answer)");
}

static void res_dir_of_exe(char *out, size_t n) {
    char exe[480];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) {
        snprintf(out, n, "res");
        return;
    }
    exe[len] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(out, n, "%s/res", exe);
}

/* WebKit bundle (tvf-web): a remembered path (launch parameter webBundle), otherwise
 * - also with "auto" or if the remembered one lacks run.sh - the package
 * com.lsd.tvf.web next to TVF (package-web.sh), otherwise the old location on the
 * developer partition */
#define WEB_BUNDLE_TEMP "/media/developer/temp/tvf-web-sdk/tvf-web-sdk-armhf"
static void web_bundle(const char *res, const char *want, char *out, size_t n) {
    char run[600];
    if (want && *want && strcmp(want, "auto")) {
        snprintf(run, sizeof(run), "%s/run.sh", want);
        if (access(run, R_OK) == 0) {
            snprintf(out, n, "%s", want);
            mt_log("[app] WebKit bundle %s (remembered)", out);
            return;
        }
        mt_log("[app] WebKit bundle %s missing", want);
    }
    /* res = <Apps>/com.lsd.tvf/res -> <Apps>/com.lsd.tvf.web/bundle */
    char apps[512];
    snprintf(apps, sizeof(apps), "%s", res);
    char *s = strrchr(apps, '/');
    if (s) *s = 0;
    s = strrchr(apps, '/');
    if (s) {
        *s = 0;
        char pkg[560];
        int len = snprintf(pkg, sizeof(pkg), "%s/com.lsd.tvf.web/bundle", apps);
        snprintf(run, sizeof(run), "%s/run.sh", pkg);
        if (len > 0 && (size_t) len < n && access(run, R_OK) == 0) {
            memcpy(out, pkg, (size_t) len + 1);
            mt_log("[app] WebKit bundle from package: %s", out);
            return;
        }
    }
    snprintf(out, n, "%s", WEB_BUNDLE_TEMP);
    mt_log("[app] WebKit bundle %s (no package com.lsd.tvf.web)", out);
}

/* ---------------- Kodi/Kore (kodi.c) ---------------- */

static int kodi_channel(void) { return app.url[0] && !recplay_active() ? app.channel : 0; }
static int kodi_live_paused(void) { return !recplay_active() && player_paused(app.pl); }
static void kodi_tune(int num) { tune(num); }
static void kodi_step(int dir) { tvsrc_step(dir > 0 ? 1 : -1); }
/* Stop: a recording leads back to live TV, otherwise playback off */
static void kodi_stop(void) {
    if (recplay_active()) {
        recplay_exit();
        tvsrc_tune_current();
    } else {
        player_stop(app.pl);
        app.url[0] = 0;
    }
}
static void kodi_key(int key) {
    mt_log("[app] key %s (Kore)", key_name(key));
    on_key(key);
}

/* TV switched on (e.g. via Wake-on-LAN from Kore) and Kore asks within the
 * first KORE_RAISE_S seconds: bring TVF to the front if another app is
 * in front - once per power-on. State from the subscriptions to tvpower and
 * applicationManager (replies in the Luna thread) */
#define KORE_RAISE_S 30
static struct {
    pthread_mutex_t lock;
    char power[32]; /* "Active", "Active Standby", ... */
    char fg[128];   /* app in the foreground */
    time_t on_at;   /* TV switched on, 0 = not observed */
    int raised;
} g_tv = {PTHREAD_MUTEX_INITIALIZER, "", "", 0, 0};

static void on_power_state(const char *reply, void *user) {
    (void) user;
    cJSON *j = cJSON_Parse(reply ? reply : "");
    const char *st = jstr(j, "state");
    pthread_mutex_lock(&g_tv.lock);
    if (st[0] && strcmp(st, g_tv.power)) {
        int on = !strcmp(st, "Active") && g_tv.power[0];
        mt_log("[app] TV power: %s%s", st, on ? " (switched on)" : "");
        if (on) {
            g_tv.on_at = time(NULL);
            g_tv.raised = 0;
        }
        snprintf(g_tv.power, sizeof(g_tv.power), "%s", st);
    }
    pthread_mutex_unlock(&g_tv.lock);
    cJSON_Delete(j);
}

static void on_foreground_app(const char *reply, void *user) {
    (void) user;
    cJSON *j = cJSON_Parse(reply ? reply : "");
    const char *id = jstr(j, "appId");
    pthread_mutex_lock(&g_tv.lock);
    if (cJSON_GetObjectItemCaseSensitive(j, "appId") && strcmp(id, g_tv.fg)) {
        snprintf(g_tv.fg, sizeof(g_tv.fg), "%s", id);
        mt_log("[app] foreground app: %s", id[0] ? id : "-");
    }
    pthread_mutex_unlock(&g_tv.lock);
    cJSON_Delete(j);
}

static void kore_tv_on_test(void) {
    pthread_mutex_lock(&g_tv.lock);
    g_tv.on_at = time(NULL);
    g_tv.raised = 0;
    pthread_mutex_unlock(&g_tv.lock);
    mt_log("[app] TV power: switched on (test)");
}

static void bring_to_front(const char *why) {
    mt_log("[app] %s, TVF not in front - bringing it to the front", why);
    luna_call("luna://com.webos.applicationManager/launch", "{\"id\":\"com.lsd.tvf\"}", NULL, NULL);
}

static void kodi_request(void) {
    pthread_mutex_lock(&g_tv.lock);
    time_t since = g_tv.on_at ? time(NULL) - g_tv.on_at : -1;
    int other = g_tv.fg[0] && strcmp(g_tv.fg, "com.lsd.tvf");
    int go = since >= 0 && since <= KORE_RAISE_S && !g_tv.raised && (other || app.background);
    if (go) g_tv.raised = 1;
    pthread_mutex_unlock(&g_tv.lock);
    if (go) bring_to_front("Kore shortly after TV on");
}

/* channel change or recording from Kore: always to the front */
static void kodi_front(void) {
    pthread_mutex_lock(&g_tv.lock);
    int other = g_tv.fg[0] && strcmp(g_tv.fg, "com.lsd.tvf");
    pthread_mutex_unlock(&g_tv.lock);
    if (other || app.background) bring_to_front("Kore channel change/recording");
}

/* restart after the settings: the same program in the same process (for
 * webOS it stays the same app), launch parameters without the remembered
 * settings - they would otherwise overwrite the new values */
static void restart_self(const char *arg0, const char *arg1) {
    cJSON *launch = cJSON_Parse(arg1 ? arg1 : "{}");
    if (!launch) launch = cJSON_CreateObject();
    cJSON *params = cJSON_GetObjectItemCaseSensitive(launch, "parameters");
    if (!cJSON_IsObject(params)) params = launch;
    static const char *const drop[] = {"tvhUser", "tvhPass",
                                       "webBundle", "lang", "hbbtvEgress"};
    for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) cJSON_DeleteItemFromObjectCaseSensitive(params, drop[i]);
    char *a1 = cJSON_PrintUnformatted(launch);
    cJSON_Delete(launch);
    mt_log("[app] restarting");
    /* inherit nothing open (connections, ports, the socket to the web helper -
     * which then exits) */
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        struct dirent *e;
        int dfd = dirfd(d);
        while ((e = readdir(d)) != NULL) {
            int fd = atoi(e->d_name);
            if (fd > 2 && fd != dfd) fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        closedir(d);
    }
    char *av[] = {(char *) arg0, a1 ? a1 : "{}", NULL};
    execv("/proc/self/exe", av);
    mt_log("[app] restart failed: %s", strerror(errno));
    free(a1);
}

int main(int argc, char *argv[]) {
    mt_log_init("/tmp/tvf.log");

    /* launch parameters: argv[1] is JSON (with "parameters" for webOS launches) */
    cJSON *launch = cJSON_Parse(argc > 1 ? argv[1] : "{}");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(launch, "parameters");
    if (!cJSON_IsObject(params)) params = launch;
    if (*jstr(params, "log")) mt_log_udp(jstr(params, "log"));
    int debug_port = atoi(jstr(params, "debug"));
    if (*jstr(params, "htspQueue")) htsp_set_queue_depth(atoi(jstr(params, "htspQueue"))); /* test: bytes, 0 = tvh default */
    if (!debug_port && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(params, "debug")))
        debug_port = (int) jnum(params, "debug");
    char *pjson = params ? cJSON_PrintUnformatted(params) : NULL;
    {
        /* to the log without password (the log also goes over the network via UDP) */
        cJSON *lp = params ? cJSON_Duplicate(params, 1) : NULL;
        if (cJSON_GetObjectItemCaseSensitive(lp, "tvhPass"))
            cJSON_ReplaceItemInObjectCaseSensitive(lp, "tvhPass", cJSON_CreateString("***"));
        cJSON *lt = cJSON_GetObjectItemCaseSensitive(lp, "tvhTest");
        if (cJSON_GetObjectItemCaseSensitive(lt, "pass"))
            cJSON_ReplaceItemInObjectCaseSensitive(lt, "pass", cJSON_CreateString("***"));
        char *ls = lp ? cJSON_PrintUnformatted(lp) : NULL;
        mt_log("[app] TVF starting, parameters %s", ls ? ls : "{}");
        free(ls);
        cJSON_Delete(lp);
    }

    /* settings: file next to the app, launch parameters are remembered
     * (Home Assistant launches without parameters) */
    char res[512];
    res_dir_of_exe(res, sizeof(res));
    {
        char appdir[512];
        snprintf(appdir, sizeof(appdir), "%s", res);
        char *sl = strrchr(appdir, '/');
        if (sl) *sl = 0;
        prefs_init(appdir);
    }
    {
        /* launch parameter -> setting: tvh credentials "tvhUser"/"tvhPass", "webBundle"
         * (tvf-web), "hbbtvEgress" ("rules", "direct"
         * or proxy), "lang" ("de", "en" ..., "auto" = TV) */
        static const char *const map[][2] = {{"webBundle", "web.bundle"}, {"lang", "ui.lang"},
                                             {"hbbtvEgress", "hbbtv.egress"}};
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (*jstr(params, map[i][0])) prefs_set(map[i][1], cJSON_CreateString(jstr(params, map[i][0])));
        /* formerly: channel source Bridge, live stream via HTTP, dvb-buffer server,
         * connection to the Bridge, StreamProxy (IPTV direct) */
        static const char *const gone[] = {"src.mode", "src.stream", "ext.dvbbuffer", "bridge.addr",
                                           "ext.streamproxy"};
        for (size_t i = 0; i < sizeof(gone) / sizeof(gone[0]); i++)
            if (prefs_get(gone[i])) prefs_set(gone[i], NULL);
        if (*jstr(params, "tvhUser")) {
            prefs_set("tvh.user", cJSON_CreateString(jstr(params, "tvhUser")));
            prefs_set("tvh.pass", cJSON_CreateString(jstr(params, "tvhPass")));
        }
    }
    char host[128], saved_host[128];
    cfg_get("tvh.host", host, sizeof(host));
    snprintf(saved_host, sizeof(saved_host), "%s", host);
    {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(params, "tvhTest");
        if (cJSON_IsObject(t)) {
            g_tvh_test = cJSON_Duplicate(t, 1);
            if (cJSON_GetObjectItemCaseSensitive(g_tvh_test, "host"))
                snprintf(host, sizeof(host), "%s", jstr(g_tvh_test, "host"));
            mt_log("[app] test: tvh server and login of this run from tvhTest (not saved)");
        }
    }
    mt_log("[app] tvh %s (HTTP %d, HTSP %d)", host[0] ? host : "(not set)", cfg_int("tvh.http_port"),
           cfg_int("tvh.htsp_port"));
    {
        char kl[16];
        cfg_get("keys.layout", kl, sizeof(kl));
        g_keys_hbbtv = !strcmp(kl, "hbbtv");
        mt_log("[app] key layout %s", g_keys_hbbtv ? "HbbTV first" : "standard");
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    crash_handler_install();

    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
    /* hide the Magic Remote pointer after 5 s of inactivity: webOS 4's SDL
     * otherwise never does (and does not report CURSOR_HIDE either) */
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_SLEEP_TIME, "5000");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        mt_log("[app] SDL_Init: %s", SDL_GetError());
        return 1;
    }
    {
        SDL_version sv;
        SDL_GetVersion(&sv);
        mt_log("[app] SDL %d.%d.%d", sv.major, sv.minor, sv.patch);
    }
    app.pl = player_create(pjson);
    app.timeshift = 1;
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(params, "timeshift")))
        app.timeshift = (int) jnum(params, "timeshift");
    tshift_init(app.pl);
    /* HTSP live: request tvh's timeshift in the subscription (tvh limits it according to its
     * settings; without timeshift in tvh it stays with the live picture) */
    if (app.timeshift) player_set_htsp_timeshift(app.pl, 7200);
    /* launch parameter "alpha":0 = window surface per SDL default (see below) */
    int want_alpha = cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(params, "alpha")) ? (int) jnum(params, "alpha") : 1;
    /* test: do not write timers and recording positions to tvh ("dvrReadOnly":1) */
    const cJSON *dro = cJSON_GetObjectItemCaseSensitive(params, "dvrReadOnly");
    int dvr_read_only = cJSON_IsTrue(dro) || (cJSON_IsNumber(dro) && dro->valuedouble != 0);
    free(pjson);
    cJSON_Delete(launch);

    /* Window surface with alpha channel: only then does the video plane show through the
     * transparent pixels of the UI. Otherwise webOS 4's SDL may
     * pick an opaque surface (OSD ok, video black). */
    if (want_alpha) {
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    }
    SDL_Window *win = SDL_CreateWindow("TVF", 0, 0, SCREEN_W, SCREEN_H,
                                       SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
                            : NULL;
    if (!ren) {
        mt_log("[app] window/renderer: %s", SDL_GetError());
        return 1;
    }
    {
        int a = -1;
        SDL_RendererInfo ri;
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
        mt_log("[app] renderer %s, window surface alpha %d bit (requested: %s)",
               SDL_GetRendererInfo(ren, &ri) == 0 ? ri.name : "?", a, want_alpha ? "8" : "default");
    }
    /* Video plane: exported window, filling the screen (webOS 5+). webOS 4's
     * SDL has none - there ACB binds each pipeline to the
     * video plane. Look it up at runtime: a weak symbol
     * does not help, in the program (no PIE) its address is the PLT entry,
     * never NULL - the call would go nowhere. */
    void *self = dlopen(NULL, RTLD_NOW);
    const char *(*create_win)(int) =
        self ? (const char *(*) (int)) dlsym(self, "SDL_webOSCreateExportedWindow") : NULL;
    SDL_bool (*set_win)(const char *, SDL_Rect *, SDL_Rect *) =
        self ? (SDL_bool (*)(const char *, SDL_Rect *, SDL_Rect *)) dlsym(self, "SDL_webOSSetExportedWindow") : NULL;
    void (*destroy_win)(const char *) =
        self ? (void (*)(const char *)) dlsym(self, "SDL_webOSDestroyExportedWindow") : NULL;
    char wid[128] = "";
    if (create_win && set_win && destroy_win) {
        const char *w = create_win(SDL_WEBOS_EXPORED_WINDOW_TYPE_VIDEO);
        if (w) {
            snprintf(wid, sizeof(wid), "%s", w);
            SDL_Rect r = {0, 0, SCREEN_W, SCREEN_H};
            set_win(wid, &r, &r);
        }
        mt_log("[app] window ready, video window %s", wid[0] ? wid : "MISSING");
    } else {
        mt_log("[app] window ready, video via ACB (webOS 4): %s", player_use_acb(app.pl) ? "ok" : "MISSING");
    }
    player_set_window(app.pl, wid);

    {
        /* https (catch-up services): our own, current root certificates - the TV's
         * are from 2011 */
        char ca[600];
        snprintf(ca, sizeof(ca), "%s/ca-certificates.crt", res);
        http_set_ca_file(ca);
    }
    {
        tvsrc_cfg tc = {.htsp_port = cfg_int("tvh.htsp_port"), .http_port = cfg_int("tvh.http_port")};
        snprintf(tc.tvh_host, sizeof(tc.tvh_host), "%.63s", host);
        const cJSON *u = prefs_get("tvh.user"), *pw = prefs_get("tvh.pass");
        if (cJSON_IsString(u)) snprintf(tc.user, sizeof(tc.user), "%s", u->valuestring);
        if (cJSON_IsString(pw)) snprintf(tc.pass, sizeof(tc.pass), "%s", pw->valuestring);
        if (cJSON_GetObjectItemCaseSensitive(g_tvh_test, "user")) {
            snprintf(tc.user, sizeof(tc.user), "%s", jstr(g_tvh_test, "user"));
            snprintf(tc.pass, sizeof(tc.pass), "%s", jstr(g_tvh_test, "pass"));
        }
        pmtinfo_init(tc.tvh_host, tc.http_port, tc.user, tc.pass);
        {
            /* logos and EPG images from tvh's imagecache: with the TV user */
            char pre[160];
            snprintf(pre, sizeof(pre), "http://%s:%d/", tc.tvh_host, tc.http_port);
            ui_image_login(pre, tc.user, tc.pass);
        }
        /* live picture via HTSP subscription: tvh's timeshift, teletext, subtitles and
         * AIT from the subscription; with the dvbbuffer in the tvh fork, start at the keyframe */
        tc.dvr_read_only = dvr_read_only;
        if (dvr_read_only) mt_log("[app] DVR read-only (test): timers and positions are not written");
        const cJSON *ch = prefs_get("tv.channel"), *la = prefs_get("tv.last");
        static const tvsrc_actions sa = {.play = app_play,
                                         .now_next = app_now_next,
                                         .groups_changed = app_groups_changed,
                                         .grid = app_grid,
                                         .schedule = app_schedule,
                                         .connected = app_connected,
                                         .noaccess = app_noaccess};
        static const tvrec_actions ra = {.recordings = app_recordings,
                                         .timers = app_timers,
                                         .play = app_play_recording,
                                         .timer_result = app_timer_result};
        tvsrc_start(&tc, &sa, &ra, cJSON_IsNumber(ch) ? (int) ch->valuedouble : 0,
                    cJSON_IsNumber(la) ? (int) la->valuedouble : 0);
        player_set_htsp_status(app.pl, tvhstat_feed); /* reception info in the banner */
        /* Kore and Home Assistant: Kodi JSON-RPC on the TV */
        kodi_cfg kc = {.http_port = cfg_int("kore.http_port"), .tcp_port = cfg_int("kore.tcp_port"),
                       .tvh_http_port = tc.http_port};
        cfg_get("kore.name", kc.name, sizeof(kc.name));
        cfg_get("kore.user", kc.kore_user, sizeof(kc.kore_user));
        cfg_get("kore.pass", kc.kore_pass, sizeof(kc.kore_pass));
        snprintf(kc.tvh_host, sizeof(kc.tvh_host), "%s", tc.tvh_host);
        snprintf(kc.user, sizeof(kc.user), "%s", tc.user);
        snprintf(kc.pass, sizeof(kc.pass), "%s", tc.pass);
        static const kodi_actions ka = {kodi_channel, kodi_live_paused, kodi_tune,    kodi_step,
                                        kodi_stop,    kodi_key,         kodi_request, kodi_front};
        if (kc.http_port > 0 || kc.tcp_port > 0) {
            if (kodi_start(&kc, &ka, fetch_wake) != 0) mt_log("[app] Kodi remote (Kore) could not start");
            /* for kodi_request: watch power on/off and the foreground app */
            luna_subscribe("luna://com.webos.service.tvpower/power/getPowerState", "{\"subscribe\":true}",
                           on_power_state, NULL);
            luna_subscribe("luna://com.webos.applicationManager/getForegroundAppInfo", "{\"subscribe\":true}",
                           on_foreground_app, NULL);
        }
    }
    {
        /* UI language: launch parameter "lang" (remembered), otherwise the menu
         * language of the TV, otherwise English - before the UI builds its texts */
        const cJSON *lg = prefs_get("ui.lang");
        char lang[16] = "";
        if (cJSON_IsString(lg) && lg->valuestring[0] && strcmp(lg->valuestring, "auto"))
            snprintf(lang, sizeof(lang), "%s", lg->valuestring);
        else tv_ui_language(lang, sizeof(lang));
        i18n_init(res, lang);
    }
    if (ui_init(ren, SCREEN_W, SCREEN_H, res) != 0) return 1;
    subs_actions sa = {subs_now_pts};
    subs_init(&sa); /* first: lies below banner and menus */
    osd_init();
    info_init(); /* above the banner */
    epg_actions ea = {epg_request, epg_tune, epg_record};
    epg_init(&ea);
    recdlg_actions ra = {rd_request, rd_action};
    recdlg_init(&ra);
    recbrowser_actions rba = {rb_request, rb_play, rd_action, rb_closed};
    recbrowser_init(&rba);
    recplay_actions rpa = {rp_exited};
    recplay_init(app.pl, &rpa);
    fetch_sdl_event = SDL_RegisterEvents(1);
    fetch_init(fetch_wake);
    sidedata_init(app.pl); /* teletext/subtitles from the HTSP stream ("local:") */
#if TVF_PMT_FALLBACK
    /* a tvh without the HTSP extensions: stream details from the PMT of the channel */
    pmtinfo_set_callback(on_pmtinfo);
    player_set_pmt_request(app.pl, on_pmt_request, NULL);
#endif
    {
        /* HbbTV in our own renderer: WebKit bundle from its own package
         * com.lsd.tvf.web (too big for TVF's IPK) or a remembered path
         * (web_bundle), data on the developer partition. Egress via
         * TVF's relay according to the country rules (egress.json) */
        hbbweb_cfg hc;
        memset(&hc, 0, sizeof(hc));
        const cJSON *wb = prefs_get("web.bundle"), *eg = prefs_get("hbbtv.egress");
        web_bundle(res, cJSON_IsString(wb) ? wb->valuestring : NULL, hc.bundle, sizeof(hc.bundle));
        snprintf(hc.data_dir, sizeof(hc.data_dir), "/media/developer/temp/com.lsd.tvf/web");
        /* fixed proxy (hbbtvEgress): WebKit goes to it directly only when
         * the relay does not run; otherwise the relay decides per channel
         * (tvh tag before the setting, egress.c) */
        if (cJSON_IsString(eg) && strcmp(eg->valuestring, "rules"))
            snprintf(hc.proxy, sizeof(hc.proxy), "%s", strcmp(eg->valuestring, "direct") ? eg->valuestring : "");
        /* OIPF replacement and dash.js from the package (no gateway): the page fetches
         * dash.js from the relay when needed (hbbweb.c) */
        snprintf(hc.polyfill, sizeof(hc.polyfill), "%s/hbbtv/polyfill.js", res);
        snprintf(hc.user_agent, sizeof(hc.user_agent),
                 "Mozilla/5.0 (Linux; armv7l) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15 "
                 "HbbTV/1.5.1 (+DRM; LGE; webOS; WEBOS6.0; 1; ;)");
        snprintf(hc.media_js, sizeof(hc.media_js), "%s/hbbtv/media.js", res);
        snprintf(hc.drm_js, sizeof(hc.drm_js), "%s/hbbtv/drm.js", res);
        /* country egress (rules as in the HbbTV gateway, but in TVF): for all
         * requests from TVF, WebKit (proxy = relay) and - via the
         * relay - for the media server's videos */
        char egf[600];
        snprintf(egf, sizeof(egf), "%s/hbbtv/egress.json", res);
        /* {host} = the saved server, also with tvhTest: the proxies run next
         * to the real tvh, not next to a test instance */
        egress_load(egf, g_tvh_test && saved_host[0] ? saved_host : host);
        egress_set_mode(cJSON_IsString(eg) ? eg->valuestring : "rules");
        snprintf(egf, sizeof(egf), "%s/hbbtv/entry.json", res);
        hbbtv_load(egf);
        http_set_proxy_resolver(egress_for);
        relay_start(8199);
        {
            char dj[600];
            snprintf(dj, sizeof(dj), "%s/hbbtv/dash.all.min.js", res);
            relay_add_file("hbbtv-dash.js", dj, "text/javascript");
        }
        /* media in WebKit (bundle with "media", setting web.media or
         * WEBMEDIA 1): the sinks in the WebProcess send the streams to the
         * media server, which feeds them into the decoder */
        static const webmedia_actions wma = {hbb_live, hbb_video, hbb_window, hbb_source};
        const cJSON *wm = prefs_get("web.media"), *wct = prefs_get("web.mediactype");
        if (cJSON_IsString(wct)) webmedia_set_ctype(wct->valuestring);
        if (webmedia_init(&wma, fetch_wake, 1280) == 0 && cJSON_IsTrue(wm))
            snprintf(hc.media_sock, sizeof(hc.media_sock), "%s", webmedia_socket());
        static const hbbweb_actions hwa = {hbb_video,     hbb_closed,    hbb_dvb_ids,
                                           hbb_broadcast, hbb_components, hbb_component};
        hbbweb_init(&hc, &hwa, fetch_wake);
        static const hbbmedia_actions hma = {hbb_live, hbbweb_media_update, hbb_window};
        hbbmedia_init(&hma, fetch_wake);
        hbbmedia_set_player(app.pl);
        hbbdrm_init(fetch_wake);
    }
    ttx_actions ta = {ttx_zap, sidedata_ttx_initial_page};
    ttx_init(res, &ta);
    choice_init();
    static const settings_actions sta = {settings_restart, cfg_get, app_find};
    settings_init(&sta); /* topmost */
    if (!host[0]) { /* no server set: look for one in the network */
        g_find_first = 1;
        app_find();
    }
    debugctl_start(debug_port);
    {
        time_t t = time(NULL);
        struct tm lt, ut;
        localtime_r(&t, &lt);
        gmtime_r(&t, &ut);
        mt_log("[app] local time %02d:%02d (UTC %02d:%02d, TZ=%s)", lt.tm_hour, lt.tm_min, ut.tm_hour, ut.tm_min,
               getenv("TZ") ? getenv("TZ") : "-");
    }

    while (!g_quit) {
        uint32_t wait = ui_frame();
        if (wait > 20) wait = 20;
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, (int) wait)) {
            do {
                if (e.type == SDL_QUIT) g_quit = 1;
                else if (e.type == SDL_KEYDOWN) on_sdl_key(&e.key, 1);
                else if (e.type == SDL_KEYUP) on_sdl_key(&e.key, 0);
                else if (e.type == SDL_MOUSEWHEEL) {
                    int y = e.wheel.y;
                    if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
                    if (app.wheel_logged < 6) {
                        app.wheel_logged++;
                        mt_log("[app] wheel y=%d x=%d (direction %u)", e.wheel.y, e.wheel.x, e.wheel.direction);
                    }
                    for (int n = 0; n < (y < 0 ? -y : y) && n < 5; n++) on_wheel(y > 0 ? 1 : -1);
                } else if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) &&
                           e.button.button == SDL_BUTTON_LEFT) {
                    int dn = e.type == SDL_MOUSEBUTTONDOWN;
                    if (app.click_logged < 8) {
                        app.click_logged++;
                        mt_log("[app] mouse %s at %d,%d (pointer %s)", dn ? "down" : "up", e.button.x, e.button.y,
                               app.pointer ? "visible" : "hidden");
                    }
                    app.mouse_at = SDL_GetTicks();
                    app.ok_pending = 0; /* the same press came as a key */
                    on_pointer(dn ? PTR_DOWN : PTR_UP, e.button.x, e.button.y);
                } else if (e.type == SDL_MOUSEMOTION) {
                    if (app.motion_logged < 3) {
                        app.motion_logged++;
                        mt_log("[app] pointer at %d,%d", e.motion.x, e.motion.y);
                    }
                    pointer_visible(1);
                    on_pointer(PTR_MOVE, e.motion.x, e.motion.y);
                }
                else if (e.type == SDL_WINDOWEVENT) on_window(&e.window);
                else if (e.type == SDL_APP_DIDENTERBACKGROUND) go_background("App");
                else if (e.type == SDL_APP_DIDENTERFOREGROUND) go_foreground("App");
            } while (SDL_PollEvent(&e));
        }
        char *m;
        tvsrc_poll();
        if (g_finding) found_servers();
        kodi_poll();
        if (app.ok_pending && SDL_GetTicks() - app.ok_pending > CLICK_DEDUP_MS) {
            app.ok_pending = 0; /* OK key without click: treat as a key */
            on_key(KEY_OK);
        }
        while ((m = debugctl_pop()) != NULL) {
            on_debug(m);
            free(m);
        }
        color_hold_tick();
        ui_poll();
        fetch_poll();
        if (app.digits[0] && SDL_GetTicks() - app.digit_at > DIGIT_TIMEOUT_MS) tune_digits();
        /* event over: fetch new now/next (like the HTML5 app) */
        if (app.now_stop && !app.nn_requested && (long) time(NULL) > app.now_stop) {
            app.nn_requested = 1;
            tvsrc_refresh_now_next();
        }
        player_tick(app.pl);
        /* live stream broken off (tvh restart, network): reconnect, with
         * growing intervals; when time-shifted, back to live */
        /* (not while HbbTV media are playing instead of live: they have the decoder) */
        if (!app.background && !recplay_active() && app.url[0] && !player_paused(app.pl) && !app.hbbtv_live_off) {
            player_status rs;
            player_status_get(app.pl, &rs);
            uint32_t nowt = SDL_GetTicks();
            /* picture watchdog: data is arriving (otherwise the session already ends after
             * 8 s without data), but no new frame */
            int stalled = !rs.ended && rs.picture_age_ms >= STALL_MS;
            if (rs.ended || stalled) {
                if (!app.retry_at) {
                    if (stalled)
                        mt_log("[app] %s for %d s - reconnecting", rs.first_frame ? "picture frozen" : "no picture",
                               rs.picture_age_ms / 1000);
                    app.stalled = stalled;
                    int wait = app.retries < 4 ? 1000 << app.retries : 10000;
                    app.retry_at = nowt + (uint32_t) wait;
                } else if ((int32_t) (nowt - app.retry_at) >= 0) {
                    app.retry_at = 0;
                    app.retries++;
                    mt_log("[app] %s - attempt %d", app.stalled ? "picture frozen" : "stream lost", app.retries);
                    if (tshift_active()) tshift_live();
                    else player_play(app.pl, app.url);
                    player_status_get(app.pl, &rs); /* hint for the new session */
                }
            } else {
                app.retry_at = 0;
                app.stalled = 0;
                if (rs.first_frame) app.retries = 0;
            }
            retry_status(&rs, nowt);
        } else {
            retry_clear(); /* recording, background, pause */
        }
        reception_tick();
        recplay_tick();
        info_tick();
        hbbweb_poll();
        hbb_broadcast_tick();
        hbbmedia_poll();
        webmedia_poll();
        hbbdrm_poll();
        /* after dwelling on a channel, preload its HbbTV app invisibly:
         * Red then shows it immediately; when zapping through nothing happens. With
         * "HbbTV first" it starts visibly - once per channel: after
         * closing it is only preloaded again */
        if (app.hbb_pre_at && (int32_t) (SDL_GetTicks() - app.hbb_pre_at) >= 0 && !app.background) {
            app.hbb_pre_at = 0;
            if (app.side_local && !recplay_active() && !app.background && !hbbweb_active()) {
                if (g_keys_hbbtv && !app.hbb_auto_done) {
                    app.hbb_auto_done = 1;
                    hbbweb_autostart(app.channel, app.chname, hbbtv_rewrites());
                } else {
                    hbbweb_preload(app.channel, app.chname, hbbtv_rewrites());
                }
            }
        }
        /* start the renderer in advance (not in the startup rush) */
        if (!app.hbb_prestarted && SDL_GetTicks() > 8000) {
            app.hbb_prestarted = 1;
            hbbweb_prestart();
        }
        tshift_tick();
        if (osd_visible() && SDL_GetTicks() - app.streams_at > 500) {
            app.streams_at = SDL_GetTicks();
            update_streams();
        }
        osd_tick();
        epg_tick();
        ttx_tick();
        subs_tick();
    }

    mt_log("[app] exit - releasing decoder");
    player_destroy(app.pl);
    ui_shutdown();
    if (wid[0]) destroy_win(wid);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_restart) restart_self(argv[0], argc > 1 ? argv[1] : NULL);
    mt_log("[app] exit");
    return 0;
}
