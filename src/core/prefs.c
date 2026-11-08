/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "prefs.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The settings file lives in /media/developer/temp: it is writable for everyone,
 * lies inside the app's jail and survives reinstalling. The app folder is no
 * place for it: ares-install replaces that folder (and on some TVs the app
 * cannot even create a file there). */
#ifndef DATA_DIR
#define DATA_DIR "/media/developer/temp/com.lsd.tvf"
#endif

static cJSON *g_root;
static char g_path[512];

static int try_dir(const char *dir) {
    if (!dir || !*dir) return 0;
    char test[512];
    snprintf(test, sizeof(test), "%s/.tvf-test", dir);
    FILE *f = fopen(test, "w");
    if (!f) return 0;
    fclose(f);
    unlink(test);
    snprintf(g_path, sizeof(g_path), "%s/tvf-prefs.json", dir);
    return 1;
}

#define PREFS_MAX (8 << 20) /* it is never bigger - if it is, something is wrong */

/* the whole file; *exists = 1 if it exists (even if it is not readable) */
static cJSON *read_json(const char *path, int *exists) {
    *exists = 0;
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    *exists = 1;
    cJSON *j = NULL;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    char *buf = n >= 0 && n <= PREFS_MAX && fseek(f, 0, SEEK_SET) == 0 ? malloc((size_t) n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t) n, f) == (size_t) n) {
        buf[n] = 0;
        j = cJSON_Parse(buf);
    }
    free(buf);
    fclose(f);
    return j;
}

void prefs_init(const char *app_dir) {
    if (mkdir(DATA_DIR, 0700) != 0 && errno != EEXIST) mt_log("[prefs] %s: %s", DATA_DIR, strerror(errno));
    int data_dir = try_dir(DATA_DIR);
    if (!data_dir && !try_dir(getenv("HOME")) && !try_dir(app_dir) && !try_dir("/tmp")) g_path[0] = 0;
    mt_log("[prefs] file %s (HOME=%s)", g_path[0] ? g_path : "NONE", getenv("HOME") ? getenv("HOME") : "-");
    int exists = 0;
    g_root = read_json(g_path, &exists);
    if (!g_root && exists) {
        /* present but not readable: move it aside - otherwise the next
         * prefs_set would replace it with an empty one (credentials gone) */
        char bad[600];
        snprintf(bad, sizeof(bad), "%s.unreadable", g_path);
        int r = rename(g_path, bad);
        mt_log("[prefs] %s unreadable - %s %s", g_path, r == 0 ? "moved to" : "could not move to", bad);
    }
    if (!g_root && data_dir) {
        /* first start with this location: take over the file of the older
         * versions from the app folder (if a reinstall has not removed it) */
        char old[512];
        snprintf(old, sizeof(old), "%s/tvf-prefs.json", getenv("HOME") ? getenv("HOME") : app_dir);
        int old_exists = 0;
        g_root = read_json(old, &old_exists);
        if (cJSON_IsObject(g_root)) {
            mt_log("[prefs] migrated from %s", old);
            prefs_set("prefs.moved", cJSON_CreateTrue()); /* writes the new file */
            if (access(g_path, R_OK) == 0) unlink(old); /* credentials only in one place */
        }
    }
    if (!cJSON_IsObject(g_root)) {
        cJSON_Delete(g_root);
        g_root = cJSON_CreateObject();
    }
}

const cJSON *prefs_get(const char *key) { return g_root ? cJSON_GetObjectItemCaseSensitive(g_root, key) : NULL; }

void prefs_set(const char *key, cJSON *value) {
    if (!g_root) {
        cJSON_Delete(value);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(g_root, key);
    if (value) cJSON_AddItemToObject(g_root, key, value);
    if (!g_path[0]) return;
    char *s = cJSON_PrintUnformatted(g_root);
    if (!s) return;
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.neu", g_path);
    /* readable only for TVF (credentials), do not follow a symlink; first onto
     * the disk, then replace */
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        size_t len = strlen(s), off = 0;
        int ok = 1;
        while (ok && off < len) {
            ssize_t w = write(fd, s + off, len - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) ok = 0;
            else off += (size_t) w;
        }
        ok = fsync(fd) == 0 && ok;
        ok = close(fd) == 0 && ok;
        if (ok && rename(tmp, g_path) == 0) {
            free(s);
            return;
        }
        int e = errno;
        unlink(tmp);
        errno = e;
    }
    mt_log("[prefs] %s not written: %s", g_path, strerror(errno));
    free(s);
}
