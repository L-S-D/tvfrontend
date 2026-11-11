/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* memmem */
#include "tvsrc.h"
#include "tvrec.h"
#include "egress.h"
#include "i18n.h"

#include "htsp.h"
#include "log.h"
#include "osd.h"
#include "prefs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EPG_WINDOW (12 * 3600) /* events pushed this far ahead */
#define PRUNE_S 300            /* clear out expired events every 5 min */
#define KEEP_PAST 7200         /* ... but keep the last 2 h */

/* ---------------- Hash table uint32 -> pointer ---------------- */

typedef struct {
    uint32_t *k;
    void **v;
    size_t cap, n, used; /* used = n + tombstones */
} u32map;

#define TOMB ((void *) 1)

static size_t h32(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7feb352dU;
    k ^= k >> 15;
    k *= 0x846ca68bU;
    k ^= k >> 16;
    return k;
}

static void *map_get(const u32map *m, uint32_t k) {
    if (!m->cap) return NULL;
    for (size_t i = h32(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->v[i]) return NULL;
        if (m->v[i] != TOMB && m->k[i] == k) return m->v[i];
    }
}

static void map_put(u32map *m, uint32_t k, void *v);

static void map_grow(u32map *m) {
    u32map o = *m;
    m->cap = o.cap ? o.cap * 2 : 256;
    while (o.n * 2 > m->cap) m->cap *= 2;
    m->k = calloc(m->cap, sizeof(uint32_t));
    m->v = calloc(m->cap, sizeof(void *));
    m->n = m->used = 0;
    for (size_t i = 0; i < o.cap; i++)
        if (o.v[i] && o.v[i] != TOMB) map_put(m, o.k[i], o.v[i]);
    free(o.k);
    free(o.v);
}

static void map_put(u32map *m, uint32_t k, void *v) {
    if ((m->used + 1) * 10 >= m->cap * 7) map_grow(m);
    size_t tomb = (size_t) -1;
    for (size_t i = h32(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->v[i]) {
            if (tomb != (size_t) -1) i = tomb;
            else m->used++;
            m->k[i] = k;
            m->v[i] = v;
            m->n++;
            return;
        }
        if (m->v[i] == TOMB) {
            if (tomb == (size_t) -1) tomb = i;
        } else if (m->k[i] == k) {
            m->v[i] = v;
            return;
        }
    }
}

static void *map_del(u32map *m, uint32_t k) {
    if (!m->cap) return NULL;
    for (size_t i = h32(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->v[i]) return NULL;
        if (m->v[i] != TOMB && m->k[i] == k) {
            void *v = m->v[i];
            m->v[i] = TOMB;
            m->n--;
            return v;
        }
    }
}

#define MAP_EACH(m, i) for (size_t i = 0; i < (m).cap; i++) if ((m).v[i] && (m).v[i] != TOMB)

/* ---------------- Data state ---------------- */

typedef struct {
    uint32_t id, chid;
    int64_t start, stop;
    char *title, *subtitle, *plot;
    /* for the info window: short text (only if different from plot), image
     * ("imagecache/<n>" or URL), genre (DVB contentType), season/episode */
    char *summary, *image, *onscreen;
    uint16_t genre, year;
    uint8_t age;
    uint16_t season, episode;
} tvev;

typedef struct {
    uint32_t id;
    int num;
    char *name, *icon;
    uint32_t now_id, next_id;
    int dvb;
    int iptv1; /* first service is IPTV (no DVB mux) */
    tvev **ev; /* sorted by start */
    int nev, cev;
} tvch;

typedef struct {
    uint32_t id;
    char *name;
    int index;
    uint32_t *mem;
    int nmem;
    char *proxy; /* HbbTV proxy tag (egress.c): egress, not a channel group */
    int internal; /* internal tvh tag (fork extension): never a channel group */
} tvtag;

static int tag_is_group(const tvtag *t) { return !t->proxy && !t->internal; }

typedef struct {
    uint32_t id, event;
    char state[16];
    char autorec[48];
} tvdvr;

static tvsrc_cfg cfg;
static tvsrc_actions act;
static u32map chans, events, tags, dvrs;
static tvch **sorted;
static int nsorted, sorted_dirty = 1;
static int synced, played, cur, last;
static int64_t horizon;  /* events known up to here */
static int64_t grid_max; /* end time of the pending fetch-more request */
static int seq_grid = -1, seq_sched = -1, sched_ch;
static time_t pruned_at;

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : NULL;
}

static int jhas(const cJSON *o, const char *k) { return cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(o, k)); }

static double jnum(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : 0;
}

static void setstr(char **dst, const char *s) {
    if (!s) return;
    free(*dst);
    *dst = strdup(s);
}

