/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * SDL key events -> app_key. The webOS-specific keys (CH+/-, Back, colour
 * keys) have their own scancodes in webOS SDL (SDL_webOS.h); the rest come
 * through the normal SDL codes. Unknown keys return KEY_NONE and are logged
 * by the caller with scancode/keycode.
 */
#include "keys_sdl.h"

#include <SDL_webOS.h>

int key_from_sdl(const SDL_KeyboardEvent *k) {
    int sc = (int) k->keysym.scancode;
    SDL_Keycode kc = k->keysym.sym;

    switch (sc) {
    case SDL_WEBOS_SCANCODE_CH_UP: return KEY_CH_UP;
    case SDL_WEBOS_SCANCODE_CH_DOWN: return KEY_CH_DOWN;
    case SDL_WEBOS_SCANCODE_BACK: return KEY_BACK;
    case SDL_WEBOS_SCANCODE_EXIT: return KEY_EXIT;
    case SDL_WEBOS_SCANCODE_RED: return KEY_RED;
    case SDL_WEBOS_SCANCODE_GREEN: return KEY_GREEN;
    case SDL_WEBOS_SCANCODE_YELLOW: return KEY_YELLOW;
    case SDL_WEBOS_SCANCODE_BLUE: return KEY_BLUE;
    case SDL_SCANCODE_AUDIOPLAY: return KEY_PLAY;
    case SDL_SCANCODE_AUDIOSTOP: return KEY_STOP;
    case SDL_SCANCODE_AUDIOREWIND: return KEY_REWIND;
    case SDL_SCANCODE_AUDIOFASTFORWARD: return KEY_FFWD;
    case SDL_SCANCODE_PAUSE: return KEY_PAUSE;
    case SDL_SCANCODE_AC_BACK: return KEY_BACK;
    default: break;
    }
    if (kc >= SDLK_0 && kc <= SDLK_9) return KEY_DIGIT0 + (int) (kc - SDLK_0);
    if (kc >= SDLK_KP_1 && kc <= SDLK_KP_9) return KEY_DIGIT0 + 1 + (int) (kc - SDLK_KP_1);
    if (kc == SDLK_KP_0) return KEY_DIGIT0;
    switch (kc) {
    case SDLK_UP: return KEY_UP;
    case SDLK_DOWN: return KEY_DOWN;
    case SDLK_LEFT: return KEY_LEFT;
    case SDLK_RIGHT: return KEY_RIGHT;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return KEY_OK;
    case SDLK_ESCAPE: return KEY_BACK;
    case SDLK_PAGEUP: return KEY_CH_UP;
    case SDLK_PAGEDOWN: return KEY_CH_DOWN;
    case SDLK_AUDIOPLAY: return KEY_PLAY;
    case SDLK_AUDIOSTOP: return KEY_STOP;
    default: return KEY_NONE;
    }
}
