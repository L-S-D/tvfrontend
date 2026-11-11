/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sidedata.h"

#include "dvbsubdec.h"
#include "fetch.h"
#include "log.h"
#include "strbuf.h"
#include "ttxdec.h"
#include "pmtinfo.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DVB 8

typedef struct {
    int index;
    char lang[4];
    int comp, anc;
    int type; /* subtitling_type (tvh with HTSP extension), 0 = unknown */
    dvbsubdec *dec;
} dvbstream;

/* teletext subtitle pages according to tvh (TEXTSUB streams with page and kind,
 * only tvh with HTSP extension) - without them, the PMT (pmtinfo.c) */
#define MAX_TTXSUB 8
typedef struct {
    int page; /* hex */
    int type; /* 2 subtitles, 5 for the hearing impaired */
    char lang[4];
} ttxsub;

/* feeder thread writes, main thread queries */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_channel; /* channel of the pages, 0 = none */
static int g_ttx_index = -1;
static ttxdec *g_ttx;
static dvbstream g_dvb[MAX_DVB];
static int g_ndvb;
static ttxsub g_ttxsub[MAX_TTXSUB];
static int g_nttxsub;
static int g_ttx_initial; /* broadcaster's start page (hex), 0 = unknown */

/* HbbTV AIT from the subscription (tvh delivers the raw sections for each new version):
 * per section number the apps as a JSON fragment, as with dvb-buffer
 * {"org","app","control","type","name","url","carousel"} */
#define AIT_SECTIONS 16
static int g_ait_index = -1;
static int g_ait_version = -1;
static char *g_ait_sec[AIT_SECTIONS];

/* HbbTV stream events (tvh fork: DSMCC streams): the versions of each event
 * seen lately (a broadcaster repeats a section several times a second and may
 * alternate two of them), and the events for the main thread */
#define MAX_DSMCC 4
#define SE_IDS 16
#define SE_HIST 8
#define SE_QUEUE 32
typedef struct {
    int id;              /* event_id, -1 = free */
    int ver[SE_HIST];    /* section versions seen, -1 = none */
    int next;
} se_seen;
typedef struct {
    int index, tag;
    se_seen seen[SE_IDS];
} dsmccstream;
static dsmccstream g_dsmcc[MAX_DSMCC];
static int g_ndsmcc;
static sidedata_stream_event g_seq[SE_QUEUE];
static int g_seq_head, g_seq_n;

static void se_clear(void) {
    g_ndsmcc = 0;
    g_seq_head = g_seq_n = 0;
}

/* 1 = this version of the event is new */
static int se_new(dsmccstream *d, int id, int ver) {
    se_seen *s = NULL;
    for (int i = 0; i < SE_IDS && !s; i++)
        if (d->seen[i].id == id) s = &d->seen[i];
    if (!s) {
        for (int i = 0; i < SE_IDS && !s; i++)
            if (d->seen[i].id < 0) s = &d->seen[i];
        if (!s) s = &d->seen[id % SE_IDS]; /* full: reuse a slot */
        s->id = id;
        for (int i = 0; i < SE_HIST; i++) s->ver[i] = -1;
        s->next = 0;
    }
    for (int i = 0; i < SE_HIST; i++)
        if (s->ver[i] == ver) return 0;
    s->ver[s->next] = ver;
    s->next = (s->next + 1) % SE_HIST;
    return 1;
}

/* one DSM-CC stream descriptor section (table 0x3D, ISO/IEC 13818-6 9.2.7):
 * its stream event descriptors (tag 0x1A) for the main thread */
static void se_section(dsmccstream *d, const uint8_t *s, size_t len) {
    if (len < 12 || s[0] != 0x3d) return;
    size_t sl = 3 + (((s[1] & 0x0f) << 8) | s[2]);
    if (sl > len || sl < 12) return;
    int ver = (s[5] >> 1) & 0x1f;
    size_t i = 8, end = sl - 4; /* without CRC32 / checksum */
    while (i + 2 <= end) {
        int tag = s[i], l = s[i + 1];
        const uint8_t *e = s + i + 2;
        if (i + 2 + (size_t) l > end) break;
        if (tag == 0x1a && l >= 10) {
            int id = (e[0] << 8) | e[1];
            if (se_new(d, id, ver)) {
                if (g_seq_n == SE_QUEUE) { /* main thread too slow: drop the oldest */
                    g_seq_head = (g_seq_head + 1) % SE_QUEUE;
                    g_seq_n--;
                }
                sidedata_stream_event *ev = &g_seq[(g_seq_head + g_seq_n++) % SE_QUEUE];
                ev->component_tag = d->tag;
                ev->event_id = id;
                ev->npt = ((unsigned long long) (e[5] & 1) << 32) | ((unsigned long long) e[6] << 24) |
                          ((unsigned long long) e[7] << 16) | ((unsigned long long) e[8] << 8) | e[9];
                ev->len = l - 10 < (int) sizeof(ev->data) ? l - 10 : (int) sizeof(ev->data);
                memcpy(ev->data, e + 10, (size_t) ev->len);
            }
        }
        i += 2 + (size_t) l;
    }
}

