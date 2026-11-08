/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* MSG_CMSG_CLOEXEC */
#include "webr.h"

#include "log.h"
#include "ui_sdl.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Helper message; fd = memfd sent along (ready/buffers), otherwise -1 */
typedef struct wmsg {
    struct wmsg *next;
    int fd;
    char text[];
} wmsg;

static webr_actions act;
static void (*g_wake)(void);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static wmsg *g_head, *g_tail; /* under g_lock */

static int g_sock = -1; /* our end of the socketpair */
static pid_t g_pid;
static int g_alive;     /* reader thread running (socket open) */
static int g_ready;
/* Buffers (main thread) */
static uint8_t *g_map;
static size_t g_map_len, g_bufsize;
static int g_w, g_h, g_stride;
static SDL_Texture *g_tex;
static int g_have_frame;

static void push(int fd, const char *text, size_t len) {
    wmsg *m = malloc(sizeof(wmsg) + len + 1);
    if (!m) {
        if (fd >= 0) close(fd);
        return;
    }
    m->next = NULL;
    m->fd = fd;
    memcpy(m->text, text, len);
    m->text[len] = 0;
    pthread_mutex_lock(&g_lock);
    if (g_tail) g_tail->next = m;
    else g_head = m;
    g_tail = m;
    pthread_mutex_unlock(&g_lock);
    if (g_wake) g_wake();
}

/* Reader thread: assemble lines, assign fds to the ready/buffers messages
 * (they arrive with the first byte of their message; only these carry one) */
static void *reader(void *arg) {
    int fd = (int) (intptr_t) arg;
    size_t cap = 64 * 1024, have = 0;
    char *line = malloc(cap);
    int fds[8], nfds = 0;
    while (line) {
        char data[16384];
        char cbuf[CMSG_SPACE(sizeof(int) * 4)];
        struct iovec iov = {data, sizeof(data)};
        struct msghdr mh;
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        ssize_t n = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int k = (int) ((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            int *fp = (int *) (void *) CMSG_DATA(c);
            for (int i = 0; i < k; i++) {
                if (nfds < (int) (sizeof(fds) / sizeof(fds[0]))) fds[nfds++] = fp[i];
                else close(fp[i]);
            }
        }
        for (ssize_t i = 0; i < n; i++) {
            if (data[i] != '\n') {
                if (have + 1 >= cap) {
                    if (cap >= (16u << 20)) {
                        have = 0; /* nonsense: discard */
                        continue;
                    }
                    char *nl = realloc(line, cap * 2);
                    if (!nl) break;
                    line = nl;
                    cap *= 2;
                }
                line[have++] = data[i];
                continue;
            }
            line[have] = 0;
            int mfd = -1;
            if (nfds > 0 && (strstr(line, "\"event\":\"ready\"") || strstr(line, "\"event\":\"buffers\""))) {
                mfd = fds[0];
                memmove(fds, fds + 1, (size_t) (nfds - 1) * sizeof(int));
                nfds--;
            }
            if (have) push(mfd, line, have);
            else if (mfd >= 0) close(mfd);
            have = 0;
        }
    }
    free(line);
    while (nfds > 0) close(fds[--nfds]);
    static const char eof[] = "{\"event\":\"exit\",\"eof\":true}";
    push(-1, eof, sizeof(eof) - 1);
    return NULL;
}

/* Helper's stderr into the log */
static void *errlog(void *arg) {
    FILE *f = fdopen((int) (intptr_t) arg, "r");
    if (!f) return NULL;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (n) mt_log("[web] %s", buf);
    }
    fclose(f);
    return NULL;
}

static void unmap(void) {
    if (g_map) munmap(g_map, g_map_len);
    g_map = NULL;
    g_map_len = 0;
}

void webr_init(const webr_actions *a, void (*wake)(void)) {
    act = *a;
    g_wake = wake;
}

int webr_running(void) { return g_sock >= 0; }
int webr_ready(void) { return g_sock >= 0 && g_ready; }
SDL_Texture *webr_texture(void) { return g_have_frame ? g_tex : NULL; }

static char g_media_sock[64]; /* "" = WebKit without media (media.js) */