/* like setstr, but NULL clears */
static void setstr_c(char **dst, const char *s) {
    free(*dst);
    *dst = s && *s ? strdup(s) : NULL;
}

/* a channel's events: insert sorted / remove */
static void ch_add_ev(tvch *c, tvev *e) {
    if (!c) return;
    if (c->nev == c->cev) {
        int n = c->cev ? c->cev * 2 : 16;
        tvev **p = realloc(c->ev, (size_t) n * sizeof(*p));
        if (!p) return;
        c->ev = p;
        c->cev = n;
    }
    int lo = 0, hi = c->nev;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (c->ev[mid]->start < e->start) lo = mid + 1;
        else hi = mid;
    }
    memmove(c->ev + lo + 1, c->ev + lo, (size_t) (c->nev - lo) * sizeof(*c->ev));
    c->ev[lo] = e;
    c->nev++;
}

static void ch_del_ev(tvch *c, tvev *e) {
    if (!c) return;
    for (int i = 0; i < c->nev; i++)
        if (c->ev[i] == e) {
            memmove(c->ev + i, c->ev + i + 1, (size_t) (c->nev - i - 1) * sizeof(*c->ev));
            c->nev--;
            return;
        }
}

static void ev_free(tvev *e) {
    free(e->title);
    free(e->subtitle);
    free(e->plot);
    free(e->summary);
    free(e->image);
    free(e->onscreen);
    free(e);
}

static void ev_apply(const cJSON *j) {
    uint32_t id = (uint32_t) jnum(j, "eventId");
    if (!id) return;
    tvev *e = map_get(&events, id);
    int isnew = !e;
    if (isnew) {
        e = calloc(1, sizeof(*e));
        if (!e) return;
        e->id = id;
    }
    uint32_t chid = jhas(j, "channelId") ? (uint32_t) jnum(j, "channelId") : e->chid;
    int64_t start = jhas(j, "start") ? (int64_t) jnum(j, "start") : e->start;
    int relink = !isnew && (chid != e->chid || start != e->start);
    if (relink) ch_del_ev(map_get(&chans, e->chid), e);
    e->chid = chid;
    e->start = start;
    if (jhas(j, "stop")) e->stop = (int64_t) jnum(j, "stop");
    setstr(&e->title, jstr(j, "title"));
    setstr(&e->subtitle, jstr(j, "subtitle"));
    /* description, otherwise summary; the
     * summary additionally if it says something different */
    const char *plot = jstr(j, "description"), *sum = jstr(j, "summary");
    setstr(&e->plot, plot ? plot : sum);
    setstr_c(&e->summary, plot && sum && strcmp(plot, sum) ? sum : NULL);
    setstr_c(&e->image, jstr(j, "image"));
    setstr_c(&e->onscreen, jstr(j, "episodeOnscreen"));
    e->genre = jhas(j, "contentType") ? (uint16_t) jnum(j, "contentType") : 0;
    e->year = jhas(j, "copyrightYear") ? (uint16_t) jnum(j, "copyrightYear") : 0;
    e->age = jhas(j, "ageRating") ? (uint8_t) jnum(j, "ageRating") : 0;
    e->season = jhas(j, "seasonNumber") ? (uint16_t) jnum(j, "seasonNumber") : 0;
    e->episode = jhas(j, "episodeNumber") ? (uint16_t) jnum(j, "episodeNumber") : 0;
    if (isnew) map_put(&events, id, e);
    if (isnew || relink) ch_add_ev(map_get(&chans, chid), e);
}

static void ev_delete(uint32_t id) {
    tvev *e = map_del(&events, id);
    if (!e) return;
    ch_del_ev(map_get(&chans, e->chid), e);
    ev_free(e);
}

/* "network/mux/service": DVB if one part is a frequency (578MHz, 11302.75H,
 * 12341V) - IPTV muxes are named after their source */
static int svc_is_dvb(const char *name) {
    const char *p = name;
    while (p && *p) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t) (e - p) : strlen(p);
        size_t i = 0, digits = 0;
        while (i < n && ((p[i] >= '0' && p[i] <= '9') || p[i] == '.')) digits += p[i++] != '.';
        if (digits && ((n - i == 3 && (!strncmp(p + i, "MHz", 3) || !strncmp(p + i, "kHz", 3))) ||
                       (n - i == 1 && strchr("HVLR", p[i]))))
            return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