int sidedata_stream_event_next(sidedata_stream_event *ev) {
    int r = 0;
    pthread_mutex_lock(&g_lock);
    if (g_seq_n > 0) {
        *ev = g_seq[g_seq_head];
        g_seq_head = (g_seq_head + 1) % SE_QUEUE;
        g_seq_n--;
        r = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void ait_clear(void) {
    for (int i = 0; i < AIT_SECTIONS; i++) {
        free(g_ait_sec[i]);
        g_ait_sec[i] = NULL;
    }
    g_ait_version = -1;
}

static void json_str(strbuf *j, const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c == '"' || c == '\\') sb_addf(j, "\\%c", c);
        else if (c >= 0x20 && c < 0x7F) sb_addf(j, "%c", c);
        else if (c >= 0x80) sb_addf(j, "\\u%04x", c); /* names mostly ISO 8859-1 */
    }
}

/* transport_protocol_descriptor (0x02): HTTP base or object carousel */
static void ait_transport(const uint8_t *d, size_t l, const uint8_t **base, size_t *bl, int *http, int *car) {
    if (l < 3) return;
    unsigned proto = (unsigned) (d[0] << 8 | d[1]);
    if (proto == 3 && l >= 4 && 4u + d[3] <= l) {
        *http = 1;
        *base = d + 4;
        *bl = d[3];
    } else if (proto == 1) {
        *car = 1;
    }
}

/* one AIT section (ETSI TS 102 809), as in dvb-buffer src/hbbtv/ait.cpp */
static void ait_section(const uint8_t *s, size_t len) {
    if (len < 12 || s[0] != 0x74 || !(s[5] & 1)) return;
    /* tvh sends the section without CRC (already checked) */
    size_t total = (size_t) ((s[1] & 0x0F) << 8 | s[2]) + 3, end;
    if (len == total - 4) end = len;
    else if (len == total) end = len - 4;
    else return;
    int version = (s[5] >> 1) & 0x1F, secno = s[6];
    if (secno >= AIT_SECTIONS) return;
    unsigned app_type = (unsigned) ((s[3] & 0x7F) << 8 | s[4]);
    size_t p = 8;
    size_t cl = (size_t) ((s[p] & 0x0F) << 8 | s[p + 1]);
    p += 2;
    const uint8_t *cbase = NULL;
    size_t cbl = 0;
    int chttp = 0, ccar = 0;
    for (size_t q = p; q + 2 <= p + cl && q + 2 <= end;) {
        size_t l = s[q + 1];
        if (q + 2 + l > end) break;
        if (s[q] == 0x02) ait_transport(s + q + 2, l, &cbase, &cbl, &chttp, &ccar);
        q += 2 + l;
    }
    p += cl;
    if (p + 2 > end) return;
    size_t al = (size_t) ((s[p] & 0x0F) << 8 | s[p + 1]);
    p += 2;
    size_t aend = p + al < end ? p + al : end;
    strbuf j = {0};
    int n = 0;
    while (p + 9 <= aend) {
        uint32_t org = (uint32_t) s[p] << 24 | (uint32_t) s[p + 1] << 16 | (uint32_t) s[p + 2] << 8 | s[p + 3];
        unsigned app = (unsigned) (s[p + 4] << 8 | s[p + 5]), control = s[p + 6];
        size_t dl = (size_t) ((s[p + 7] & 0x0F) << 8 | s[p + 8]);
        p += 9;
        const uint8_t *base = cbase, *name = NULL, *path = NULL;
        size_t bl = cbl, nl = 0, pl = 0;
        int http = chttp, car = ccar;
        for (size_t q = p; q + 2 <= p + dl && q + 2 <= aend;) {
            size_t l = s[q + 1];
            if (q + 2 + l > aend) break;
            const uint8_t *d = s + q + 2;
            if (s[q] == 0x01 && l >= 4 && !name && 4u + d[3] <= l) { /* application_name: language, length, name */
                name = d + 4;
                nl = d[3];
            } else if (s[q] == 0x02) {
                ait_transport(d, l, &base, &bl, &http, &car);
            } else if (s[q] == 0x15) { /* simple_application_location */
                path = d;
                pl = l;
            }
            q += 2 + l;
        }
        p += dl;
        sb_addf(&j, "%s{\"org\":%u,\"app\":%u,\"control\":%u,\"type\":%u,\"name\":\"", n++ ? "," : "", org, app,
                control, app_type);
        json_str(&j, name, nl);
        sb_add(&j, "\",\"url\":\"");
        if (http) {
            json_str(&j, base, bl);
            json_str(&j, path, pl);
        }
        sb_addf(&j, "\",\"carousel\":%s}", car && !http ? "true" : "false");
    }
    char *frag = sb_take(&j, NULL);
    if (version != g_ait_version) {
        ait_clear();
        g_ait_version = version;
        mt_log("[side] AIT version %d from the subscription", version);
    }
    free(g_ait_sec[secno]);
    g_ait_sec[secno] = frag ? frag : strdup("");
}