void webr_set_media(const char *sock) { snprintf(g_media_sock, sizeof(g_media_sock), "%s", sock ? sock : ""); }

int webr_start(const char *bundle, const char *data_dir, int w, int h) {
    if (g_sock >= 0) return 0;
    char run[512];
    snprintf(run, sizeof(run), "%s/run.sh", bundle);
    if (access(run, R_OK) != 0) {
        mt_log("[web] %s missing", run);
        return -1;
    }
    if (data_dir && *data_dir) mkdir(data_dir, 0700);
    int sv[2], ep[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return -1;
    if (pipe2(ep, O_CLOEXEC) != 0) ep[0] = ep[1] = -1;
    /* Child: socket as fd 3, stderr into the pipe. dup2 onto itself would leave
     * CLOEXEC set - hence move it away from 3 first */
    int child = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
    close(sv[1]);
    char sw[16], sh[16];
    snprintf(sw, sizeof(sw), "%d", w);
    snprintf(sh, sizeof(sh), "%d", h);
    char *argv[14] = {"/bin/sh", run, "--fd", "3", "--width", sw, "--height", sh};
    int ac = 8;
    if (data_dir && *data_dir) {
        argv[ac++] = "--data-dir";
        argv[ac++] = (char *) data_dir;
    }
    /* Media in WebKit: the sinks in the WebProcess find the media server via
     * the environment (the WebProcess inherits it from the helper) */
    char envvar[96];
    char **envp = environ;
    if (g_media_sock[0]) {
        argv[ac++] = "--media";
        snprintf(envvar, sizeof(envvar), "TVF_MEDIA_SOCK=%s", g_media_sock);
        size_t n = 0;
        while (environ[n]) n++;
        envp = calloc(n + 2, sizeof(char *));
        if (envp) {
            size_t k = 0;
            for (size_t i = 0; i < n; i++)
                if (strncmp(environ[i], "TVF_MEDIA_SOCK=", 15)) envp[k++] = environ[i];
            envp[k++] = envvar;
            envp[k] = NULL;
        } else {
            envp = environ;
        }
    }
    argv[ac] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, child, 3);
    if (ep[1] >= 0) posix_spawn_file_actions_adddup2(&fa, ep[1], 2);
    int r = posix_spawn(&g_pid, "/bin/sh", &fa, NULL, argv, envp);
    if (envp != environ) free(envp);
    posix_spawn_file_actions_destroy(&fa);
    close(child);
    if (ep[1] >= 0) close(ep[1]);
    if (r != 0) {
        mt_log("[web] start: %s", strerror(r));
        close(sv[0]);
        if (ep[0] >= 0) close(ep[0]);
        return -1;
    }
    g_sock = sv[0];
    g_ready = 0;
    g_alive = 1;
    g_w = w;
    g_h = h;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, reader, (void *) (intptr_t) g_sock);
    if (ep[0] >= 0 && pthread_create(&t, &at, errlog, (void *) (intptr_t) ep[0]) != 0) close(ep[0]);
    pthread_attr_destroy(&at);
    mt_log("[web] helper started (pid %d, %dx%d%s%s)", (int) g_pid, w, h, g_media_sock[0] ? ", media on " : "",
           g_media_sock);
    return 0;
}

static void send_line(const char *json) {
    if (g_sock < 0 || !g_alive) return;
    size_t n = strlen(json);
    char *b = malloc(n + 2);
    if (!b) return;
    memcpy(b, json, n);
    b[n] = '\n';
    size_t off = 0;
    while (off < n + 1) {
        ssize_t w = send(g_sock, b + off, n + 1 - off, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (size_t) w;
    }
    free(b);
}

static void send_json(cJSON *j) {
    char *s = cJSON_PrintUnformatted(j);
    if (s) send_line(s);
    free(s);
    cJSON_Delete(j);
}

void webr_load(const char *url, const char *user_agent, const char *proxy) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "cmd", "load");
    cJSON_AddStringToObject(j, "url", url);
    if (user_agent && *user_agent) cJSON_AddStringToObject(j, "userAgent", user_agent);
    if (proxy) cJSON_AddStringToObject(j, "proxy", proxy);
    send_json(j);
}

