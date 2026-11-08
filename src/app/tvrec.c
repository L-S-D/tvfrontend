/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tvrec.h"

#include "htsp.h"
#include "http.h"
#include "log.h"
#include "prefs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PUSH_DELAY_S 2     /* batch changes (during a recording they come in series) */
#define POS_EVERY_S 60     /* ongoing playback: write the position at most this often */
#define WATCHED_REST_S 120 /* this little left counts as watched (credits, post-roll padding) */
#define LOCAL_KEY "rec.local"

typedef struct {
    uint32_t id;
    cJSON *m; /* latest state from tvh (dvrEntryAdd + updates) */
} entry;

typedef struct {
    char id[48];
    char name[256];
} series;

typedef struct {
    int seq;
    int kind; /* 'a' timer action, 'p' position */
    char action[16];
    int channel;
    char uuid[40];
    double pos;
    int ended;
    void (*done)(void *user, int ok, const char *err); /* result to the caller, otherwise act.timer_result */
    void *user;
} pending;

static tvrec_cfg cfg;
static entry *ents;
static int nents, cents;
static series *autos;
static int nautos, cautos;
static pending pend[16];
static int watching, dirty;
static time_t dirty_at;
static int sched_ch;
static time_t sched_until;
static int local_only; /* tvh rejected updateDvrEntry: positions only locally */

static struct {
    char uuid[40];
    double pos;
    int paused;
    time_t at;
} lastpos;

/* ---------------- Helpers ---------------- */

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : 0;
}

static int jhas(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k) != NULL; }

static int jtrue(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsTrue(i) || (cJSON_IsNumber(i) && i->valuedouble != 0);
}

static entry *find_id(uint32_t id) {
    for (int i = 0; i < nents; i++)
        if (ents[i].id == id) return &ents[i];
    return NULL;
}

static entry *find_uuid(const char *uuid) {
    if (!uuid || !*uuid) return NULL;
    for (int i = 0; i < nents; i++)
        if (!strcmp(jstr(ents[i].m, "idStr"), uuid)) return &ents[i];
    return NULL;
}

static int active_state(const cJSON *m) {
    const char *s = jstr(m, "state");
    return !strcmp(s, "scheduled") || !strcmp(s, "recording");
}

static entry *find_event(double ev) {
    if (ev <= 0) return NULL;
    for (int i = 0; i < nents; i++)
        if (jnum(ents[i].m, "eventId") == ev && active_state(ents[i].m)) return &ents[i];
    return NULL;
}

static series *find_series(const char *id) {
    for (int i = 0; i < nautos; i++)
        if (!strcmp(autos[i].id, id)) return &autos[i];
    return NULL;
}

static void mark_dirty(void) {
    if (!dirty) dirty_at = time(NULL);
    dirty = 1;
}

/* ---------------- Local positions ---------------- */

static const cJSON *local_get(const char *uuid) {
    return cJSON_GetObjectItemCaseSensitive(prefs_get(LOCAL_KEY), uuid);
}

static void local_set(const char *uuid, double pos, int watched) {
    cJSON *all = cJSON_Duplicate(prefs_get(LOCAL_KEY), 1);
    if (!cJSON_IsObject(all)) {
        cJSON_Delete(all);
        all = cJSON_CreateObject();
    }
    cJSON_DeleteItemFromObjectCaseSensitive(all, uuid);
    if (pos > 0 || watched) { /* start, unwatched = nothing of our own */
        cJSON *e = cJSON_AddObjectToObject(all, uuid);
        cJSON_AddNumberToObject(e, "p", floor(pos));
        cJSON_AddBoolToObject(e, "w", watched);
    }
    prefs_set(LOCAL_KEY, all);
}

/* ---------------- Lists for the app ---------------- */

