/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recplay.h"
#include "i18n.h"

#include "keys.h"
#include "log.h"
#include "osd.h"
#include "recbrowser.h"
#include "recindex.h"
#include "tvrec.h"
#include "tvsrc.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEEK_DELAY_MS 700     /* collect key presses (each jump costs a restart) */
#define REPORT_MS 10000
#define INDEX_WAIT_MS 6000    /* resume: wait this long for the index */

static player *pl;
static recplay_actions act;

static struct {
    int active;
    char id[64];
    uint32_t num; /* HTSP ID (Kore), 0 = unknown */
    char file_url[1024];
    char title[256], subtitle[256], channel[128], icon[512], plot[1024];
    long start;
    double duration;   /* according to tvh; the index (PCR) is more accurate */
    int recording;     /* recording still running: end = state at load time */
    recindex *index;
    double pending_pos; /* start waits for the index (-1 = no) */
    uint32_t pending_at;
    int started;       /* first frame of the session is there */
    int begun;         /* playback has run once (jumps/reports allowed) */
    int want_paused;   /* pause again after a jump while paused */
    double seek_target; /* collected jump target, -1 = none */
    uint32_t seek_at;
    uint32_t report_at, osd_at;
    double last_pos;
    int suspended;
    int dragging;      /* pointer is dragging on the bar */
    uint32_t ptr_show_at;
} rp;

void recplay_init(player *p, const recplay_actions *a) {
    pl = p;
    act = *a;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : 0;
}

static double duration(void) {
    double d = recindex_duration(rp.index);
    return d > 0 ? d : rp.duration;
}

static double position(void) {
    if (rp.seek_target >= 0) return rp.seek_target;
    double p = player_position(pl);
    if (p < 0) p = 0;
    rp.last_pos = p;
    return p;
}

static void send_position(int stopped, int ended) {
    if (!rp.active || !rp.begun) return;
    /* recording in progress: duration 0 (still growing) */
    tvrec_position(rp.id, (long) position(), rp.recording ? 0 : (long) duration(), player_paused(pl), stopped, ended);
    rp.report_at = SDL_GetTicks();
}

static void show_progress(void) {
    const char *st = rp.seek_target >= 0 ? _("SEEK") : player_paused(pl) ? _("PAUSE") : "";
    osd_rec_progress(position(), duration(), st);
}

/* file from time t (s): segment from the index, otherwise from the start */
static void play_at(double t) {
    long long off = 0;
    double seg_t = 0;
    if (t > 0 && recindex_lookup(rp.index, t, &off, &seg_t) != 0) {
        mt_log("[rec] no index - starting at the beginning instead of %.0f s", t);
        off = 0;
        seg_t = 0;
    }
    mt_log("[rec] start at %.0f s: segment from %.1f s, byte %lld", t, seg_t, off);
    rp.started = 0;
    player_play_file(pl, rp.file_url, off, seg_t);
}

void recplay_start(const cJSON *msg) {
    if (rp.active) recplay_exit();
    memset(&rp, 0, sizeof(rp));
    rp.active = 1;
    snprintf(rp.id, sizeof(rp.id), "%s", jstr(msg, "id"));
    rp.num = (uint32_t) jnum(msg, "num");
    snprintf(rp.file_url, sizeof(rp.file_url), "%s", jstr(msg, "fileUrl"));
    snprintf(rp.title, sizeof(rp.title), "%s", jstr(msg, "title"));
    snprintf(rp.subtitle, sizeof(rp.subtitle), "%s", jstr(msg, "subtitle"));
    snprintf(rp.channel, sizeof(rp.channel), "%s", jstr(msg, "channelName"));
    snprintf(rp.icon, sizeof(rp.icon), "%s", jstr(msg, "icon"));
    snprintf(rp.plot, sizeof(rp.plot), "%s", jstr(msg, "description"));
    rp.start = (long) jnum(msg, "start");
    rp.duration = jnum(msg, "duration");
    rp.recording = !strcmp(jstr(msg, "state"), "recording");
    rp.seek_target = -1;
    rp.pending_pos = -1;
    rp.index = rp.file_url[0] ? recindex_fetch(rp.file_url) : NULL;
    double pos = jnum(msg, "position");
    mt_log("[rec] recording %s \"%s\" from %.0f s, duration %.0f s%s", rp.id, rp.title, pos, rp.duration,
           rp.recording ? " (still recording)" : "");
    osd_rec_mode(rp.title, rp.subtitle, rp.plot, rp.channel, rp.start, rp.icon);
    osd_rec_progress(pos, rp.duration, "");
    osd_show(5000);
    if (pos > 0 && rp.index) {
        rp.pending_pos = pos; /* wait for the index (recplay_tick) */
        rp.pending_at = SDL_GetTicks();
        player_stop(pl);
        osd_message(_("Loading recording \xE2\x80\xA6"), 0, 0);
    } else {
        play_at(0);
    }
}

