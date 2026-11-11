/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* FNM_CASEFOLD */
#include "egress.h"

#include "cJSON.h"
#include "log.h"

#include <fnmatch.h>
#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char *pat;   /* host pattern (fnmatch, *.de) */
    char *proxy; /* "" = direct */
} host_rule;

typedef struct {
    regex_t re;
    char *proxy;
} chan_rule;

static host_rule *g_over, *g_routes;
static int g_nover, g_nroutes;
static chan_rule *g_chan;
static int g_nchan;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cur = -1;       /* channel rule of the current HbbTV channel, -1 = none */
static char g_cur_name[96];
static char g_host[64];      /* for {host} */
static cJSON *g_egress;      /* the file's "egress" (names -> URL), for the tags */

/* Channel rules from tvh tags (under g_lock) */
typedef struct {
    char *channel; /* channel name as in tvh */
    char *proxy;   /* "" = direct */
} tag_rule;
static tag_rule *g_tag;
static int g_ntag;
static char g_tag_proxy[256]; /* egress of the current channel according to its tag */
static int g_tag_cur;         /* 1 = a tag applies to the current channel */

/* the "Internet access" setting for channels without a tag: the file's
 * rules, direct, or one fixed proxy (set once at the start) */
enum { MODE_RULES, MODE_DIRECT, MODE_FIXED };
static int g_mode = MODE_RULES;
static char g_fixed[256];

/* Egress from the file -> proxy URL ("" = direct) */
static char *resolve(const cJSON *egress, const char *v, const char *host) {
    const char *u = v;
    if (!strcasecmp(v, "direct") || !strcasecmp(v, "direkt")) u = "";
    else {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(egress, v);
        if (cJSON_IsString(e)) u = e->valuestring;
    }
    char out[256];
    const char *h = strstr(u, "{host}");
    if (h) snprintf(out, sizeof(out), "%.*s%s%s", (int) (h - u), u, host, h + 6);
    else snprintf(out, sizeof(out), "%s", u);
    return strdup(out);
}

static int load_hosts(const cJSON *arr, const cJSON *egress, const char *host, host_rule **out) {
    int n = cJSON_GetArraySize(arr), k = 0;
    *out = n > 0 ? calloc((size_t) n, sizeof(host_rule)) : NULL;
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) {
        const cJSON *p = cJSON_GetArrayItem(r, 0), *v = cJSON_GetArrayItem(r, 1);
        if (!*out || !cJSON_IsString(p) || !cJSON_IsString(v)) continue;
        (*out)[k].pat = strdup(p->valuestring);
        (*out)[k].proxy = resolve(egress, v->valuestring, host);
        k++;
    }
    return k;
}

int egress_load(const char *path, const char *host) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        mt_log("[egress] %s missing - all direct", path ? path : "-");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (t && fread(t, 1, (size_t) n, f) == (size_t) n) t[n] = 0;
    fclose(f);
    cJSON *j = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!j) {
        mt_log("[egress] %s: not JSON", path);
        return -1;
    }
    const cJSON *eg = cJSON_GetObjectItemCaseSensitive(j, "egress");
    snprintf(g_host, sizeof(g_host), "%s", host ? host : "");
    g_egress = cJSON_Duplicate(eg, 1);
    g_nover = load_hosts(cJSON_GetObjectItemCaseSensitive(j, "overrides"), eg, host, &g_over);
    g_nroutes = load_hosts(cJSON_GetObjectItemCaseSensitive(j, "routes"), eg, host, &g_routes);
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(j, "channels"), *r;
    int nc = cJSON_GetArraySize(ch);
    g_chan = nc > 0 ? calloc((size_t) nc, sizeof(chan_rule)) : NULL;
    cJSON_ArrayForEach(r, ch) {
        const cJSON *p = cJSON_GetArrayItem(r, 0), *v = cJSON_GetArrayItem(r, 1);
        if (!g_chan || !cJSON_IsString(p) || !cJSON_IsString(v)) continue;
        /* case-insensitive: channels are also called "zdf_neo HD" */
        if (regcomp(&g_chan[g_nchan].re, p->valuestring, REG_EXTENDED | REG_NOSUB | REG_ICASE) != 0) {
            mt_log("[egress] invalid channel rule: %s", p->valuestring);
            continue;
        }
        g_chan[g_nchan].proxy = resolve(eg, v->valuestring, host);
        g_nchan++;
    }
    cJSON_Delete(j);
    mt_log("[egress] %d channel rules, %d host rules, %d overrides", g_nchan, g_nroutes, g_nover);
    return 0;
}

void egress_set_channel(const char *name) {
    int idx = -1;
    for (int i = 0; name && *name && i < g_nchan && idx < 0; i++)
        if (regexec(&g_chan[i].re, name, 0, NULL, 0) == 0) idx = i;
    pthread_mutex_lock(&g_lock);
    g_cur = idx;
    g_tag_cur = 0;
    for (int i = 0; name && *name && i < g_ntag && !g_tag_cur; i++)
        if (!strcmp(g_tag[i].channel, name)) {
            g_tag_cur = 1;
            snprintf(g_tag_proxy, sizeof(g_tag_proxy), "%s", g_tag[i].proxy);
        }
    snprintf(g_cur_name, sizeof(g_cur_name), "%s", name ? name : "");
    int tag = g_tag_cur, mode = g_mode;
    char tp[256];
    snprintf(tp, sizeof(tp), "%s", tag ? g_tag_proxy : g_fixed);
    pthread_mutex_unlock(&g_lock);
    if (name && *name) {
        if (tag) mt_log("[egress] channel %s: %s (tvh tag)", name, tp[0] ? tp : "direct");
        else if (mode == MODE_FIXED) mt_log("[egress] channel %s: %s (fixed proxy)", name, tp);
        else if (mode == MODE_DIRECT) mt_log("[egress] channel %s: direct (setting)", name);
        else
            mt_log("[egress] channel %s: %s", name,
                   idx < 0 ? "no channel rule (host rules)" : g_chan[idx].proxy[0] ? g_chan[idx].proxy : "direct");
    }
}