static void add_channel(cJSON *o, const cJSON *m) {
    int num = 0;
    char name[128] = "", icon[512] = "";
    uint32_t chid = (uint32_t) jnum(m, "channel");
    if (!(chid && cfg.channel && cfg.channel(chid, &num, name, sizeof(name), icon, sizeof(icon))))
        snprintf(name, sizeof(name), "%s", jstr(m, "channelName"));
    cJSON_AddStringToObject(o, "channelName", name);
    if (num) cJSON_AddNumberToObject(o, "channelNumber", num);
    else cJSON_AddNullToObject(o, "channelNumber");
    cJSON_AddStringToObject(o, "icon", icon);
}

static void add_texts(cJSON *o, const cJSON *m) {
    cJSON_AddStringToObject(o, "title", jstr(m, "title"));
    cJSON_AddStringToObject(o, "subtitle", jstr(m, "subtitle"));
    const char *d = jstr(m, "description"), *sum = jstr(m, "summary");
    cJSON_AddStringToObject(o, "description", d[0] ? d : strcmp(sum, jstr(m, "subtitle")) ? sum : "");
}

static int is_audio(const char *t) {
    static const char *const a[] = {"AC3", "EAC3", "AAC", "MP4A", "MPEG2AUDIO", "VORBIS", "OPUS", "FLAC"};
    for (size_t i = 0; i < sizeof(a) / sizeof(a[0]); i++)
        if (!strcmp(t, a[i])) return 1;
    return 0;
}

/* playable recording (finished or in progress, file present) or NULL */
static cJSON *rec_item(const cJSON *m) {
    const char *state = jstr(m, "state"), *uuid = jstr(m, "idStr");
    int recording = !strcmp(state, "recording");
    if ((!recording && strcmp(state, "completed")) || !uuid[0] || !strcmp(jstr(m, "error"), "File missing"))
        return NULL;
    /* /dvrfile delivers the entry's last file (several after a tvh restart
     * during the recording) - so duration/size are taken from that one */
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(m, "files");
    int nf = cJSON_GetArraySize(files);
    const cJSON *file = nf ? cJSON_GetArrayItem(files, nf - 1) : NULL;
    double size = jnum(file, "size");
    if (size <= 0) size = jnum(m, "dataSize");
    if (size <= 0) return NULL;
    double now = (double) time(NULL);
    double fstart = jhas(file, "start") ? jnum(file, "start") : jnum(m, "start");
    double fstop = recording ? fmin(now, jnum(m, "stop")) : jhas(file, "stop") ? jnum(file, "stop") : jnum(m, "stop");
    char video[64] = "";
    const char *audio = "";
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(file, "info")) {
        if (!video[0] && jnum(s, "width") > 0)
            snprintf(video, sizeof(video), "%.0fx%.0f %s", jnum(s, "width"), jnum(s, "height"), jstr(s, "type"));
        if (!audio[0] && is_audio(jstr(s, "type"))) audio = jstr(s, "type");
    }
    double pos = jnum(m, "playposition");
    int watched = jnum(m, "playcount") > 0;
    const cJSON *loc = local_get(uuid); /* our own position (tvh refused) takes precedence */
    if (cJSON_IsObject(loc)) {
        pos = jnum(loc, "p");
        watched = jtrue(loc, "w");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", uuid);
    cJSON_AddNumberToObject(o, "num", jnum(m, "id")); /* HTSP ID (Kodi/Kore address recordings by it) */
    add_texts(o, m);
    add_channel(o, m);
    cJSON_AddNumberToObject(o, "start", jnum(m, "start"));
    cJSON_AddNumberToObject(o, "stop", jnum(m, "stop"));
    cJSON_AddNumberToObject(o, "duration", fmax(0, fstop - fstart));
    cJSON_AddNumberToObject(o, "size", size);
    cJSON_AddStringToObject(o, "state", state);
    cJSON_AddStringToObject(o, "error", jstr(m, "error"));
    cJSON_AddNumberToObject(o, "position", pos);
    cJSON_AddBoolToObject(o, "watched", watched);
    cJSON_AddStringToObject(o, "video", video);
    cJSON_AddStringToObject(o, "audio", audio);
    return o;
}