static void drop_dvb(void) {
    for (int i = 0; i < g_ndvb; i++) dvbsubdec_free(g_dvb[i].dec);
    g_ndvb = 0;
}

static void on_start(void *user, unsigned long channel, const player_side_stream *s, int n) {
    (void) user;
    pthread_mutex_lock(&g_lock);
    int ttx_index = -1, ait_index = -1;
    g_nttxsub = 0;
    g_ttx_initial = 0;
    int ntextsub = 0;
    for (int i = 0; i < n; i++) {
        ntextsub += s[i].textsub;
        if (s[i].teletext && ttx_index < 0) {
            ttx_index = s[i].index;
            g_ttx_initial = s[i].ttx_initial;
        }
        if (s[i].ait && ait_index < 0) ait_index = s[i].index;
        if (s[i].textsub && s[i].ttx_page && g_nttxsub < MAX_TTXSUB) {
            ttxsub *t = &g_ttxsub[g_nttxsub++];
            t->page = s[i].ttx_page;
            t->type = s[i].sub_type;
            snprintf(t->lang, sizeof(t->lang), "%s", s[i].lang);
        }
    }
    if (channel != g_channel || ait_index < 0) ait_clear(); /* same channel: tvh only sends new versions */
    g_ait_index = ait_index;
    /* stream events: on the same channel (reconnect) the versions seen stay,
     * nothing fires twice */
    if (channel != g_channel) se_clear();
    int ndsmcc = 0;
    for (int i = 0; i < n && ndsmcc < MAX_DSMCC; i++) {
        if (!s[i].dsmcc) continue;
        dsmccstream *d = &g_dsmcc[ndsmcc];
        if (ndsmcc >= g_ndsmcc || d->index != s[i].index) {
            memset(d, 0, sizeof(*d));
            for (int k = 0; k < SE_IDS; k++) d->seen[k].id = -1;
        }
        d->index = s[i].index;
        d->tag = s[i].component_tag;
        ndsmcc++;
    }
    g_ndsmcc = ndsmcc;
    if (channel != g_channel || ttx_index < 0) {
        ttxdec_free(g_ttx);
        g_ttx = NULL;
    } else if (g_ttx) {
        /* same channel reconnected: keep pages, new time base */
        ttxdec_reset_input(g_ttx);
        ttxdec_clear_events(g_ttx);
    }
    if (ttx_index >= 0 && !g_ttx) g_ttx = ttxdec_new();
    g_ttx_index = ttx_index;
    g_channel = channel;
    drop_dvb();
    for (int i = 0; i < n && g_ndvb < MAX_DVB; i++) {
        if (s[i].teletext || s[i].ait || s[i].textsub || s[i].dsmcc) continue;
        dvbstream *d = &g_dvb[g_ndvb];
        d->index = s[i].index;
        snprintf(d->lang, sizeof(d->lang), "%s", s[i].lang);
        d->comp = s[i].composition;
        d->anc = s[i].ancillary;
        d->type = s[i].sub_type;
        d->dec = dvbsubdec_new(d->comp, d->anc);
        if (d->dec) g_ndvb++;
    }
    int pages = ttxdec_page_count(g_ttx), ndvb = g_ndvb, nttxsub = g_nttxsub, initial = g_ttx_initial;
    char se[64] = "";
    for (int i = 0; i < g_ndsmcc; i++) {
        size_t o = strlen(se);
        snprintf(se + o, sizeof(se) - o, "%s%d", i ? "," : "", g_dsmcc[i].tag);
    }
    pthread_mutex_unlock(&g_lock);
    /* subtitle pages from the PMT only if tvh does not name their pages (tvh
     * without HTSP extension: TEXTSUB streams without page) */
    if (ttx_index >= 0 && ntextsub > nttxsub) pmtinfo_lookup(channel);
    mt_log("[side] channel %lu: teletext %s%s, initial page %03X, %d subtitle page(s) from tvh, %d DVB subtitle "
           "stream(s)%s%s",
           channel, ttx_index >= 0 ? "yes" : "no", pages ? " (pages kept)" : "", initial, nttxsub, ndvb,
           se[0] ? ", stream events tag " : "", se);
}