static void ch_apply(const cJSON *j) {
    uint32_t id = (uint32_t) jnum(j, "channelId");
    if (!id) return;
    tvch *c = map_get(&chans, id);
    if (!c) {
        c = calloc(1, sizeof(*c));
        if (!c) return;
        c->id = id;
        map_put(&chans, id, c);
        /* link in events that arrived before the channel (resync) */
        MAP_EACH(events, i) {
            tvev *e = events.v[i];
            if (e->chid == id) ch_add_ev(c, e);
        }
    }
    if (jhas(j, "channelNumber")) {
        c->num = (int) jnum(j, "channelNumber");
        sorted_dirty = 1;
    }
    setstr(&c->name, jstr(j, "channelName"));
    setstr(&c->icon, jstr(j, "channelIcon"));
    if (jhas(j, "eventId")) c->now_id = (uint32_t) jnum(j, "eventId");
    if (jhas(j, "nextEventId")) c->next_id = (uint32_t) jnum(j, "nextEventId");
    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(j, "services");
    if (cJSON_IsArray(sv)) {
        c->dvb = 0;
        const cJSON *s;
        cJSON_ArrayForEach(s, sv) {
            const char *n = jstr(s, "name");
            if (n && svc_is_dvb(n)) c->dvb = 1;
        }
        const char *n0 = jstr(cJSON_GetArrayItem(sv, 0), "name");
        c->iptv1 = n0 && !svc_is_dvb(n0);
    }
}

static void ch_delete(uint32_t id) {
    tvch *c = map_del(&chans, id);
    if (!c) return;
    for (int i = 0; i < c->nev; i++) {
        map_del(&events, c->ev[i]->id);
        ev_free(c->ev[i]);
    }
    free(c->ev);
    free(c->name);
    free(c->icon);
    free(c);
    sorted_dirty = 1;
}

static void tag_apply(const cJSON *j) {
    uint32_t id = (uint32_t) jnum(j, "tagId");
    if (!id) return;
    tvtag *t = map_get(&tags, id);
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t) return;
        t->id = id;
        map_put(&tags, id, t);
    }
    setstr(&t->name, jstr(j, "tagName"));
    /* tagAdd/tagUpdate carry the whole tag: without tagInternal it is not (or no longer) internal */
    if (cJSON_GetObjectItemCaseSensitive(j, "tagName")) t->internal = jnum(j, "tagInternal") != 0;
    if (cJSON_GetObjectItemCaseSensitive(j, "tagName") || cJSON_GetObjectItemCaseSensitive(j, "tagIcon")) {
        free(t->proxy);
        t->proxy = egress_tag_proxy(t->name, jstr(j, "tagIcon"));
    }
    if (jhas(j, "tagIndex")) t->index = (int) jnum(j, "tagIndex");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "members");
    if (cJSON_IsArray(m)) {
        free(t->mem);
        t->nmem = 0;
        t->mem = malloc(sizeof(uint32_t) * (size_t) (cJSON_GetArraySize(m) + 1));
        const cJSON *x;
        cJSON_ArrayForEach(x, m) if (t->mem && cJSON_IsNumber(x)) t->mem[t->nmem++] = (uint32_t) x->valuedouble;
    }
}

static void tag_free(tvtag *t) {
    free(t->name);
    free(t->proxy);
    free(t->mem);
    free(t);
}

/* channels of the HbbTV proxy tags to egress.c (channel name -> egress) */
static void proxy_tags_push(void) {
    size_t cap = 0;
    MAP_EACH(tags, i) if (((tvtag *) tags.v[i])->proxy) cap += (size_t) ((tvtag *) tags.v[i])->nmem;
    const char **cn = cap ? malloc(cap * sizeof(*cn)) : NULL, **pv = cap ? malloc(cap * sizeof(*pv)) : NULL;
    int n = 0;
    MAP_EACH(tags, i) {
        const tvtag *t = tags.v[i];
        if (!t->proxy) continue;
        int k = 0;
        for (int m = 0; cn && pv && m < t->nmem; m++) {
            const tvch *c = map_get(&chans, t->mem[m]);
            if (!c || !c->name) continue;
            cn[n] = c->name;
            pv[n++] = t->proxy;
            k++;
        }
        mt_log("[tvsrc] proxy tag '%s': %s, %d channel(s)", t->name ? t->name : "", t->proxy[0] ? t->proxy : "direct", k);
    }
    egress_set_tag_rules(cn, pv, n);
    free(cn);
    free(pv);
}

static void dvr_apply(const cJSON *j) {
    uint32_t id = (uint32_t) jnum(j, "id");
    if (!id) return;
    tvdvr *d = map_get(&dvrs, id);
    if (!d) {
        d = calloc(1, sizeof(*d));
        if (!d) return;
        d->id = id;
        map_put(&dvrs, id, d);
    }
    if (jhas(j, "eventId")) d->event = (uint32_t) jnum(j, "eventId");
    const char *s = jstr(j, "state");
    if (s) snprintf(d->state, sizeof(d->state), "%s", s);
    s = jstr(j, "autorecId");
    if (s) snprintf(d->autorec, sizeof(d->autorec), "%s", s);
}