static cJSON *timer_item(const cJSON *m) {
    if (!active_state(m)) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "timerId", jnum(m, "id"));
    cJSON_AddStringToObject(o, "id", jstr(m, "idStr"));
    if (jnum(m, "eventId") > 0) cJSON_AddNumberToObject(o, "eventId", jnum(m, "eventId"));
    else cJSON_AddNullToObject(o, "eventId");
    add_texts(o, m);
    add_channel(o, m);
    cJSON_AddNumberToObject(o, "start", jnum(m, "start"));
    cJSON_AddNumberToObject(o, "stop", jnum(m, "stop"));
    cJSON_AddStringToObject(o, "state", jstr(m, "state"));
    const char *ar = jstr(m, "autorecId");
    const series *se = ar[0] ? find_series(ar) : NULL;
    if (ar[0]) cJSON_AddStringToObject(o, "autorecId", ar);
    else cJSON_AddNullToObject(o, "autorecId");
    cJSON_AddStringToObject(o, "seriesTitle", se ? se->name : "");
    return o;
}

static int by_start_desc(const void *a, const void *b) {
    double x = jnum(*(cJSON *const *) a, "start"), y = jnum(*(cJSON *const *) b, "start");
    return x < y ? 1 : x > y ? -1 : 0;
}

static int by_start_asc(const void *a, const void *b) { return -by_start_desc(a, b); }

static cJSON *build_list(int timers) {
    cJSON **v = nents ? calloc((size_t) nents, sizeof(cJSON *)) : NULL;
    int n = 0;
    for (int i = 0; v && i < nents; i++) {
        cJSON *it = timers ? timer_item(ents[i].m) : rec_item(ents[i].m);
        if (it) v[n++] = it;
    }
    if (n > 1) qsort(v, (size_t) n, sizeof(cJSON *), timers ? by_start_asc : by_start_desc);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, v[i]);
    free(v);
    return arr;
}

static void send_list(int timers) {
    void (*fn)(cJSON *) = timers ? cfg.act.timers : cfg.act.recordings;
    if (fn) fn(build_list(timers));
}

/* ---------------- tvh messages ---------------- */

void tvrec_init(const tvrec_cfg *c) { cfg = *c; }

void tvrec_clear(void) {
    for (int i = 0; i < nents; i++) cJSON_Delete(ents[i].m);
    nents = 0;
    nautos = 0;
    /* replies on the old connection will not arrive any more */
    for (size_t i = 0; i < sizeof(pend) / sizeof(pend[0]); i++) {
        pend[i].seq = 0;
    }
}

static void put_entry(const cJSON *m, int merge) {
    uint32_t id = (uint32_t) jnum(m, "id");
    if (!id) return;
    entry *e = find_id(id);
    if (!e) {
        if (nents == cents) {
            int nc = cents ? cents * 2 : 64;
            entry *ne = realloc(ents, (size_t) nc * sizeof(entry));
            if (!ne) return;
            ents = ne;
            cents = nc;
        }
        e = &ents[nents++];
        e->id = id;
        e->m = NULL;
        merge = 0;
    }
    int relevant = !merge;
    if (!merge) {
        cJSON_Delete(e->m);
        e->m = cJSON_Duplicate(m, 1);
    } else {
        /* update: only the changed fields; only dataSize/files (recording in
         * progress, every few seconds) is not worth mentioning for the lists */
        const cJSON *f;
        cJSON_ArrayForEach(f, m) {
            if (!f->string) continue;
            if (strcmp(f->string, "method") && strcmp(f->string, "id") && strcmp(f->string, "dataSize") &&
                strcmp(f->string, "files") && strcmp(f->string, "duplicate")) {
                const cJSON *old = cJSON_GetObjectItemCaseSensitive(e->m, f->string);
                if (!old || !cJSON_Compare(old, f, 1)) relevant = 1;
            }
            cJSON *cp = cJSON_Duplicate(f, 1);
            if (cJSON_GetObjectItemCaseSensitive(e->m, f->string)) cJSON_ReplaceItemInObjectCaseSensitive(e->m, f->string, cp);
            else cJSON_AddItemToObject(e->m, f->string, cp);
        }
    }
    if (relevant) mark_dirty();
}

