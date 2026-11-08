/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fetch.h"

#include "http.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKERS 3
#define MAX_BODY (8 * 1024 * 1024)

typedef struct job {
    struct job *next;
    char url[1024];
    fetch_cb cb;
    void *user;
    int status;
    char *body;
    size_t len;
} job;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static job *g_queue, *g_queue_tail, *g_done, *g_done_tail;
static int g_workers;
static void (*g_wake)(void);
static fetch_local_fn g_local;

static void done_push(job *j);

static void run(job *j) {
    http_conn c;
    int rc = http_open_quiet(j->url, NULL, NULL, &c);
    j->status = c.status;
    if (rc != 0) return;
    size_t cap = 0;
    for (;;) {
        if (j->len + 4096 + 1 > cap) {
            size_t nc = cap ? cap * 2 : 16 * 1024;
            if (nc > MAX_BODY) break;
            char *n = realloc(j->body, nc);
            if (!n) break;
            j->body = n;
            cap = nc;
        }
        ssize_t r = http_read(&c, j->body + j->len, cap - j->len - 1);
        if (r <= 0) break;
        j->len += (size_t) r;
    }
    if (j->body) j->body[j->len] = 0;
    http_close(&c);
}

static void *worker(void *arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_queue) pthread_cond_wait(&g_cond, &g_lock);
        job *j = g_queue;
        g_queue = j->next;
        if (!g_queue) g_queue_tail = NULL;
        pthread_mutex_unlock(&g_lock);

        run(j);
        done_push(j);
    }
    return NULL;
}

void fetch_init(void (*wake)(void)) { g_wake = wake; }

void fetch_set_local(fetch_local_fn fn) { g_local = fn; }

static void done_push(job *j) {
    pthread_mutex_lock(&g_lock);
    j->next = NULL;
    if (g_done_tail) g_done_tail->next = j;
    else g_done = j;
    g_done_tail = j;
    pthread_mutex_unlock(&g_lock);
    if (g_wake) g_wake();
}

void fetch_get(const char *url, fetch_cb cb, void *user) {
    job *j = calloc(1, sizeof(*j));
    if (!j) return;
    snprintf(j->url, sizeof(j->url), "%s", url);
    j->cb = cb;
    j->user = user;
    if (!strncmp(url, "local:", 6)) {
        j->status = g_local ? g_local(url + 6, &j->body, &j->len) : 404;
        done_push(j);
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (g_queue_tail) g_queue_tail->next = j;
    else g_queue = j;
    g_queue_tail = j;
    pthread_cond_signal(&g_cond);
    int start = g_workers < WORKERS;
    if (start) g_workers++;
    pthread_mutex_unlock(&g_lock);
    if (start) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
    }
}

void fetch_poll(void) {
    pthread_mutex_lock(&g_lock);
    job *list = g_done;
    g_done = g_done_tail = NULL;
    pthread_mutex_unlock(&g_lock);
    while (list) {
        job *j = list;
        list = j->next;
        j->cb(j->user, j->status, j->body, j->len);
        free(j->body);
        free(j);
    }
}