int recplay_active(void) { return rp.active; }

int recplay_info(uint32_t *num, double *pos, double *dur, int *paused) {
    if (!rp.active) return 0;
    if (num) *num = rp.num;
    if (pos) *pos = position();
    if (dur) *dur = duration();
    if (paused) *paused = player_paused(pl) || rp.want_paused;
    return 1;
}

void recplay_exit(void) {
    if (!rp.active) return;
    send_position(1, 0);
    player_stop(pl);
    recindex_free(rp.index);
    rp.index = NULL;
    rp.active = 0;
    osd_live_mode();
    osd_hide();
    osd_message(NULL, 0, 0);
}

void recplay_report(void) { send_position(0, 0); }

static void seek_to(double t, int delay_ms) {
    if (!rp.begun) return;
    double d = duration();
    if (d > 0 && t > d - 5) t = d - 5;
    if (t < 0) t = 0;
    rp.seek_target = t;
    rp.seek_at = SDL_GetTicks() + (uint32_t) delay_ms;
    show_progress();
    osd_show(5000);
}

static void seek_by(double delta) { seek_to(position() + delta, SEEK_DELAY_MS); }

static void set_paused(int p) {
    player_set_paused(pl, p);
    rp.want_paused = p;
    show_progress();
    osd_show(p ? 0 : 3000); /* paused: banner stays */
    send_position(0, 0);
}

void recplay_control(const cJSON *msg) {
    if (!rp.active) return;
    const char *a = jstr(msg, "action");
    if (!strcmp(a, "pause")) set_paused(1);
    else if (!strcmp(a, "play")) set_paused(0);
    else if (!strcmp(a, "seek")) seek_to(jnum(msg, "position"), 0);
}

int recplay_key(int key) {
    if (!rp.active) return 0;
    switch (key) {
    case KEY_OK:
    case KEY_PLAYPAUSE: set_paused(!player_paused(pl)); break;
    case KEY_PLAY: set_paused(0); break;
    case KEY_PAUSE: set_paused(1); break;
    case KEY_INFO:
        if (osd_visible()) osd_hide();
        else {
            show_progress();
            osd_show(5000);
        }
        break;
    case KEY_LEFT: seek_by(-10); break;
    case KEY_RIGHT: seek_by(30); break;
    case KEY_DOWN: seek_by(-300); break;
    case KEY_UP: seek_by(300); break;
    case KEY_REWIND: seek_by(-60); break;
    case KEY_FFWD: seek_by(60); break;
    case KEY_BACK:
    case KEY_STOP: {
        char id[64];
        snprintf(id, sizeof(id), "%s", rp.id);
        recplay_exit();
        if (act.exited) act.exited(id);
        break;
    }
    case KEY_CH_UP: tvsrc_step(1); break; /* tvsrc replies with play */
    case KEY_CH_DOWN: tvsrc_step(-1); break;
    default: return 0;
    }
    return 1;
}

static void hover(double frac) {
    char t[32];
    rec_fmt_duration(frac * duration(), t, sizeof(t));
    osd_seek_hover(frac, t);
}