static const char *match_host(const host_rule *r, int n, const char *host) {
    for (int i = 0; i < n; i++)
        if (fnmatch(r[i].pat, host, FNM_CASEFOLD) == 0 ||
            (!strncmp(r[i].pat, "*.", 2) && !strcasecmp(r[i].pat + 2, host)))
            return r[i].proxy;
    return NULL;
}

/* Hosts on the local network (tvh, gateway, proxies) always direct */
static int local_host(const char *h) {
    return !strncmp(h, "192.168.", 8) || !strncmp(h, "10.", 3) || !strcmp(h, "127.0.0.1") || !strcasecmp(h, "localhost") ||
           !strncmp(h, "172.16.", 7);
}

/* host:port is enough for a proxy: adds http:// (malloc) */
static char *with_scheme(char *p) {
    if (!p || !*p || strstr(p, "://")) return p;
    char *u = malloc(strlen(p) + 8);
    if (u) sprintf(u, "http://%s", p);
    free(p);
    return u;
}

void egress_set_mode(const char *setting) {
    char *fixed = NULL;
    int mode = MODE_RULES;
    if (setting && !strcasecmp(setting, "direct")) mode = MODE_DIRECT;
    else if (setting && *setting && strcasecmp(setting, "rules")) {
        mode = MODE_FIXED;
        fixed = with_scheme(strdup(setting));
    }
    if (mode == MODE_FIXED && !fixed) mode = MODE_DIRECT;
    pthread_mutex_lock(&g_lock);
    g_mode = mode;
    snprintf(g_fixed, sizeof(g_fixed), "%s", fixed ? fixed : "");
    pthread_mutex_unlock(&g_lock);
    mt_log("[egress] channels without a tvh tag: %s",
           mode == MODE_RULES ? "rules (egress.json)" : mode == MODE_DIRECT ? "direct" : fixed);
    free(fixed);
}

/* Order: local hosts direct > in rules mode the file's host exceptions >
 * the current channel's tvh tag > the setting: the file's channel and host
 * rules, direct, or the fixed proxy */
const char *egress_for(const char *host, char *buf, size_t n) {
    buf[0] = 0;
    if (!host || !*host || local_host(host)) return buf;
    pthread_mutex_lock(&g_lock);
    int mode = g_mode;
    const char *p = mode == MODE_RULES ? match_host(g_over, g_nover, host) : NULL;
    if (!p) {
        if (g_tag_cur) {
            snprintf(buf, n, "%s", g_tag_proxy);
            pthread_mutex_unlock(&g_lock);
            return buf;
        }
        if (mode == MODE_FIXED) {
            snprintf(buf, n, "%s", g_fixed);
            pthread_mutex_unlock(&g_lock);
            return buf;
        }
        if (mode == MODE_RULES && g_cur >= 0) p = g_chan[g_cur].proxy;
    }
    pthread_mutex_unlock(&g_lock);
    if (!p && mode == MODE_RULES) p = match_host(g_routes, g_nroutes, host);
    if (p) snprintf(buf, n, "%s", p);
    return buf;
}

const char *egress_channel_proxy(char *buf, size_t n) {
    buf[0] = 0;
    pthread_mutex_lock(&g_lock);
    if (g_tag_cur) snprintf(buf, n, "%s", g_tag_proxy);
    else if (g_mode == MODE_FIXED) snprintf(buf, n, "%s", g_fixed);
    else if (g_mode == MODE_RULES && g_cur >= 0) snprintf(buf, n, "%s", g_chan[g_cur].proxy);
    pthread_mutex_unlock(&g_lock);
    return buf;
}

/* ---------------- Channel rules from tvh tags ---------------- */

#define TAG_PREFIX "HbbTV-Proxy"

char *egress_tag_proxy(const char *name, const char *icon) {
    if (!name || strncasecmp(name, TAG_PREFIX, strlen(TAG_PREFIX))) return NULL;
    const char *v = icon ? icon : "";
    while (*v == ' ') v++;
    if (!*v) {
        v = name + strlen(TAG_PREFIX);
        while (*v == ' ' || *v == '-' || *v == ':' || *v == '_') v++;
    }
    if (!*v) return NULL;
    pthread_mutex_lock(&g_lock);
    char *p = resolve(g_egress, v, g_host);
    pthread_mutex_unlock(&g_lock);
    /* host:port is enough (in the icon field tvh turns a URL into an imagecache link) */
    return with_scheme(p);
}

void egress_set_tag_rules(const char *const *channels, const char *const *proxies, int n) {
    tag_rule *r = n > 0 ? calloc((size_t) n, sizeof(*r)) : NULL;
    int k = 0;
    for (int i = 0; r && i < n; i++) {
        r[k].channel = strdup(channels[i] ? channels[i] : "");
        r[k].proxy = strdup(proxies[i] ? proxies[i] : "");
        if (r[k].channel && r[k].proxy) k++;
        else {
            free(r[k].channel);
            free(r[k].proxy);
        }
    }
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ntag; i++) {
        free(g_tag[i].channel);
        free(g_tag[i].proxy);
    }
    free(g_tag);
    g_tag = r;
    g_ntag = k;
    pthread_mutex_unlock(&g_lock);
}