void tvrec_htsp(const cJSON *m) {
    const char *meth = jstr(m, "method");
    if (!strcmp(meth, "dvrEntryAdd")) put_entry(m, 0);
    else if (!strcmp(meth, "dvrEntryUpdate")) put_entry(m, 1);
    else if (!strcmp(meth, "dvrEntryDelete")) {
        entry *e = find_id((uint32_t) jnum(m, "id"));
        if (e) {
            cJSON_Delete(e->m);
            *e = ents[--nents];
            mark_dirty();
        }
    } else if (!strcmp(meth, "autorecEntryAdd") || !strcmp(meth, "autorecEntryUpdate")) {
        const char *id = jstr(m, "id");
        if (!id[0]) return;
        series *s = find_series(id);
        if (!s) {
            if (nautos == cautos) {
                int nc = cautos ? cautos * 2 : 16;
                series *ns = realloc(autos, (size_t) nc * sizeof(series));
                if (!ns) return;
                autos = ns;
                cautos = nc;
            }
            s = &autos[nautos++];
            snprintf(s->id, sizeof(s->id), "%s", id);
            s->name[0] = 0;
        }
        const char *nm = jstr(m, "name")[0] ? jstr(m, "name") : jstr(m, "title");
        if (nm[0]) snprintf(s->name, sizeof(s->name), "%s", nm);
        mark_dirty();
    } else if (!strcmp(meth, "autorecEntryDelete")) {
        series *s = find_series(jstr(m, "id"));
        if (s) {
            *s = autos[--nautos];
            mark_dirty();
        }
    }
}

/* ---------------- Requests to tvh ---------------- */

static pending *pend_new(int seq, int kind) {
    for (size_t i = 0; i < sizeof(pend) / sizeof(pend[0]); i++)
        if (!pend[i].seq) {
            memset(&pend[i], 0, sizeof(pend[i]));
            pend[i].seq = seq;
            pend[i].kind = kind;
            return &pend[i];
        }
    return NULL;
}

static void timer_result(const char *action, int ok, const char *err, int channel,
                         void (*done)(void *, int, const char *), void *user) {
    if (!ok) mt_log("[tvrec] timer %s failed: %s", action, err ? err : "?");
    else mt_log("[tvrec] timer %s ok", action);
    if (done) { /* Kore */
        done(user, ok, err ? err : "");
        if (channel > 0 && cfg.schedule) cfg.schedule(channel);
        return;
    }
    if (cfg.act.timer_result) cfg.act.timer_result(action, ok, err ? err : "");
    if (channel > 0 && cfg.schedule) {
        cfg.schedule(channel);
        /* the change from tvh may arrive after the reply: then once more */
        sched_ch = channel;
        sched_until = time(NULL) + 5;
    }
}

/* title as an exact regular expression */
static void regex_exact(const char *t, char *out, size_t n) {
    size_t o = 0;
    out[o++] = '^';
    for (; *t && o + 3 < n; t++) {
        if (strchr(".*+?^${}()|[]\\", *t)) out[o++] = '\\';
        out[o++] = *t;
    }
    out[o++] = '$';
    out[o] = 0;
}

