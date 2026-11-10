/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HTSP client (TVHeadend, port 9982): a single connection per TV for
 * metadata and live subscriptions (like Kodi's pvr.hts). A background thread maintains
 * it: hello, authentication (digest = SHA1(password + challenge)),
 * enableAsyncMetadata with an EPG window; after a disconnect it reconnects
 * by itself. It dispatches what arrives:
 * - messages of a subscription (subscriptionId: muxpkt, subscriptionStart,
 *   signalStatus ...) and replies to its commands go raw into the
 *   subscription's queue (htsp_sub_read, player thread); when that is full,
 *   it waits (back pressure on tvh);
 * - everything else as cJSON into a queue for the main thread:
 *   asynchronous messages ("method": channelAdd, eventUpdate, dvrEntryAdd,
 *   initialSyncCompleted, ...), replies to htsp_request (with "seq"), plus
 *   {"method":"_sync"} before every (re)sync, {"method":"_down"} after a
 *   disconnect and {"method":"_noaccess"} when tvh refuses the user (login
 *   rejected, or no rights - not the connection limit); then it tries again
 *   after 60 s.
 */
#pragma once

#include "cJSON.h"
#include "htsmsg.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char host[64];
    int port;        /* 9982 */
    char user[64];   /* "" = anonymous (only where tvh allows it) */
    char pass[64];
    long epg_window; /* s: events this far ahead arrive asynchronously */
} htsp_cfg;

void htsp_start(const htsp_cfg *c);

/* main thread: next message (then owned by the caller) or NULL */
cJSON *htsp_next(void);

/* request (method + flat fields from args: number, text, true/false);
 * returns the seq of the reply, -1 = no connection */
int htsp_request(const char *method, const cJSON *args);

int htsp_connected(void);

/* ---- Subscriptions (player) ---- */
typedef struct htsp_sub htsp_sub;

/* subscribe to a channel (90 kHz, optional timeshift in s and profile); *id = the
 * subscriptionId. NULL = not connected */
htsp_sub *htsp_sub_open(uint32_t channel, int timeshift_s, const char *profile, int *id);
/* queueDepth (bytes) asked of tvh for each subscription; 0 = tvh's default (for tests) */
void htsp_set_queue_depth(int bytes);
/* next message of the subscription (body without length field, malloc) or NULL:
 * timed out (timeout_ms, < 0 = no timeout), or *gone = connection lost or
 * htsp_sub_cancel. Only the owner of the subscription */
unsigned char *htsp_sub_read(htsp_sub *s, size_t *len, int timeout_ms, int *gone);
/* wake a waiting htsp_sub_read (any thread) */
void htsp_sub_cancel(int id);
/* command to the subscription (htsp sets subscriptionId and seq, the reply is
 * discarded); 0 = sent */
int htsp_sub_cmd(int id, hmsg *m);
/* unsubscribe (if the connection is still the same one), free the queue */
void htsp_sub_close(htsp_sub *s);