static void clear_all(void) {
    MAP_EACH(chans, i) ch_delete(chans.k[i]);
    MAP_EACH(events, i) ev_free(events.v[i]); /* without a channel */
    MAP_EACH(tags, i) tag_free(tags.v[i]);
    MAP_EACH(dvrs, i) free(dvrs.v[i]);
    u32map *maps[] = {&chans, &events, &tags, &dvrs};
    for (size_t i = 0; i < 4; i++) {
        free(maps[i]->k);
        free(maps[i]->v);
        memset(maps[i], 0, sizeof(u32map));
    }
    sorted_dirty = 1;
    horizon = 0;
}

static int cmp_num(const void *a, const void *b) {
    const tvch *x = *(tvch *const *) a, *y = *(tvch *const *) b;
    return x->num != y->num ? (x->num < y->num ? -1 : 1) : (x->id < y->id ? -1 : x->id > y->id);
}

static void sort_channels(void) {
    if (!sorted_dirty) return;
    free(sorted);
    sorted = malloc(sizeof(tvch *) * (chans.n + 1));
    nsorted = 0;
    MAP_EACH(chans, i) if (sorted) sorted[nsorted++] = chans.v[i];
    if (sorted) qsort(sorted, (size_t) nsorted, sizeof(*sorted), cmp_num);
    sorted_dirty = 0;
}

static tvch *ch_by_num(int num) {
    sort_channels();
    int lo = 0, hi = nsorted - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sorted[mid]->num == num) return sorted[mid];
        if (sorted[mid]->num < num) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* ---------------- Messages to the app ---------------- */

/* logo directly from tvh's image cache (channelIcon "imagecache/<n>"); the UI
 * trims the border when loading (ui.c) */
static void icon_url(const tvch *c, char *out, size_t n) {
    out[0] = 0;
    if (!c->icon || !c->icon[0]) return;
    if (!strncmp(c->icon, "http://", 7)) snprintf(out, n, "%s", c->icon);
    else if (!strstr(c->icon, "://")) snprintf(out, n, "http://%s:%d/%s", cfg.tvh_host, cfg.http_port, c->icon);
}

static const tvdvr *dvr_for(uint32_t event) {
    if (!event) return NULL;
    MAP_EACH(dvrs, i) {
        const tvdvr *d = dvrs.v[i];
        if (d->event == event && (!strcmp(d->state, "scheduled") || !strcmp(d->state, "recording"))) return d;
    }
    return NULL;
}

static cJSON *broadcast(const tvev *e) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "id", e->id);
    cJSON_AddStringToObject(b, "title", e->title ? e->title : "");
    cJSON_AddStringToObject(b, "subtitle", e->subtitle ? e->subtitle : "");
    cJSON_AddStringToObject(b, "plot", e->plot ? e->plot : "");
    cJSON_AddNumberToObject(b, "start", (double) e->start);
    cJSON_AddNumberToObject(b, "stop", (double) e->stop);
    const tvdvr *d = dvr_for(e->id);
    if (d) {
        cJSON_AddStringToObject(b, "rec", d->state);
        cJSON_AddBoolToObject(b, "series", d->autorec[0] != 0);
        cJSON_AddNumberToObject(b, "timerId", d->id);
        if (d->autorec[0]) cJSON_AddStringToObject(b, "autorecId", d->autorec);
        else cJSON_AddNullToObject(b, "autorecId");
    } else {
        cJSON_AddNullToObject(b, "rec");
        cJSON_AddBoolToObject(b, "series", 0);
        cJSON_AddNullToObject(b, "timerId");
        cJSON_AddNullToObject(b, "autorecId");
    }
    return b;
}

/* current and next event */
static void now_next(const tvch *c, const tvev **now, const tvev **next) {
    time_t t = time(NULL);
    *now = *next = NULL;
    const tvev *n = c->now_id ? map_get(&events, c->now_id) : NULL;
    if (n && n->start <= t && n->stop > t) *now = n;
    for (int i = 0; i < c->nev && !*now; i++)
        if (c->ev[i]->start <= t && c->ev[i]->stop > t) *now = c->ev[i];
    for (int i = 0; i < c->nev; i++)
        if (c->ev[i]->start >= (*now ? (*now)->stop : t)) {
            *next = c->ev[i];
            break;
        }
}

