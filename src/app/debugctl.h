/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Development aid (only with launch parameter "debug":"<udp-port>"): accepts
 * text commands via UDP and passes them on to the main thread.
 *   KEY <name>        key (names as in key_name: UP, OK, YELLOW, CH+, 5 ...)
 *   SHOT <ip:port>    send the UI layer (without video) as PNG via TCP
 *   WHEEL <1|-1>      Magic Remote wheel (1 = up/away from the user)
 *   PTR <MOVE|DOWN|UP> <x> <y>   Magic Remote pointer
 * Example:  echo "KEY YELLOW" | socat - UDP:<tv>:5556
 */
#pragma once

#include <stdint.h>

extern uint32_t debugctl_sdl_event;

void debugctl_start(int port);

/* next command (release with free()) or NULL; main thread */
char *debugctl_pop(void);
