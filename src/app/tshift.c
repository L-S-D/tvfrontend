/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tshift.h"
#include "i18n.h"

#include "log.h"
#include "osd.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#define SEEK_DELAY_MS 700 /* collect key presses (each jump costs a restart) */
#define LIVE_GAP_S 4      /* closer than this to live: live */
#define SHOW_MS 5000      /* playing time-shifted: display after the last action; while paused it stays */

static player *pl;

/* tvh's timeshift in the running HTSP subscription: pause, jumps and back to
 * live as commands; the buffer lives in tvh */
static struct {
    int htsp;              /* current channel is an HTSP subscription */
    int pending_s;         /* collected jump (s), sent after SEEK_DELAY_MS */
    long long pause_shift; /* behind live at the pause (90 kHz) */
    char live_url[1024];
    int paused;
    uint32_t pause_at;
    uint32_t seek_at;
    uint32_t shown_at;   /* last drawn, 0 = hidden */
    uint32_t show_until; /* playing time-shifted: visible until then */
} ts;

void tshift_init(player *p) { pl = p; }

static void fmt_behind(double s, char *out, size_t n) {
    long x = s > 0 ? (long) (s + 0.5) : 0;
    if (x >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", x / 3600, x % 3600 / 60, x % 60);
    else snprintf(out, n, "%ld:%02ld", x / 60, x % 60);
}

/* behind live according to tvh; while paused the gap grows with time */
static double behind_s(void) {
    player_tshift st;
    double b = player_htsp_timeshift(pl, &st) && st.have_status ? st.shift90 / 90000.0 : 0;
    if (ts.paused) {
        double p = ts.pause_shift / 90000.0 + (SDL_GetTicks() - ts.pause_at) / 1000.0;
        if (p > b) b = p;
    }
    b -= ts.pending_s; /* collected jump (back = negative) */
    return b > 0 ? b : 0;
}

int tshift_active(void) { return ts.htsp && (ts.paused || ts.pending_s || behind_s() > LIVE_GAP_S); }

static int can_shift(void) {
    player_tshift st;
    return ts.htsp && player_htsp_timeshift(pl, &st);
}

int tshift_available(void) { return can_shift(); }

/* display "−m:ss behind live": always while paused and during a jump,
 * when playing time-shifted only for SHOW_MS after the last action */
static int visible(void) {
    if (ts.paused || ts.pending_s) return 1;
    return tshift_active() && (int32_t) (SDL_GetTicks() - ts.show_until) < 0;
}

static void render(void) {
    if (!visible()) {
        osd_tshift(NULL, NULL);
        ts.shown_at = 0;
        return;
    }
    char b[32], l1[96];
    fmt_behind(behind_s(), b, sizeof(b));
    if (ts.paused) snprintf(l1, sizeof(l1), _("Paused   \xE2\x88\x92%s behind live"), b);
    else snprintf(l1, sizeof(l1), _("\xE2\x88\x92%s behind live"), b);
    osd_tshift(l1, ts.paused ? _("Left \xE2\x88\x92" "30 s \xC2\xB7 Right +30 s \xC2\xB7 OK resume \xC2\xB7 Back live")
                             : _("Left \xE2\x88\x92" "30 s \xC2\xB7 Right +30 s \xC2\xB7 OK pause \xC2\xB7 Back live"));
    ts.shown_at = SDL_GetTicks();
}

/* after an action (pause, resume, jump): show */
static void show(void) {
    ts.show_until = SDL_GetTicks() + SHOW_MS;
    render();
}

void tshift_on_play(const char *live_url, int enabled) {
    ts.paused = 0;
    ts.pending_s = 0;
    snprintf(ts.live_url, sizeof(ts.live_url), "%s", live_url ? live_url : "");
    osd_tshift(NULL, NULL);
    ts.htsp = enabled && live_url && !strncmp(live_url, "htsp://", 7);
}

void tshift_stop(void) {
    ts.pending_s = 0;
    ts.paused = 0;
    osd_tshift(NULL, NULL);
}

void tshift_live(void) {
    if (!ts.htsp) return;
    int was = ts.paused || ts.pending_s || behind_s() > 1;
    ts.pending_s = 0;
    if (ts.paused) player_set_paused(pl, 0);
    ts.paused = 0;
    player_tshift st;
    if (!player_htsp_timeshift(pl, &st)) { /* subscription gone (aborted): reconnect */
        if (ts.live_url[0]) player_play_keep_audio(pl, ts.live_url);
    } else if (was) {
        mt_log("[tshift] back to live (tvh)");
        player_htsp_live(pl);
    }
    if (was) osd_message(_("Live"), 2000, 0);
    osd_tshift(NULL, NULL);
}

void tshift_toggle_pause(void) {
    if (!can_shift()) {
        osd_message(_("No timeshift for this channel"), 3000, 0);
        return;
    }
    if (!ts.paused) {
        player_tshift st;
        ts.pause_shift = player_htsp_timeshift(pl, &st) && st.have_status ? st.shift90 : 0;
        ts.pause_at = SDL_GetTicks();
        ts.paused = 1;
        player_htsp_speed(pl, 0); /* tvh stops, keeps buffering */
        player_set_paused(pl, 1);
        mt_log("[tshift] paused (tvh), %.1f s behind live", ts.pause_shift / 90000.0);
    } else {
        ts.paused = 0;
        player_htsp_speed(pl, 100);
        player_set_paused(pl, 0);
        mt_log("[tshift] resumed (tvh)");
    }
    show();
}

void tshift_seek(int delta_s) {
    if (!can_shift()) {
        osd_message(_("No timeshift for this channel"), 3000, 0);
        return;
    }
    /* collect, after SEEK_DELAY_MS one jump in tvh; forward up to live = live */
    double before = behind_s();
    if (delta_s > 0 && before < 1 && !ts.paused) return; /* already live */
    ts.pending_s += delta_s;
    if (behind_s() < LIVE_GAP_S && ts.pending_s > 0) {
        tshift_live();
        return;
    }
    ts.seek_at = SDL_GetTicks() + SEEK_DELAY_MS;
    show();
}

void tshift_tick(void) {
    uint32_t now = SDL_GetTicks();
    if (ts.htsp && ts.pending_s && (int32_t) (now - ts.seek_at) >= 0) {
        int d = ts.pending_s;
        ts.pending_s = 0;
        if (ts.paused) { /* a jump resumes playback (as before) */
            ts.paused = 0;
            player_htsp_speed(pl, 100);
        }
        mt_log("[tshift] jump %+d s (tvh)", d);
        player_htsp_skip(pl, (long long) d * 90000);
        show();
    }
    /* visible: redraw every second (the time behind live keeps running); otherwise gone */
    if (visible()) {
        if (now - ts.shown_at >= 1000) render();
    } else if (ts.shown_at) {
        ts.shown_at = 0;
        osd_tshift(NULL, NULL);
    }
}