static void fill_event(const tvev *e, tvsrc_event *out) {
    memset(out, 0, sizeof(*out));
    out->id = e->id;
    out->start = e->start;
    out->stop = e->stop;
    out->title = e->title ? e->title : "";
    out->subtitle = e->subtitle ? e->subtitle : "";
    out->text = e->plot ? e->plot : "";
    out->summary = e->summary ? e->summary : "";
    out->onscreen = e->onscreen ? e->onscreen : "";
    out->genre = e->genre;
    out->year = e->year;
    out->age = e->age;
    out->season = e->season;
    out->episode = e->episode;
    /* image like the logos: "imagecache/<n>" from tvh's web server, otherwise the URL */
    if (e->image) {
        if (!strncmp(e->image, "http://", 7) || !strncmp(e->image, "https://", 8))
            snprintf(out->image, sizeof(out->image), "%s", e->image);
        else if (!strstr(e->image, "://"))
            snprintf(out->image, sizeof(out->image), "http://%s:%d/%s", cfg.tvh_host, cfg.http_port,
                     e->image[0] == '/' ? e->image + 1 : e->image);
    }
}

int tvsrc_event_info(int num, int next, tvsrc_event *out) {
    memset(out, 0, sizeof(*out));
    const tvch *c = ch_by_num(num);
    if (!c) return 0;
    const tvev *n, *x;
    now_next(c, &n, &x);
    const tvev *e = next ? x : n;
    if (!e) return 0;
    fill_event(e, out);
    return 1;
}

/* ---------------- For Kodi/Kore (kodi.c) ---------------- */

static int cmp_tag(const void *a, const void *b); /* further below (grid) */

int tvsrc_groups(uint32_t *ids, const char **names, int max) {
    const tvtag **v = tags.n ? malloc(sizeof(tvtag *) * tags.n) : NULL;
    int n = 0;
    MAP_EACH(tags, i) if (v && tag_is_group(tags.v[i])) v[n++] = tags.v[i];
    if (n > 1) qsort(v, (size_t) n, sizeof(*v), cmp_tag);
    int k = 0;
    for (int i = 0; i < n && k < max; i++, k++) {
        ids[k] = v[i]->id;
        names[k] = v[i]->name ? v[i]->name : "";
    }
    free(v);
    return k;
}

int tvsrc_group_name(uint32_t tag, char *out, size_t n) {
    const tvtag *t = map_get(&tags, tag);
    if (!t) return 0;
    snprintf(out, n, "%s", t->name ? t->name : "");
    return 1;
}

int tvsrc_channels(uint32_t tag, int *nums, int max) {
    sort_channels();
    const tvtag *t = tag ? map_get(&tags, tag) : NULL;
    if (tag && !t) return 0;
    int k = 0;
    for (int i = 0; i < nsorted; i++) {
        const tvch *c = sorted[i];
        if (t) {
            int in = 0;
            for (int m = 0; m < t->nmem && !in; m++) in = t->mem[m] == c->id;
            if (!in) continue;
        }
        if (k < max) nums[k] = c->num;
        k++;
    }
    return k;
}

int tvsrc_channel_meta(int num, char *name, size_t nn, char *icon, size_t ni) {
    const tvch *c = ch_by_num(num);
    if (!c) return 0;
    if (name) snprintf(name, nn, "%s", c->name ? c->name : "");
    if (icon) icon_url(c, icon, ni);
    return 1;
}

int tvsrc_events(int num, tvsrc_event *out, int max) {
    const tvch *c = ch_by_num(num);
    if (!c) return 0;
    int k = 0;
    for (int i = 0; i < c->nev && k < max; i++) fill_event(c->ev[i], &out[k++]);
    return k;
}

int tvsrc_event_by_id(uint32_t id, tvsrc_event *out, int *num) {
    const tvev *e = map_get(&events, id);
    if (!e) return 0;
    const tvch *c = map_get(&chans, e->chid);
    if (num) *num = c ? c->num : 0;
    fill_event(e, out);
    return 1;
}

static void epg_of(const tvev *e, int with_plot, epg_entry *o) {
    memset(o, 0, sizeof(*o));
    if (!e) return;
    snprintf(o->title, sizeof(o->title), "%s", e->title ? e->title : "");
    snprintf(o->subtitle, sizeof(o->subtitle), "%s", e->subtitle ? e->subtitle : "");
    if (with_plot) snprintf(o->plot, sizeof(o->plot), "%s", e->plot ? e->plot : "");
    o->start = (long) e->start;
    o->stop = (long) e->stop;
}

static void now_next_of(const tvch *c, epg_entry *now, epg_entry *next) {
    const tvev *n, *x;
    now_next(c, &n, &x);
    epg_of(n, 1, now);
    epg_of(x, 0, next);
}

static void save_state(void) {
    prefs_set("tv.channel", cJSON_CreateNumber(cur));
    prefs_set("tv.last", cJSON_CreateNumber(last));
}

uint32_t tvsrc_status_channel(int num) {
    tvch *c = ch_by_num(num);
    return c && c->dvb && !c->iptv1 ? c->id : 0;
}

