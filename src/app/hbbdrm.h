/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * PlayReady via LG's DRM service (luna://com.webos.service.drm), like an
 * OIPF DRM agent: one client per app (load), messages to it
 * (sendDrmMessage: PlayReady initiator - licence URL override, CustomData
 * or pre-delivered licence), rights errors via subscription (getRightsError).
 * The TV's DRM client fetches the licence itself; the media load refers to it
 * with option.drm = {type:"playready", clientId} (hbbmedia.c).
 * See scratchpad/drm-playready.md (design note). Main thread only.
 */
#pragma once

#define HBBDRM_PLAYREADY "urn:dvb:casystemid:19219"
#define HBBDRM_INITIATOR "application/vnd.ms-playready.initiator+xml"

/* Result of a message: code as in OIPF (0 ok, 1 unknown error,
 * 2 cannot be processed, 3 unknown MIME, 4 user consent needed, 5 unknown DRM
 * system, 6 wrong format) */
typedef void (*hbbdrm_result_fn)(void *user, int code, const char *result_msg);

void hbbdrm_init(void (*wake)(void));

/* Load the client (if there is none yet), rights subscription; asynchronous. 0 = running/present */
int hbbdrm_load(void);
/* clientId or NULL (not loaded yet) */
const char *hbbdrm_client(void);

/* Message to the client (loads it if needed); cb is called exactly once */
void hbbdrm_send(const char *msg_type, const char *msg, const char *system_id, hbbdrm_result_fn cb, void *user);

/* Reset licence URL override and CustomData (otherwise they persist for the
 * app) */
void hbbdrm_reset(void);

/* Licence requests (debug command DRMPROXY 0|1|2, default 1): 1 = LA_URL
 * of subsequent initiators points to the relay (relay_license_url),
 * which forwards according to the egress rules, one log line per licence;
 * 2 = additionally the full request and response in the log; 0 = direct */
void hbbdrm_proxy(int mode);

/* Release the client (end of session/app) */
void hbbdrm_unload(void);

/* Rights error from the subscription: state 0 no licence, 1 invalid, 2 valid */
void hbbdrm_on_rights(void (*fn)(int state, const char *content_id));

void hbbdrm_poll(void);