int recplay_pointer(int ev, int x, int y) {
    if (!rp.active) return 0;
    uint32_t now = SDL_GetTicks();
    double d = duration(), frac = 0;
    int can = rp.begun && d > 0 && osd_visible();
    if (ev == PTR_MOVE) {
        if (now - rp.ptr_show_at > 500) { /* show the controls, but not on every movement */
            rp.ptr_show_at = now;
            show_progress();
            osd_show(player_paused(pl) ? 0 : 5000);
        }
        if (rp.dragging && osd_bar_frac(x, y, 1, &frac)) {
            rp.seek_target = frac * d; /* preview only: the jump happens on release */
            rp.seek_at = now + 3600 * 1000;
            show_progress();
            hover(frac);
        } else if (can && osd_bar_frac(x, y, 0, &frac)) {
            hover(frac);
        } else {
            osd_seek_hover(-1, NULL);
        }
        return 1;
    }
    if (ev == PTR_DOWN) {
        if (can && osd_bar_frac(x, y, 0, &frac)) {
            rp.dragging = 1;
            rp.seek_target = frac * d;
            rp.seek_at = now + 3600 * 1000;
            show_progress();
            hover(frac);
        }
        return 1;
    }
    /* PTR_UP */
    if (rp.dragging) {
        rp.dragging = 0;
        osd_bar_frac(x, y, 1, &frac);
        osd_seek_hover(-1, NULL);
        seek_to(frac * d, 0);
        return 1;
    }
    if (!osd_visible()) { /* a click into empty space first brings up the controls */
        show_progress();
        osd_show(player_paused(pl) ? 0 : 5000);
    } else {
        set_paused(!player_paused(pl));
    }
    return 1;
}

void recplay_pointer_gone(void) {
    osd_seek_hover(-1, NULL);
    if (!rp.dragging) return;
    rp.dragging = 0;
    rp.seek_target = -1; /* discard the preview */
    show_progress();
}

void recplay_wheel(int dir) {
    if (rp.active) seek_by(dir > 0 ? 30 : -30);
}

void recplay_select_audio(int pid) {
    if (!rp.active) return;
    double t = position();
    long long off = 0;
    double seg_t = 0;
    if (recindex_lookup(rp.index, t, &off, &seg_t) != 0) {
        off = 0;
        seg_t = 0;
    }
    rp.started = 0;
    player_select_audio_file(pl, pid, off, seg_t);
}

void recplay_suspend(void) {
    if (!rp.active || rp.suspended) return;
    rp.suspended = 1;
    rp.last_pos = position();
    send_position(0, 0);
    player_stop(pl);
}

void recplay_resume(void) {
    if (!rp.active || !rp.suspended) return;
    rp.suspended = 0;
    play_at(rp.last_pos);
}

void recplay_tick(void) {
    if (!rp.active || rp.suspended) return;
    uint32_t now = SDL_GetTicks();

    /* resume: start as soon as the index is there */
    if (rp.pending_pos >= 0) {
        int st = recindex_state(rp.index);
        if (st != 0 || now - rp.pending_at > INDEX_WAIT_MS) {
            double p = rp.pending_pos;
            rp.pending_pos = -1;
            osd_message(NULL, 0, 0);
            play_at(st == 1 ? p : 0);
        }
        return;
    }

    player_status s;
    player_status_get(pl, &s);
    if (!rp.started && s.first_frame) {
        rp.started = 1;
        rp.begun = 1;
        if (rp.want_paused) player_set_paused(pl, 1);
    }

    if (rp.seek_target >= 0 && (int32_t) (now - rp.seek_at) >= 0) {
        double t = rp.seek_target;
        rp.seek_target = -1;
        mt_log("[rec] seek to %.0f s", t);
        play_at(t);
        rp.last_pos = t;
    }

    if (rp.started && !player_paused(pl) && now - rp.report_at >= REPORT_MS) send_position(0, 0);

    if (osd_visible() && now - rp.osd_at >= 500) {
        rp.osd_at = now;
        show_progress();
    }

    int end = rp.started ? player_ended(pl) : 0;
    if (end) {
        mt_log(end == 2 ? "[rec] connection to tvh lost - playback stopped" : "[rec] end of recording");
        char id[64];
        snprintf(id, sizeof(id), "%s", rp.id);
        /* recording in progress: end of file is only the state at load time, not "watched";
         * aborted (network): only remember the position */
        send_position(rp.recording || end == 2, !rp.recording && end == 1);
        player_stop(pl);
        recindex_free(rp.index);
        rp.index = NULL;
        rp.active = 0;
        osd_live_mode();
        osd_hide();
        if (act.exited) act.exited(id);
        if (end == 2) osd_message(_("Connection to the server lost - the position is saved"), 4000, 1);
    }
}