static void play(int num, int initial) {
    tvch *c = ch_by_num(num);
    if (!c) {
        char t[64];
        snprintf(t, sizeof(t), _("Channel %d does not exist"), num);
        osd_message(t, 3000, 0);
        return;
    }
    if (num != cur) {
        if (cur) last = cur;
        cur = num;
        save_state();
    }
    tvsrc_play_info p;
    char url[1200], icon[1200];
    snprintf(url, sizeof(url), "htsp://%s:%d/%u", cfg.tvh_host, cfg.htsp_port, c->id);
    icon_url(c, icon, sizeof(icon));
    p.channel = c->num;
    p.name = c->name ? c->name : "";
    p.url = url;
    p.icon = icon;
    p.initial = initial;
    now_next_of(c, &p.now, &p.next);
    mt_log("[tvsrc] play %d %s%s", c->num, c->name ? c->name : "", initial ? " (startup)" : "");
    if (act.play) act.play(&p);
}

static void step(int dir) {
    sort_channels();
    if (!nsorted) return;
    int idx = -1;
    for (int i = 0; i < nsorted; i++)
        if (sorted[i]->num == cur) idx = i;
    int next = idx < 0 ? 0 : (idx + dir + nsorted) % nsorted;
    play(sorted[next]->num, 0);
}

static void send_now_next(void) {
    tvch *c = ch_by_num(cur);
    if (!c || !act.now_next) return;
    epg_entry now, next;
    now_next_of(c, &now, &next);
    act.now_next(&now, &next);
}

/* groups or recording markers changed: reload the open EPG */
static void send_groups(void) {
    if (act.groups_changed) act.groups_changed();
}

static cJSON *grid_row(const tvch *c, int64_t ws, int64_t we) {
    char icon[1200];
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", c->num);
    cJSON_AddStringToObject(r, "name", c->name ? c->name : "");
    icon_url(c, icon, sizeof(icon));
    cJSON_AddStringToObject(r, "icon", icon);
    cJSON *b = cJSON_AddArrayToObject(r, "broadcasts");
    for (int i = 0; i < c->nev; i++) {
        const tvev *e = c->ev[i];
        if (e->start >= we) break;
        if (e->stop > ws) cJSON_AddItemToArray(b, broadcast(e));
    }
    return r;
}

static int cmp_tag(const void *a, const void *b) {
    const tvtag *x = *(tvtag *const *) a, *y = *(tvtag *const *) b;
    return x->index != y->index ? (x->index < y->index ? -1 : 1) : (x->id < y->id ? -1 : x->id > y->id);
}

/* groupid "groups": groups by tagIndex, within them by
 * number; channels without a group at the end under "Other channels" */
static void send_grid(int64_t ws, int64_t dur) {
    int64_t we = ws + dur;
    sort_channels();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "groupid", "groups");
    cJSON *groups = cJSON_AddArrayToObject(j, "groups");
    cJSON_AddNumberToObject(j, "start", (double) ws);
    cJSON_AddNumberToObject(j, "duration", (double) dur);
    cJSON_AddNumberToObject(j, "currentChannel", cur);
    cJSON *rows = cJSON_AddArrayToObject(j, "channels");
    tvtag **tl = malloc(sizeof(tvtag *) * (tags.n + 1));
    size_t nt = 0;
    MAP_EACH(tags, i) if (tl && tag_is_group(tags.v[i])) tl[nt++] = tags.v[i];
    if (tl) qsort(tl, nt, sizeof(*tl), cmp_tag);
    u32map grouped = {0};
    int nrows = 0;
    for (size_t t = 0; tl && t < nt; t++) {
        int start = nrows;
        for (int i = 0; i < nsorted; i++) { /* members in number order */
            const tvch *c = sorted[i];
            int member = 0;
            for (int m = 0; m < tl[t]->nmem && !member; m++) member = tl[t]->mem[m] == c->id;
            if (!member) continue;
            cJSON_AddItemToArray(rows, grid_row(c, ws, we));
            map_put(&grouped, c->id, (void *) c);
            nrows++;
        }
        if (nrows == start) continue;
        char id[16];
        snprintf(id, sizeof(id), "%u", tl[t]->id);
        cJSON *g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "id", id);
        cJSON_AddStringToObject(g, "label", tl[t]->name ? tl[t]->name : "");
        cJSON_AddNumberToObject(g, "start", start);
        cJSON_AddItemToArray(groups, g);
    }
    int start = nrows;
    for (int i = 0; i < nsorted; i++) {
        if (map_get(&grouped, sorted[i]->id)) continue;
        cJSON_AddItemToArray(rows, grid_row(sorted[i], ws, we));
        nrows++;
    }
    if (nrows > start) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "id", "other");
        cJSON_AddStringToObject(g, "label", "Other channels");
        cJSON_AddNumberToObject(g, "start", start);
        cJSON_AddItemToArray(groups, g);
    }
    free(grouped.k);
    free(grouped.v);
    free(tl);
    if (act.grid) act.grid(j);
    else cJSON_Delete(j);
}

