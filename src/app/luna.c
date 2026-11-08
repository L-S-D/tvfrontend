/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "luna.h"

#include "log.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct LSHandle LSHandle;
typedef struct LSMessage LSMessage;
typedef bool (*LSFilterFunc)(LSHandle *sh, LSMessage *reply, void *ctx);

/* HContext as in webosbrew/webos-userland (libhelpers.h). LG's struct may be
 * longer: room for 128 bytes (like mariotaku/commons-c lunasynccall). */
typedef struct {
    LSFilterFunc callback;
    void *userdata;
    void *unknown;
    int multiple;
    int pub;
    unsigned long ret_token;
} HContext;

typedef struct {
    union {
        HContext ctx;
        unsigned char pad[128];
    } base; /* must come first: the callback gets the pointer to the context */
    luna_reply_fn cb;
    void *user;
    int sub;            /* subscription: context stays until luna_unsubscribe */
    volatile int dead;  /* subscription ended: discard further replies */
} call_ctx;

static int (*h_call)(const char *, const char *, HContext *);
static const char *(*h_message)(LSMessage *);
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void) {
    void *h = dlopen("libhelpers.so.2", RTLD_NOW);
    if (!h) {
        mt_log("[luna] libhelpers missing: %s", dlerror());
        return;
    }
    h_call = (int (*)(const char *, const char *, HContext *)) dlsym(h, "HLunaServiceCall");
    h_message = (const char *(*) (LSMessage *)) dlsym(h, "HLunaServiceMessage");
    if (!h_call || !h_message) mt_log("[luna] HLunaServiceCall missing");
}

static bool on_reply(LSHandle *sh, LSMessage *reply, void *ctx) {
    (void) sh;
    call_ctx *c = ctx;
    const char *msg = reply ? h_message(reply) : NULL;
    if (c->sub) {
        /* subscription: the context belongs to the library as long as replies can
         * arrive - never free it (one context per media session) */
        if (!c->dead && c->cb) c->cb(msg, c->user);
        return true;
    }
    if (c->cb) c->cb(msg, c->user);
    free(c); /* one-shot call (multiple = 0): nothing more arrives after this */
    return true;
}

luna_sub *luna_subscribe(const char *uri, const char *payload, luna_reply_fn cb, void *user) {
    pthread_once(&once, load);
    if (!h_call || !h_message) return NULL;
    call_ctx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->base.ctx.callback = on_reply;
    c->base.ctx.multiple = 1;
    c->base.ctx.pub = 1;
    c->cb = cb;
    c->user = user;
    c->sub = 1;
    if (h_call(uri, payload, &c->base.ctx) != 0) {
        free(c);
        return NULL;
    }
    return (luna_sub *) c;
}

void luna_unsubscribe(luna_sub *s) {
    if (s) ((call_ctx *) s)->dead = 1;
}

int luna_call(const char *uri, const char *payload, luna_reply_fn cb, void *user) {
    pthread_once(&once, load);
    if (!h_call || !h_message) return -1;
    call_ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->base.ctx.callback = on_reply;
    c->base.ctx.multiple = 0;
    c->base.ctx.pub = 1;
    c->cb = cb;
    c->user = user;
    if (h_call(uri, payload, &c->base.ctx) != 0) {
        free(c);
        return -1;
    }
    return 0;
}
