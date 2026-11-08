/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Client for tvf-web: WPE WebKit in a process of its own (bundle from the
 * wpe-webos project, protocol there in PROTOCOL.md). The helper renders
 * the page into shared memory (BGRA, premultiplied alpha, two
 * buffers); here the changed rectangles end up in an SDL texture that the
 * UI draws beneath LVGL (ui_set_layer). If WebKit crashes, TVF keeps
 * running.
 *
 * Control: JSON lines over a socketpair; a reader thread accepts the
 * messages (and the buffers' memfd) and wakes the main thread, everything
 * else runs there (webr_poll).
 */
#pragma once

#include "cJSON.h"

#include <SDL.h>

typedef struct {
    /* Helper event (loaded, loadFailed, tlsError, msg, console,
     * evalResult, crashed, exit; "exit" also when it is simply gone) */
    void (*event)(const cJSON *ev);
    /* new image in the texture */
    void (*frame)(void);
} webr_actions;

/* wake: wakes the main thread (SDL event) */
void webr_init(const webr_actions *a, void (*wake)(void));

/* Start the helper if it is not running: bundle = directory with run.sh,
 * data_dir writable and persistent (cookies, cache, font cache).
 * 0 = running or started */
int webr_start(const char *bundle, const char *data_dir, int w, int h);

/* WebKit plays media itself (bundle with "media"): subsequent starts with
 * --media and TVF_MEDIA_SOCK = sock (media server webmedia.c); NULL/"" = off */
void webr_set_media(const char *sock);

/* running and has reported its buffers (ready) */
int webr_ready(void);
int webr_running(void);

/* Stop (close, then close the socket) */
void webr_stop(void);

/* Commands (only while it is running; discarded otherwise) */
void webr_load(const char *url, const char *user_agent, const char *proxy);
void webr_key(int dom_code); /* pressed and released */
/* Pointer in page coordinates: type "move", "down", "up", "leave" */
void webr_pointer(const char *type, double x, double y);
void webr_scroll(int dir, double x, double y); /* wheel step, dir > 0 = up */
void webr_user_script(const char *js);
void webr_clear_user_scripts(void);
void webr_eval(const char *js); /* in the main frame, the result is discarded */
void webr_visible(int on);

/* Texture with the most recently reported image (size as at start), NULL before
 * the first image */
SDL_Texture *webr_texture(void);

/* Process messages (main loop) */
void webr_poll(void);