static void send_schedule(int num) {
    tvch *c = ch_by_num(num);
    if (!c) return;
    char icon[1200];
    time_t t = time(NULL);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "channel", c->num);
    cJSON_AddStringToObject(j, "name", c->name ? c->name : "");
    icon_url(c, icon, sizeof(icon));
    cJSON_AddStringToObject(j, "icon", icon);
    cJSON *b = cJSON_AddArrayToObject(j, "broadcasts");
    int n = 0;
    for (int i = 0; i < c->nev && n < 12; i++) {
        const tvev *e = c->ev[i];
        if (e->stop <= t) continue;
        if (e->start > t + 24 * 3600) break;
        cJSON_AddItemToArray(b, broadcast(e));
        n++;
    }
    if (act.schedule) act.schedule(j);
    else cJSON_Delete(j);
}

/* fetch more events (all channels or one) */
static int get_events(uint32_t chid, int64_t max_time) {
    cJSON *a = cJSON_CreateObject();
    if (chid) cJSON_AddNumberToObject(a, "channelId", chid);
    cJSON_AddNumberToObject(a, "maxTime", (double) max_time);
    int seq = htsp_request("getEvents", a);
    cJSON_Delete(a);
    return seq;
}

/* ---------------- For tvrec (recordings, timers) ---------------- */

static int rec_channel(uint32_t chid, int *num, char *name, size_t nn, char *icon, size_t ni) {
    const tvch *c = map_get(&chans, chid);
    if (!c) return 0;
    *num = c->num;
    snprintf(name, nn, "%s", c->name ? c->name : "");
    icon_url(c, icon, ni);
    return 1;
}

static int rec_event(uint32_t id, char *title, size_t n, uint32_t *chid) {
    const tvev *e = map_get(&events, id);
    if (!e) return 0;
    snprintf(title, n, "%s", e->title ? e->title : "");
    *chid = e->chid;
    return 1;
}

/* ---------------- Interface ---------------- */

void tvsrc_start(const tvsrc_cfg *c, const tvsrc_actions *a, const tvrec_actions *ra, int channel, int last_ch) {
    cfg = *c;
    if (!cfg.htsp_port) cfg.htsp_port = 9982;
    if (!cfg.http_port) cfg.http_port = 9981;
    act = *a;
    cur = channel;
    last = last_ch;
    tvrec_cfg rc = {.act = *ra, .channel = rec_channel, .event = rec_event, .schedule = send_schedule,
                    .http_port = cfg.http_port, .read_only = cfg.dvr_read_only};
    snprintf(rc.host, sizeof(rc.host), "%s", cfg.tvh_host);
    snprintf(rc.user, sizeof(rc.user), "%s", cfg.user);
    snprintf(rc.pass, sizeof(rc.pass), "%s", cfg.pass);
    tvrec_init(&rc);
    htsp_cfg h = {.port = cfg.htsp_port, .epg_window = EPG_WINDOW};
    snprintf(h.host, sizeof(h.host), "%s", cfg.tvh_host);
    snprintf(h.user, sizeof(h.user), "%s", cfg.user);
    snprintf(h.pass, sizeof(h.pass), "%s", cfg.pass);
    mt_log("[tvsrc] HTSP %s:%d as %s, channel %d (previous %d)", h.host, h.port, h.user[0] ? h.user : "anonymous",
           cur, last);
    htsp_start(&h);
}

int tvsrc_channel(void) { return cur; }
int tvsrc_last(void) { return last; }

void tvsrc_tune(int num) { play(num, 0); }

void tvsrc_step(int dir) { step(dir); }

void tvsrc_tune_last(void) {
    if (last) play(last, 0);
}

void tvsrc_tune_current(void) {
    if (cur) play(cur, 0);
}

void tvsrc_refresh_now_next(void) { send_now_next(); }

void tvsrc_grid(int64_t ws, int64_t dur) {
    if (dur <= 0) dur = 10800;
    if (!ws) ws = time(NULL);
    send_grid(ws, dur);
    /* further ahead than known: fetch more, the reply triggers a reload */
    if (synced && ws + dur > horizon && seq_grid < 0) {
        grid_max = ws + dur + 3600;
        seq_grid = get_events(0, grid_max);
    }
}

void tvsrc_schedule(int num) {
    send_schedule(num);
    tvch *c = ch_by_num(num);
    if (c && synced && time(NULL) + 24 * 3600 > horizon) {
        seq_sched = get_events(c->id, time(NULL) + 24 * 3600);
        sched_ch = num;
    }
}

