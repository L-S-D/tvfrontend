/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "debugctl.h"

#include "log.h"

#include <SDL.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

uint32_t debugctl_sdl_event = (uint32_t) -1;

typedef struct cmd {
    struct cmd *next;
    char text[];
} cmd;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static cmd *g_head, *g_tail;
static int g_fd = -1;

static void *rx_thread(void *arg) {
    (void) arg;
    char buf[8192]; /* WEBJS: longer scripts */
    for (;;) {
        ssize_t n = recv(g_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
        buf[n] = 0;
        cmd *c = malloc(sizeof(cmd) + (size_t) n + 1);
        if (!c) continue;
        c->next = NULL;
        memcpy(c->text, buf, (size_t) n + 1);
        pthread_mutex_lock(&g_lock);
        if (g_tail) g_tail->next = c;
        else g_head = c;
        g_tail = c;
        pthread_mutex_unlock(&g_lock);
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        e.type = debugctl_sdl_event;
        SDL_PushEvent(&e);
    }
    return NULL;
}

void debugctl_start(int port) {
    if (port <= 0) return;
    debugctl_sdl_event = SDL_RegisterEvents(1);
    g_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (g_fd < 0 || bind(g_fd, (struct sockaddr *) &a, sizeof(a)) != 0) {
        mt_log("[debug] UDP port %d not available", port);
        if (g_fd >= 0) close(g_fd);
        g_fd = -1;
        return;
    }
    pthread_t t;
    pthread_create(&t, NULL, rx_thread, NULL);
    pthread_detach(t);
    mt_log("[debug] control on UDP port %d", port);
}

char *debugctl_pop(void) {
    pthread_mutex_lock(&g_lock);
    cmd *c = g_head;
    if (c) {
        g_head = c->next;
        if (!g_head) g_tail = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    if (!c) return NULL;
    char *s = strdup(c->text);
    free(c);
    return s;
}