static void on_data(void *user, int index, const uint8_t *d, size_t len, long long pts) {
    (void) user;
    pthread_mutex_lock(&g_lock);
    if (index == g_ttx_index) {
        ttxdec_feed(g_ttx, d, len, pts);
    } else if (index == g_ait_index) {
        ait_section(d, len); /* followed by an htsmsg with the same apps: discarded */
    } else {
        for (int i = 0; i < g_ndsmcc; i++)
            if (g_dsmcc[i].index == index) {
                se_section(&g_dsmcc[i], d, len);
                pthread_mutex_unlock(&g_lock);
                return;
            }
        for (int i = 0; i < g_ndvb; i++)
            if (g_dvb[i].index == index) dvbsubdec_feed(g_dvb[i].dec, d, len, pts);
    }
    pthread_mutex_unlock(&g_lock);
}

static void on_skip(void *user) {
    (void) user;
    pthread_mutex_lock(&g_lock);
    if (g_ttx) {
        ttxdec_reset_input(g_ttx);
        ttxdec_clear_events(g_ttx);
    }
    for (int i = 0; i < g_ndvb; i++) dvbsubdec_reset(g_dvb[i].dec);
    pthread_mutex_unlock(&g_lock);
}

void sidedata_reset(void) {
    pthread_mutex_lock(&g_lock);
    ttxdec_free(g_ttx);
    g_ttx = NULL;
    g_ttx_index = -1;
    g_channel = 0;
    g_nttxsub = 0;
    g_ttx_initial = 0;
    drop_dvb();
    ait_clear();
    g_ait_index = -1;
    se_clear();
    pthread_mutex_unlock(&g_lock);
}