static void prune(void) {
    time_t t = time(NULL);
    if (t - pruned_at < PRUNE_S) return;
    pruned_at = t;
    int n = 0;
    MAP_EACH(events, i) {
        tvev *e = events.v[i];
        if (e->stop < t - KEEP_PAST) {
            ev_delete(e->id);
            n++;
        }
    }
    if (n) mt_log("[tvsrc] %d old events purged, %zu remain", n, events.n);
}

static void merge_events(const cJSON *r) {
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(r, "events")) ev_apply(e);
}

void tvsrc_poll(void) {
    int changed_dvr = 0;
    for (int budget = 4000; budget > 0; budget--) {
        cJSON *j = htsp_next();
        if (!j) break;
        const char *m = jstr(j, "method");
        if (!m) m = "";
        if (!strcmp(m, "_sync")) {
            clear_all();
            tvrec_clear();
            synced = 0;
        } else if (!strcmp(m, "_down")) {
            if (act.connected) act.connected(0);
        } else if (!strcmp(m, "_noaccess")) {
            if (act.noaccess) act.noaccess();
        } else if (!strcmp(m, "channelAdd") || !strcmp(m, "channelUpdate")) {
            int id = (int) jnum(j, "channelId");
            tvch *c = map_get(&chans, (uint32_t) id);
            int now_changed = c && c->num == cur && jhas(j, "eventId") && (uint32_t) jnum(j, "eventId") != c->now_id;
            ch_apply(j);
            if (synced && now_changed) send_now_next();
        } else if (!strcmp(m, "channelDelete")) ch_delete((uint32_t) jnum(j, "channelId"));
        else if (!strcmp(m, "tagAdd") || !strcmp(m, "tagUpdate")) {
            tag_apply(j);
            const tvtag *t = map_get(&tags, (uint32_t) jnum(j, "tagId"));
            if (synced && t && t->proxy) proxy_tags_push();
        } else if (!strcmp(m, "tagDelete")) {
            tvtag *t = map_del(&tags, (uint32_t) jnum(j, "tagId"));
            int px = t && t->proxy;
            if (t) tag_free(t);
            if (synced && px) proxy_tags_push();
        } else if (!strcmp(m, "eventAdd") || !strcmp(m, "eventUpdate")) {
            ev_apply(j);
        } else if (!strcmp(m, "eventDelete")) {
            ev_delete((uint32_t) jnum(j, "eventId"));
        } else if (!strcmp(m, "dvrEntryAdd") || !strcmp(m, "dvrEntryUpdate")) {
            dvr_apply(j);
            tvrec_htsp(j);
            changed_dvr = 1;
        } else if (!strcmp(m, "dvrEntryDelete")) {
            free(map_del(&dvrs, (uint32_t) jnum(j, "id")));
            tvrec_htsp(j);
            changed_dvr = 1;
        } else if (!strncmp(m, "autorecEntry", 12)) {
            tvrec_htsp(j);
        } else if (!strcmp(m, "initialSyncCompleted")) {
            synced = 1;
            horizon = (int64_t) time(NULL) + EPG_WINDOW;
            sort_channels();
            /* events with an image (tvh EPG) and the first channels that have them */
            size_t nimg = 0;
            char with[96] = "";
            MAP_EACH(events, i) {
                const tvev *e = events.v[i];
                if (!e->image) continue;
                nimg++;
                const tvch *c = map_get(&chans, e->chid);
                char t[12];
                snprintf(t, sizeof(t), " %d", c ? c->num : 0);
                if (c && c->num && !strstr(with, t) && strlen(with) + strlen(t) < sizeof(with)) strcat(with, t);
            }
            proxy_tags_push();
            size_t ngroups = 0;
            MAP_EACH(tags, i) ngroups += tag_is_group(tags.v[i]);
            mt_log("[tvsrc] ready: %d channels, %zu groups, %zu events (%zu with image, channels%s), %zu DVR entries",
                   nsorted, ngroups, events.n, nimg, with[0] ? with : " none", dvrs.n);
            if (act.connected) act.connected(1);
            if (!played) {
                played = 1;
                if (!ch_by_num(cur) && nsorted) cur = sorted[0]->num;
                play(cur, 1);
            } else {
                send_now_next();
            }
            send_groups(); /* reload the open EPG */
        } else if (cJSON_GetObjectItemCaseSensitive(j, "seq")) {
            int seq = (int) jnum(j, "seq");
            if (seq == seq_grid) {
                merge_events(j);
                seq_grid = -1;
                if (grid_max > horizon) horizon = grid_max;
                send_groups(); /* the EPG reloads the window */
            } else if (seq == seq_sched) {
                merge_events(j);
                seq_sched = -1;
                send_schedule(sched_ch);
            } else {
                tvrec_reply(seq, j);
            }
        }
        cJSON_Delete(j);
    }
    if (synced && changed_dvr) send_groups(); /* recording markers in the open EPG */
    tvrec_poll(synced);
    prune();
}
