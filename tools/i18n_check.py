#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the UI translations against the sources.

Collects every _("...") and N_("...") text from src/ (adjacent string
literals joined, C escapes decoded) and compares each res/lang/*.json:
missing entries (stay English), stale entries (text no longer in the
sources) and printf conversions that differ between text and translation
(that would crash or garble). Exit code 1 only for conversion mismatches
or broken JSON.

    tools/i18n_check.py [--missing]    # --missing: list the missing texts
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CALL = re.compile(r'\bN?_\(\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)\)')
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CONV = re.compile(r'%(?:%|[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcspn])')


def decode(raw):
    b = raw.encode('latin-1', 'backslashreplace').decode('unicode_escape').encode('latin-1')
    return b.decode('utf-8')


TOKEN = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|/\*.*?\*/|//[^\n]*', re.S)


def strip_comments(src):
    # comments become blanks (line numbers stay), string literals stay
    return TOKEN.sub(lambda m: m.group(0) if m.group(0)[0] in '"\'' else re.sub(r'[^\n]', ' ', m.group(0)), src)


def texts():
    out = {}
    for dp, _, fn in os.walk(os.path.join(ROOT, 'src')):
        for f in fn:
            if not f.endswith(('.c', '.cpp', '.h')):
                continue
            p = os.path.join(dp, f)
            src = strip_comments(open(p, encoding='utf-8', errors='replace').read())
            for m in CALL.finditer(src):
                t = decode(''.join(LIT.findall(m.group(1))))
                out.setdefault(t, '%s:%d' % (os.path.relpath(p, ROOT), src.count('\n', 0, m.start()) + 1))
    return out


def convs(s):
    return [c for c in CONV.findall(s) if c != '%%']


def main():
    show_missing = '--missing' in sys.argv
    src = texts()
    bad = 0
    lang_dir = os.path.join(ROOT, 'res', 'lang')
    print('%d texts in the sources' % len(src))
    for f in sorted(os.listdir(lang_dir)) if os.path.isdir(lang_dir) else []:
        if not f.endswith('.json'):
            continue
        try:
            tr = json.load(open(os.path.join(lang_dir, f), encoding='utf-8'))
        except ValueError as e:
            print('%s: broken JSON: %s' % (f, e))
            bad += 1
            continue
        missing = [t for t in src if t not in tr]
        stale = [t for t in tr if t not in src]
        print('%s: %d translated, %d missing, %d stale' % (f, len(src) - len(missing), len(missing), len(stale)))
        for t, v in tr.items():
            if t in src and convs(t) != convs(v):
                print('  CONVERSIONS DIFFER (%s): %r -> %r' % (src[t], t, v))
                bad += 1
        if show_missing:
            for t in missing:
                print('  missing (%s): %r' % (src[t], t))
        for t in stale:
            print('  stale: %r' % t)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
