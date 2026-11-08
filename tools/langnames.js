#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Generates res/langnames/<language>.json: ISO 639-2 code (T and B) -> language name
// in the UI language. Codes from iso-codes (iso_639-2.json), names from
// CLDR (Node's Intl.DisplayNames), otherwise the English name from iso-codes.
// Usage: node tools/langnames.js [/usr/share/iso-codes/json/iso_639-2.json] [de en ...]
const fs = require('fs');
const path = require('path');
const src = process.argv[2] || '/usr/share/iso-codes/json/iso_639-2.json';
const langs = process.argv.length > 3 ? process.argv.slice(3) : ['en', 'de'];
const list = JSON.parse(fs.readFileSync(src, 'utf8'))['639-2'];
const out = path.join(__dirname, '..', 'res', 'langnames');
for (const ui of langs) {
  const dn = new Intl.DisplayNames([ui], { type: 'language', fallback: 'none' });
  const table = {};
  for (const e of list) {
    const code = e.alpha_3;
    if (/^q[a-t][a-z]$/.test(code) || code === 'qaa-qtz' || code === 'und') continue; // local codes, undetermined: done by TVF
    let name;
    try { name = dn.of(e.alpha_2 || code); } catch (_) { name = undefined; }
    if (!name || name === code) name = ui === 'en' ? e.name : undefined;
    if (!name) { try { name = new Intl.DisplayNames(['en'], { type: 'language', fallback: 'none' }).of(e.alpha_2 || code); } catch (_) {} }
    if (!name) name = e.name;
    // capitalize first letter (CLDR writes some languages in lower case)
    name = name.charAt(0).toLocaleUpperCase(ui) + name.slice(1);
    table[code] = name;
    if (e.bibliographic) table[e.bibliographic] = name;
  }
  const keys = Object.keys(table).sort();
  const sorted = {};
  for (const k of keys) sorted[k] = table[k];
  fs.writeFileSync(path.join(out, ui + '.json'), JSON.stringify(sorted, null, 0).replace(/","/g, '",\n"').replace(/^\{/, '{\n').replace(/\}$/, '\n}\n'));
  console.log(ui + ': ' + keys.length + ' codes');
}
