/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "i18n.h"

#include "cJSON.h"
#include "log.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *g_doc;          /* owns the strings the table points to */
static const char **g_keys, **g_vals;
static size_t g_cap;          /* power of two, 0 = no table */
static char g_lang[16] = "en";
static char g_res[512];
static cJSON *g_iso; /* res/langnames/<language>.json: ISO 639-2 code -> name */

static char *read_file(const char *path, size_t *len);

static void load_iso(const char *code) {
    char path[600];
    snprintf(path, sizeof(path), "%s/langnames/%s.json", g_res, code);
    size_t len = 0;
    char *txt = read_file(path, &len);
    cJSON *doc = txt ? cJSON_ParseWithLength(txt, len) : NULL;
    free(txt);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        if (strcmp(code, "en")) load_iso("en");
        return;
    }
    g_iso = doc;
}

static uint32_t hash(const char *s) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for (; *s; s++) h = (h ^ (uint8_t) *s) * 16777619u;
    return h;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *len = (size_t) n;
    }
    return buf;
}

void i18n_init(const char *res_dir, const char *lang) {
    snprintf(g_res, sizeof(g_res), "%s", res_dir ? res_dir : "");
    char code[16] = "";
    for (size_t i = 0; lang && lang[i] && lang[i] != '-' && lang[i] != '_' && i + 1 < sizeof(code); i++)
        code[i] = (char) (lang[i] >= 'A' && lang[i] <= 'Z' ? lang[i] - 'A' + 'a' : lang[i]);
    load_iso(code[0] ? code : "en");
    if (!code[0] || !strcmp(code, "en")) {
        mt_log("[i18n] language en (source texts)");
        return;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/lang/%s.json", res_dir, code);
    size_t len = 0;
    char *txt = read_file(path, &len);
    cJSON *doc = txt ? cJSON_ParseWithLength(txt, len) : NULL;
    free(txt);
    if (!cJSON_IsObject(doc)) {
        mt_log("[i18n] no translation %s - English", path);
        cJSON_Delete(doc);
        return;
    }
    size_t n = (size_t) cJSON_GetArraySize(doc), cap = 64;
    while (cap < n * 2) cap *= 2;
    g_keys = calloc(cap, sizeof(*g_keys));
    g_vals = calloc(cap, sizeof(*g_vals));
    if (!g_keys || !g_vals) {
        free(g_keys);
        free(g_vals);
        g_keys = g_vals = NULL;
        cJSON_Delete(doc);
        return;
    }
    size_t used = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, doc) {
        if (!cJSON_IsString(e) || !e->string || !e->valuestring[0]) continue;
        size_t i = hash(e->string) & (cap - 1);
        while (g_keys[i] && strcmp(g_keys[i], e->string)) i = (i + 1) & (cap - 1);
        if (!g_keys[i]) used++;
        g_keys[i] = e->string;
        g_vals[i] = e->valuestring;
    }
    g_doc = doc;
    g_cap = cap;
    snprintf(g_lang, sizeof(g_lang), "%s", code);
    mt_log("[i18n] language %s: %zu texts", g_lang, used);
}

const char *i18n_lang(void) { return g_lang; }

const char *i18n_iso639(const char *code) {
    char c[4] = "";
    for (int i = 0; code && code[i] && i < 3; i++)
        c[i] = (char) (code[i] >= 'A' && code[i] <= 'Z' ? code[i] - 'A' + 'a' : code[i]);
    if (!c[0]) return NULL;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(g_iso, c);
    return cJSON_IsString(e) ? e->valuestring : NULL;
}

char i18n_decimal(void) { return strcmp(g_lang, "en") ? ',' : '.'; }

const char *tr(const char *msgid) {
    if (!g_cap || !msgid) return msgid;
    size_t i = hash(msgid) & (g_cap - 1);
    while (g_keys[i]) {
        if (!strcmp(g_keys[i], msgid)) return g_vals[i];
        i = (i + 1) & (g_cap - 1);
    }
    return msgid;
}

static int cmp_code(const void *a, const void *b) { return strcmp(*(const char *const *) a, *(const char *const *) b); }

int i18n_languages(const char **codes, int max) {
    static char found[24][16];
    int n = 0, nf = 0;
    if (max > 0) codes[n++] = "en";
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/lang", g_res);
    DIR *d = g_res[0] ? opendir(dir) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && n < max && nf < 24) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l - 5 >= sizeof(found[0]) || strcmp(e->d_name + l - 5, ".json")) continue;
        memcpy(found[nf], e->d_name, l - 5);
        found[nf][l - 5] = 0;
        if (strcmp(found[nf], "en")) codes[n++] = found[nf++];
    }
    if (d) closedir(d);
    if (n > 2) qsort(codes + 1, (size_t) (n - 1), sizeof(codes[0]), cmp_code);
    return n;
}

const char *i18n_language_name(const char *code) {
    static const char *const names[][2] = {
        {"de", "Deutsch"},  {"en", "English"},    {"es", "Espa\xC3\xB1ol"}, {"fr", "Fran\xC3\xA7" "ais"},
        {"it", "Italiano"}, {"nl", "Nederlands"}, {"pl", "Polski"},          {"pt", "Portugu\xC3\xAAs"},
        {"cs", "\xC4\x8C" "e\xC5\xA1tina"},   {"hu", "Magyar"},           {"tr", "T\xC3\xBCrk\xC3\xA7" "e"},
        {"da", "Dansk"},    {"sv", "Svenska"},    {"no", "Norsk"},            {"fi", "Suomi"}};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(names[i][0], code)) return names[i][1];
    return code;
}
