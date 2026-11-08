/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_sdl.h"

#include "log.h"
#include "lodepng.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

uint32_t ui_sdl_event = (uint32_t) -1;

static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static uint8_t *g_fb;
static int g_w, g_h;
static lv_display_t *g_disp;
static SDL_Texture *g_layer; /* layer below LVGL (HbbTV page), NULL = none */

void ui_compose(void) {
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
    SDL_RenderClear(g_ren);
    if (g_layer) {
        /* premultiplied: copy (alpha too), blend LVGL on top */
        SDL_RenderCopy(g_ren, g_layer, NULL, NULL);
        SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_BLEND);
    } else {
        SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE); /* take over alpha */
    }
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

void ui_set_layer(SDL_Texture *t) {
    g_layer = t;
    ui_compose();
}

SDL_Renderer *ui_renderer(void) { return g_ren; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    (void) px; /* DIRECT: px is the whole buffer */
    SDL_Rect r = {a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1};
    SDL_UpdateTexture(g_tex, &r, g_fb + ((size_t) a->y1 * g_w + a->x1) * 4, g_w * 4);
    if (lv_display_flush_is_last(d)) ui_compose();
    lv_display_flush_ready(d);
}

static uint32_t tick_cb(void) { return SDL_GetTicks(); }

static void log_cb(lv_log_level_t level, const char *buf) {
    (void) level;
    mt_log("[lvgl] %s", buf);
}

static void wake(void) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = ui_sdl_event;
    SDL_PushEvent(&e);
}

int ui_init(SDL_Renderer *ren, int w, int h, const char *res_dir) {
    g_ren = ren;
    g_w = w;
    g_h = h;
    if (ui_sdl_event == (uint32_t) -1) ui_sdl_event = SDL_RegisterEvents(1);

    g_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_tex) {
        mt_log("[ui] texture: %s", SDL_GetError());
        return -1;
    }
    SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE); /* take over alpha, do not blend */
    g_fb = calloc((size_t) w * h, 4);
    if (!g_fb) return -1;

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_log_register_print_cb(log_cb);
    g_disp = lv_display_create(w, h);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(g_disp, g_fb, NULL, (uint32_t) ((size_t) w * h * 4), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(g_disp, flush_cb);

    /* screen transparent: the video is here */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_res_init(res_dir, wake);
    mt_log("[ui] LVGL %d.%d.%d, %dx%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, w, h);

    /* show transparent once, before the first frame comes */
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);
    return 0;
}

uint32_t ui_frame(void) { return lv_timer_handler(); }

int ui_screenshot_send(const char *hostport) {
    char host[64];
    const char *colon = strrchr(hostport, ':');
    if (!colon || (size_t) (colon - hostport) >= sizeof(host)) return -1;
    memcpy(host, hostport, (size_t) (colon - hostport));
    host[colon - hostport] = 0;
    lv_refr_now(NULL);
    size_t npx = (size_t) g_w * g_h;
    uint8_t *rgba = malloc(npx * 4);
    if (!rgba) return -1;
    int done = 0;
    if (g_layer) {
        /* with a layer (HbbTV): read the composed image from the window */
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
        SDL_RenderClear(g_ren);
        SDL_RenderCopy(g_ren, g_layer, NULL, NULL);
        SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
        done = SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ABGR8888, rgba, g_w * 4) == 0;
        SDL_RenderPresent(g_ren);
    }
    for (size_t i = 0; !done && i < npx; i++) { /* ARGB8888 in memory: B,G,R,A */
        rgba[i * 4 + 0] = g_fb[i * 4 + 2];
        rgba[i * 4 + 1] = g_fb[i * 4 + 1];
        rgba[i * 4 + 2] = g_fb[i * 4 + 0];
        rgba[i * 4 + 3] = g_fb[i * 4 + 3];
    }
    unsigned char *png = NULL;
    size_t len = 0;
    unsigned err = lodepng_encode32(&png, &len, rgba, (unsigned) g_w, (unsigned) g_h);
    free(rgba);
    if (err) return -1;
    int rc = -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) atoi(colon + 1));
    if (fd >= 0 && inet_pton(AF_INET, host, &a.sin_addr) == 1 && connect(fd, (struct sockaddr *) &a, sizeof(a)) == 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t n = send(fd, png + off, len - off, MSG_NOSIGNAL);
            if (n <= 0) break;
            off += (size_t) n;
        }
        rc = off == len ? 0 : -1;
    }
    if (fd >= 0) close(fd);
    lv_free(png);
    mt_log("[ui] screenshot %zu bytes to %s: %s", len, hostport, rc == 0 ? "ok" : "ERROR");
    return rc;
}

void ui_shutdown(void) {
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = NULL;
}