int sidedata_has_teletext(void) {
    pthread_mutex_lock(&g_lock);
    int r = g_ttx != NULL;
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* ---------------- Queries ("local:ttx/...") ---------------- */

/* value of a query parameter (without decoding - numbers only) */
static int qparam(const char *q, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    while (q && *q) {
        if (!strncmp(q, name, nl) && q[nl] == '=') {
            const char *v = q + nl + 1;
            size_t l = strcspn(v, "&");
            if (l >= n) l = n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return 1;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
    return 0;
}

static int reply(char **body, size_t *len, char *json, int missing_status) {
    if (!json) {
        *body = strdup(missing_status == 404 ? "not found" : "error");
        *len = *body ? strlen(*body) : 0;
        return missing_status;
    }
    *body = json;
    *len = strlen(json);
    return 200;
}

static int local_get(const char *path, char **body, size_t *len) {
    *body = NULL;
    *len = 0;
    if (strncmp(path, "ttx/", 4)) return 404;
    char what[32];
    const char *p = path + 4, *q = strchr(p, '?');
    size_t wl = q ? (size_t) (q - p) : strlen(p);
    if (wl >= sizeof(what)) return 404;
    memcpy(what, p, wl);
    what[wl] = 0;
    if (q) q++;
    char v[32];
    int status;
    pthread_mutex_lock(&g_lock);
    if (!strcmp(what, "hbbtv")) {
        /* 404: the subscription has an AIT stream, but no section yet */
        int have = 0;
        for (int i = 0; i < AIT_SECTIONS; i++) have |= g_ait_sec[i] != NULL;
        if (g_ait_index >= 0 && !have) {
            status = reply(body, len, NULL, 404);
        } else {
            strbuf j = {0};
            sb_add(&j, "{\"apps\":[");
            int first = 1;
            for (int i = 0; i < AIT_SECTIONS; i++)
                if (g_ait_sec[i] && *g_ait_sec[i]) {
                    sb_addf(&j, "%s%s", first ? "" : ",", g_ait_sec[i]);
                    first = 0;
                }
            sb_add(&j, "]}");
            status = reply(body, len, sb_take(&j, NULL), 500);
        }
    } else if (!strcmp(what, "dvbsub")) {
        if (qparam(q, "pid", v, sizeof(v))) {
            int pid = atoi(v), page = qparam(q, "page", v, sizeof(v)) ? atoi(v) : 1;
            uint64_t since = qparam(q, "since", v, sizeof(v)) ? strtoull(v, NULL, 10) : 0;
            char *j = NULL;
            for (int i = 0; i < g_ndvb && !j; i++)
                if (g_dvb[i].index == pid && g_dvb[i].comp == page) j = dvbsubdec_events_json(g_dvb[i].dec, since);
            status = reply(body, len, j, 404);
        } else {
            strbuf j = {0};
            sb_add(&j, "{\"streams\":[");
            for (int i = 0; i < g_ndvb; i++) {
                char lang[4] = "";
                size_t o = 0;
                for (const char *c = g_dvb[i].lang; *c && o < 3; c++)
                    if (isalnum((unsigned char) *c)) lang[o++] = *c;
                lang[o] = 0;
                /* kind (0x20.. hearing impaired): only tvh with HTSP extension, otherwise 0 */
                sb_addf(&j, "%s{\"pid\":%d,\"lang\":\"%s\",\"type\":%d,\"page\":%d}", i ? "," : "", g_dvb[i].index,
                        lang, g_dvb[i].type, g_dvb[i].comp);
            }
            sb_add(&j, "]}");
            status = reply(body, len, sb_take(&j, NULL), 500);
        }
    } else if (!g_ttx) {
        status = reply(body, len, NULL, 404);
    } else if (!strcmp(what, "subtitles")) {
        if (qparam(q, "page", v, sizeof(v))) {
            int page = (int) strtoul(v, NULL, 16);
            uint64_t since = qparam(q, "since", v, sizeof(v)) ? strtoull(v, NULL, 10) : 0;
            status = reply(body, len, ttxdec_subtitles_json(g_ttx, page, since), 500);
        } else {
            /* according to tvh or the PMT: only the programme's pages (a shared
             * teletext also carries those of other channels); otherwise all with
             * the subtitle bit */
            pmtinfo_page pp[MAX_TTXSUB];
            int np = g_nttxsub;
            const char *source = "tvh";
            for (int i = 0; i < np; i++) {
                pp[i].page = g_ttxsub[i].page;
                pp[i].type = g_ttxsub[i].type;
                snprintf(pp[i].lang, sizeof(pp[i].lang), "%s", g_ttxsub[i].lang);
            }
            if (!np) {
                np = pmtinfo_pages(g_channel, pp, MAX_TTXSUB);
                source = "pmt";
            }
            if (np > 0) {
                strbuf j = {0};
                sb_add(&j, "{\"pages\":[");
                for (int i = 0; i < np; i++) sb_addf(&j, "%s\"%03X\"", i ? "," : "", pp[i].page);
                sb_add(&j, "],\"info\":[");
                for (int i = 0; i < np; i++)
                    sb_addf(&j, "%s{\"page\":\"%03X\",\"lang\":\"%s\",\"type\":%d}", i ? "," : "", pp[i].page,
                            pp[i].lang, pp[i].type);
                sb_addf(&j, "],\"source\":\"%s\"}", source);
                status = reply(body, len, sb_take(&j, NULL), 500);
            } else {
                status = reply(body, len, ttxdec_subtitle_pages_json(g_ttx), 500);
            }
        }
    } else if (!strcmp(what, "pages")) {
        status = reply(body, len, ttxdec_pages_json(g_ttx), 500);
    } else {
        char *end = NULL;
        unsigned long page = strtoul(what, &end, 16);
        if (strlen(what) != 3 || !end || *end || page < 0x100 || page > 0x8FF) {
            status = reply(body, len, NULL, 404);
        } else {
            int sub = qparam(q, "sub", v, sizeof(v)) ? (int) strtoul(v, NULL, 16) : -1;
            status = reply(body, len, ttxdec_page_json(g_ttx, (int) page, sub), 404);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return status;
}

void sidedata_set_ttx_initial(int page) {
    pthread_mutex_lock(&g_lock);
    if (page && !g_ttx_initial) g_ttx_initial = page;
    pthread_mutex_unlock(&g_lock);
}

int sidedata_ttx_initial_page(void) {
    pthread_mutex_lock(&g_lock);
    int p = g_ttx ? g_ttx_initial : 0;
    pthread_mutex_unlock(&g_lock);
    return p;
}

void sidedata_init(player *p) {
    player_side s = {.start = on_start, .data = on_data, .skip = on_skip, .user = NULL};
    player_set_side(p, &s);
    fetch_set_local(local_get);
}