void tvrec_action(const char *action, uint32_t ev, uint32_t timer, const char *autorec, int channel,
                  void (*done)(void *user, int ok, const char *err), void *user) {
    const char *method = NULL;
    cJSON *args = cJSON_CreateObject();
    const char *err = NULL;
    if (!strcmp(action, "record")) {
        if (!ev) err = "no event";
        else if (find_event(ev)) { /* already scheduled */
            cJSON_Delete(args);
            timer_result(action, 1, NULL, channel, done, user);
            return;
        } else {
            method = "addDvrEntry";
            cJSON_AddNumberToObject(args, "eventId", ev);
        }
    } else if (!strcmp(action, "cancel")) {
        entry *e = timer ? find_id(timer) : NULL;
        if (!e) e = find_event(ev);
        if (!e) err = "timer not found";
        else {
            method = !strcmp(jstr(e->m, "state"), "recording") ? "stopDvrEntry" : "cancelDvrEntry";
            cJSON_AddNumberToObject(args, "id", e->id);
        }
    } else if (!strcmp(action, "recordSeries")) {
        char title[256], re[600];
        uint32_t chid = 0;
        if (!ev || !cfg.event || !cfg.event(ev, title, sizeof(title), &chid) || !title[0])
            err = "event not found";
        else {
            regex_exact(title, re, sizeof(re));
            method = "addAutorecEntry";
            cJSON_AddStringToObject(args, "title", re);
            cJSON_AddStringToObject(args, "name", title);
            cJSON_AddNumberToObject(args, "channelId", chid);
            cJSON_AddNumberToObject(args, "fulltext", 0);
            cJSON_AddStringToObject(args, "comment", "Created by TVF");
        }
    } else if (!strcmp(action, "cancelSeries")) {
        const char *ar = autorec ? autorec : "";
        if (!ar[0] || !find_series(ar)) err = "series not found";
        else {
            method = "deleteAutorecEntry";
            cJSON_AddStringToObject(args, "id", ar);
        }
    } else {
        err = "unknown action";
    }
    if (method && cfg.read_only) {
        char *a = cJSON_PrintUnformatted(args);
        mt_log("[tvrec] read-only: %s %s not sent", method, a ? a : "");
        free(a);
        err = "read-only (test)";
        method = NULL;
    }
    int seq = method ? htsp_request(method, args) : -1;
    cJSON_Delete(args);
    if (method && seq < 0) err = "HTSP not connected";
    pending *p = seq >= 0 ? pend_new(seq, 'a') : NULL;
    if (!p) {
        timer_result(action, 0, err ? err : "too many requests", channel, done, user);
        return;
    }
    mt_log("[tvrec] %s (%s)", method, action);
    snprintf(p->action, sizeof(p->action), "%s", action);
    p->channel = channel;
    p->done = done;
    p->user = user;
}

static void save_position(const char *uuid, double pos, int ended) {
    entry *e = find_uuid(uuid);
    if (!e) return;
    if (ended) pos = 0;
    if (cfg.read_only) {
        mt_log("[tvrec] read-only: position %s %.0f s%s not saved", uuid, pos, ended ? " (watched)" : "");
        return;
    }
    if (local_only) {
        local_set(uuid, pos, ended);
        return;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "id", e->id);
    cJSON_AddNumberToObject(args, "playposition", floor(pos));
    if (ended) cJSON_AddNumberToObject(args, "playcount", 1); /* HTSP_DVR_PLAYCOUNT_SET */
    int seq = htsp_request("updateDvrEntry", args);
    cJSON_Delete(args);
    pending *p = seq >= 0 ? pend_new(seq, 'p') : NULL;
    if (!p) {
        local_set(uuid, pos, ended);
        return;
    }
    snprintf(p->uuid, sizeof(p->uuid), "%s", uuid);
    p->pos = pos;
    p->ended = ended;
}

void tvrec_position(const char *uuid, double pos, double dur, int paused, int stopped, int ended) {
    if (!uuid || !uuid[0]) return;
    if (dur > 0 && dur - pos < WATCHED_REST_S) ended = 1; /* dur 0: recording still in progress */
    time_t now = time(NULL);
    int same = !strcmp(uuid, lastpos.uuid);
    /* during playback write only rarely; pause, end, stop immediately */
    if (same && !ended && !stopped && paused == lastpos.paused &&
        (now - lastpos.at < POS_EVERY_S || fabs(pos - lastpos.pos) < 5))
        return;
    snprintf(lastpos.uuid, sizeof(lastpos.uuid), "%s", uuid);
    lastpos.pos = pos;
    lastpos.paused = paused;
    lastpos.at = now;
    save_position(uuid, pos, ended);
}

