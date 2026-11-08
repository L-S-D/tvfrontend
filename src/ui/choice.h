/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Selection list in the middle of the screen (like "choice" in the HTML5 app): title, options,
 * the current one with a check mark. Up/Down selects (wrapping around), OK executes, Back
 * closes. Main thread only.
 */
#pragma once

typedef void (*choice_cb)(void *user, int index);

#define CHOICE_MAX 32

void choice_init(void);

/* labels are copied; checked = index with check mark (-1 = none), sel =
 * initially selected. cb is called after closing with the index. */
void choice_open(const char *title, const char *const *labels, int n, int checked, int sel, choice_cb cb, void *user);

/* like choice_open, check mark on all rows with checked[i] != 0 (NULL = none) */
void choice_open_multi(const char *title, const char *const *labels, int n, const int *checked, int sel, choice_cb cb,
                       void *user);
void choice_close(void);
int choice_visible(void);

/* app_key; 1 = consumed (open: all keys) */
int choice_key(int key);

/* pointer (ptr_event); open: consumes everything */
int choice_pointer(int ev, int x, int y);
