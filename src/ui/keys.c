/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "keys.h"

const char *key_name(int key) {
    static const char *names[] = {"-",   "UP",    "DOWN",   "LEFT", "RIGHT", "OK",  "BACK",       "EXIT",
                                  "RED", "GREEN", "YELLOW", "BLUE", "CH+",   "CH-", "INFO",       "REC",
                                  "PLAY", "PAUSE", "PLAY/PAUSE", "STOP", "REW", "FFWD"};
    static const char *digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    if (KEY_IS_DIGIT(key)) return digits[key - KEY_DIGIT0];
    if (key >= 0 && key < (int) (sizeof(names) / sizeof(names[0]))) return names[key];
    return "?";
}