int tvrec_reply(int seq, const cJSON *m) {
    pending *p = NULL;
    for (size_t i = 0; i < sizeof(pend) / sizeof(pend[0]) && !p; i++)
        if (pend[i].seq == seq) p = &pend[i];
    if (!p) return 0;
    const char *err = jstr(m, "error");
    /* without permission tvh replies only {"noaccess":1} (no error, no success) */
    if (!err[0] && jhas(m, "noaccess") && jnum(m, "noaccess") != 0) err = "no permission for this tvh user";
    int ok = !err[0] && (!jhas(m, "success") || jnum(m, "success") != 0);
    if (p->kind == 'a') {
        timer_result(p->action, ok, ok ? NULL : err[0] ? err : "failed", p->channel, p->done, p->user);
    } else if (ok) {
        mt_log("[tvrec] position %s %.0f s%s saved in tvh", p->uuid, p->pos, p->ended ? " (watched)" : "");
        if (local_get(p->uuid)) local_set(p->uuid, -1, 0); /* tvh has it now: drop it locally */
    } else {
        if (!local_only)
            mt_log("[tvrec] tvh refuses updateDvrEntry (%s) - positions are kept on the TV", err[0] ? err : "?");
        local_only = 1;
        local_set(p->uuid, p->pos, p->ended);
        mark_dirty();
    }
    p->seq = 0;
    return 1;
}

/* ---------------- Requests from the app ---------------- */

int tvrec_play_uuid(const char *uuid, int from_start) {
    entry *e = find_uuid(uuid);
    cJSON *it = e ? rec_item(e->m) : NULL;
    if (!it) {
        mt_log("[tvrec] recording %s not found", uuid);
        send_list(0);
        return 0;
    }
    if (from_start || jtrue(it, "watched"))
        cJSON_ReplaceItemInObjectCaseSensitive(it, "position", cJSON_CreateNumber(0));
    char ui[400], url[1024];
    http_url_userinfo(cfg.user, cfg.pass, ui, sizeof(ui));
    snprintf(url, sizeof(url), "http://%s%s:%d/dvrfile/%s", ui, cfg.host, cfg.http_port, uuid);
    cJSON_AddStringToObject(it, "fileUrl", url);
    mt_log("[tvrec] play %s \"%s\" from %.0f s", uuid, jstr(it, "title"), jnum(it, "position"));
    if (cfg.act.play) cfg.act.play(it);
    else cJSON_Delete(it);
    return 1;
}

void tvrec_request_list(int timers) {
    watching = 1;
    send_list(timers);
}

void tvrec_poll(int synced) {
    if (!dirty || !synced) return;
    time_t now = time(NULL);
    if (sched_ch && now <= sched_until && cfg.schedule) {
        cfg.schedule(sched_ch);
        sched_ch = 0;
    }
    if (now - dirty_at < PUSH_DELAY_S) return;
    dirty = 0;
    if (!watching) return;
    send_list(0);
    send_list(1);
}

/* ---------------- For Kodi/Kore (kodi.c) ---------------- */

cJSON *tvrec_list(int timers) { return build_list(timers); }

cJSON *tvrec_recording(uint32_t num) {
    entry *e = find_id(num);
    return e ? rec_item(e->m) : NULL;
}

int tvrec_timer_of_event(uint32_t event, uint32_t *timer, char *autorec, size_t n, int *recording) {
    entry *e = find_event(event);
    if (!e) return 0;
    if (timer) *timer = e->id;
    if (autorec) snprintf(autorec, n, "%s", jstr(e->m, "autorecId"));
    if (recording) *recording = !strcmp(jstr(e->m, "state"), "recording");
    return 1;
}

int tvrec_play(uint32_t num, int from_start) {
    entry *e = find_id(num);
    return e ? tvrec_play_uuid(jstr(e->m, "idStr"), from_start) : 0;
}