void webr_key(int code) {
    char b[96];
    snprintf(b, sizeof(b), "{\"cmd\":\"key\",\"code\":%d,\"down\":true}", code);
    send_line(b);
    snprintf(b, sizeof(b), "{\"cmd\":\"key\",\"code\":%d,\"down\":false}", code);
    send_line(b);
}

void webr_pointer(const char *type, double x, double y) {
    char b[128];
    snprintf(b, sizeof(b), "{\"cmd\":\"pointer\",\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f}", type, x, y);
    send_line(b);
}

void webr_scroll(int dir, double x, double y) {
    char b[128];
    snprintf(b, sizeof(b), "{\"cmd\":\"scroll\",\"dy\":%d,\"x\":%.1f,\"y\":%.1f}", dir > 0 ? 1 : -1, x, y);
    send_line(b);
}

void webr_user_script(const char *js) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "cmd", "userScript");
    cJSON_AddStringToObject(j, "js", js);
    send_json(j);
}

void webr_clear_user_scripts(void) { send_line("{\"cmd\":\"clearUserScripts\"}"); }

void webr_eval(const char *js) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "cmd", "eval");
    cJSON_AddStringToObject(j, "js", js);
    cJSON_AddNumberToObject(j, "id", 0);
    send_json(j);
}

void webr_visible(int on) { send_line(on ? "{\"cmd\":\"visible\",\"on\":true}" : "{\"cmd\":\"visible\",\"on\":false}"); }

void webr_stop(void) {
    if (g_sock < 0) return;
    send_line("{\"cmd\":\"close\"}");
    shutdown(g_sock, SHUT_WR);
}

/* ready/buffers: new memfd with two buffers */
static void on_buffers(const cJSON *j, int fd) {
    unmap();
    g_ready = 0;
    g_have_frame = 0;
    if (fd < 0) {
        mt_log("[web] buffers without memfd");
        return;
    }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(j, "width"), *h = cJSON_GetObjectItemCaseSensitive(j, "height");
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "stride"), *bs = cJSON_GetObjectItemCaseSensitive(j, "bufferSize");
    const cJSON *nb = cJSON_GetObjectItemCaseSensitive(j, "buffers");
    int bw = cJSON_IsNumber(w) ? (int) w->valuedouble : 0, bh = cJSON_IsNumber(h) ? (int) h->valuedouble : 0;
    int n = cJSON_IsNumber(nb) ? (int) nb->valuedouble : 2;
    g_stride = cJSON_IsNumber(st) ? (int) st->valuedouble : bw * 4;
    g_bufsize = cJSON_IsNumber(bs) ? (size_t) bs->valuedouble : (size_t) g_stride * (size_t) bh;
    g_map_len = g_bufsize * (size_t) (n > 0 ? n : 2);
    void *p = bw > 0 && bh > 0 ? mmap(NULL, g_map_len, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (p == MAP_FAILED) {
        mt_log("[web] mmap %zu: %s", g_map_len, strerror(errno));
        g_map_len = 0;
        return;
    }
    g_map = p;
    if (!g_tex || bw != g_w || bh != g_h) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        /* linear scaling: SDL_SetTextureScaleMode only exists from SDL 2.0.12 on,
         * webOS 4 has 2.0.5 - a direct call terminated TVF there on the first
         * frame ("symbol lookup error"). Look it up at runtime (in the program,
         * no PIE, a weak symbol is never NULL), otherwise the hint applies
         * to the one new texture */
        static int (*set_scale)(SDL_Texture *, SDL_ScaleMode);
        static int looked_up;
        if (!looked_up) {
            looked_up = 1;
            void *self = dlopen(NULL, RTLD_NOW);
            if (self) set_scale = (int (*)(SDL_Texture *, SDL_ScaleMode)) dlsym(self, "SDL_SetTextureScaleMode");
            if (!set_scale) mt_log("[web] SDL without SDL_SetTextureScaleMode: scale quality hint");
        }
        char old_hint[16] = "";
        if (!set_scale) {
            const char *h = SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
            snprintf(old_hint, sizeof(old_hint), "%s", h ? h : "nearest");
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        }
        g_tex = SDL_CreateTexture(ui_renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, bw, bh);
        if (!set_scale) SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, old_hint);
        if (g_tex) {
            /* premultiplied, copied as the bottom layer (ui_sdl.c) */
            SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE);
            if (set_scale) set_scale(g_tex, SDL_ScaleModeLinear);
        } else {
            mt_log("[web] texture: %s", SDL_GetError());
        }
    }
    g_w = bw;
    g_h = bh;
    g_ready = 1;
}

