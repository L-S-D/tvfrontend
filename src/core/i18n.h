/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * User interface texts. The source language is English: every visible text
 * is written as _("English text") and translated at run time from
 * res/lang/<language>.json, a flat JSON object {"English text": "translation"}.
 * A missing file or entry keeps the English text, so a new language is just
 * another JSON file (tools/i18n_check.py lists missing and stale entries and
 * checks that printf conversions match).
 *
 * The table is read-only after i18n_init(): tr() may be called from any thread.
 */
#pragma once

/* lang: "de", "en", "fr" ... (only the part before '-' or '_' counts) */
void i18n_init(const char *res_dir, const char *lang);

/* languages there are: "en" and one per res/lang/<code>.json, sorted;
 * returns the count (at most max), codes stay valid */
int i18n_languages(const char **codes, int max);

/* name of a language in that language ("Deutsch"), the code if unknown */
const char *i18n_language_name(const char *code);

/* name of an ISO 639-2 language (T or B code, "deu"/"ger") in the user
 * interface language, from res/langnames/<language>.json (tools/langnames.js);
 * NULL if unknown. Local codes (qaa-qtz) and "und" are not in the table. */
const char *i18n_iso639(const char *code);

/* active language ("en" when no file was loaded) */
const char *i18n_lang(void);

/* decimal separator of the language ('.' for English, ',' otherwise) */
char i18n_decimal(void);

/* translation of msgid, or msgid itself */
const char *tr(const char *msgid);

#define _(s) tr(s)
/* marks a text for the checker where it is only looked up later (tables) */
#define N_(s) (s)
