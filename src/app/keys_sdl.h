/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SDL key event -> app_key (see ui/keys.h) */
#pragma once

#include "keys.h"

#include <SDL.h>

int key_from_sdl(const SDL_KeyboardEvent *k);