static void on_frame(const cJSON *j) {
    const cJSON *fr = cJSON_GetObjectItemCaseSensitive(j, "frame");
    const cJSON *bf = cJSON_GetObjectItemCaseSensitive(j, "buf");
    const cJSON *dm = cJSON_GetObjectItemCaseSensitive(j, "damage");
    long seq = (long) fr->valuedouble;
    int buf = cJSON_IsNumber(bf) ? (int) bf->valuedouble : 0;
    if (g_map && g_tex && buf >= 0 && (size_t) (buf + 1) * g_bufsize <= g_map_len) {
        SDL_Rect r = {0, 0, g_w, g_h};
        if (cJSON_GetArraySize(dm) == 4) {
            r.x = (int) cJSON_GetArrayItem(dm, 0)->valuedouble;
            r.y = (int) cJSON_GetArrayItem(dm, 1)->valuedouble;
            r.w = (int) cJSON_GetArrayItem(dm, 2)->valuedouble;
            r.h = (int) cJSON_GetArrayItem(dm, 3)->valuedouble;
        }
        /* clamp to the buffer size; without an earlier frame copy everything */
        if (!g_have_frame) r = (SDL_Rect){0, 0, g_w, g_h};
        if (r.x < 0) r.w += r.x, r.x = 0;
        if (r.y < 0) r.h += r.y, r.y = 0;
        if (r.x + r.w > g_w) r.w = g_w - r.x;
        if (r.y + r.h > g_h) r.h = g_h - r.y;
        if (r.w > 0 && r.h > 0) {
            const uint8_t *src = g_map + (size_t) buf * g_bufsize + (size_t) r.y * g_stride + (size_t) r.x * 4;
            SDL_UpdateTexture(g_tex, &r, src, g_stride);
        }
        g_have_frame = 1;
    }
    char b[48];
    snprintf(b, sizeof(b), "{\"ack\":%ld}", seq);
    send_line(b);
    if (act.frame) act.frame();
}

void webr_poll(void) {
    for (;;) {
        pthread_mutex_lock(&g_lock);
        wmsg *m = g_head;
        if (m) {
            g_head = m->next;
            if (!g_head) g_tail = NULL;
        }
        pthread_mutex_unlock(&g_lock);
        if (!m) break;
        cJSON *j = cJSON_Parse(m->text);
        if (!j) {
            mt_log("[web] not JSON: %.120s", m->text);
            if (m->fd >= 0) close(m->fd);
            free(m);
            continue;
        }
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(j, "event");
        const char *e = cJSON_IsString(ev) ? ev->valuestring : "";
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "frame"))) {
            on_frame(j);
        } else if (!strcmp(e, "ready") || !strcmp(e, "buffers")) {
            on_buffers(j, m->fd);
            m->fd = -1;
            mt_log("[web] %s %dx%d", e, g_w, g_h);
            if (act.event) act.event(j);
        } else {
            if (!strcmp(e, "exit")) {
                /* helper gone (close or crash): clean up, restart it the next
                 * time */
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "eof")) && g_sock >= 0) {
                    g_alive = 0;
                    close(g_sock);
                    g_sock = -1;
                    g_ready = 0;
                    g_have_frame = 0;
                    unmap();
                    int st = 0;
                    pid_t p = waitpid(g_pid, &st, WNOHANG);
                    mt_log("[web] helper exited%s", p == g_pid ? "" : " (process still running)");
                    if (act.event) act.event(j);
                }
            } else if (act.event) {
                act.event(j);
            }
        }
        cJSON_Delete(j);
        if (m->fd >= 0) close(m->fd);
        free(m);
    }
    /* reap leftover children (after close/crash) */
    if (g_sock < 0 && g_pid > 0 && waitpid(g_pid, NULL, WNOHANG) == g_pid) g_pid = 0;
}
