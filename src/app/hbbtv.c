/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hbbtv.h"

#include "cJSON.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>

static char *rewrites; /* JSON array from entry.json or NULL */

const char *hbbtv_rewrites(void) { return rewrites; }

int hbbtv_load(const char *path) {
    free(rewrites);
    rewrites = NULL;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        mt_log("[hbbtv] %s missing - no entry rewrites", path ? path : "-");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (t && fread(t, 1, (size_t) n, f) == (size_t) n) t[n] = 0;
    else {
        free(t);
        t = NULL;
    }
    fclose(f);
    cJSON *j = t ? cJSON_Parse(t) : NULL;
    free(t);
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "rewrites");
    if (!cJSON_IsArray(r)) {
        mt_log("[hbbtv] %s: no \"rewrites\" list", path);
        cJSON_Delete(j);
        return -1;
    }
    if (cJSON_GetArraySize(r) > 0) rewrites = cJSON_PrintUnformatted(r);
    mt_log("[hbbtv] %d entry rewrite(s)", cJSON_GetArraySize(r));
    cJSON_Delete(j);
    return 0;
}
