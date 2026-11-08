# Third-party notices

TVF's own code is licensed under the GNU GPL version 3 or later (`LICENSE`). The package
(`com.lsd.tvf_<version>_arm.ipk`) also contains or links the following third-party
software. `package.sh` copies this file and the licence texts into the package
(`licenses/`, fonts and HbbTV scripts next to their files under `res/`).

| Component | Version | Licence | Where | Licence text in the package |
|---|---|---|---|---|
| [OpenSSL](https://www.openssl.org) | 3.4.1 (SDK, linked statically) | Apache-2.0 | HTTPS (`src/net/http.c`) | `licenses/Apache-2.0.txt` |
| [LVGL](https://lvgl.io) | 9.5.0 (git submodule) | MIT | user interface | `licenses/LVGL.txt` |
| LodePNG (in LVGL) | LVGL 9.5.0 | Zlib | PNG decoding | `licenses/lodepng.txt` |
| TJpgDec (in LVGL) | R0.03 | ChaN's free licence | JPEG decoding | `licenses/tjpgd.txt` |
| TinyTTF / stb_truetype (in LVGL) | LVGL 9.5.0 | MIT or public domain | TrueType rendering (Lato, teletext font) | `licenses/tiny_ttf.txt` |
| Montserrat font (in LVGL, sizes 14 and 22) | LVGL 9.5.0 | OFL-1.1 | built-in symbols font | `licenses/Montserrat-OFL.txt` |
| Font Awesome 5 Free symbols (in LVGL's Montserrat fonts) | LVGL 9.5.0 | OFL-1.1 (fonts), CC BY 4.0 (icons) | symbols | `licenses/FontAwesome5.txt` |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | MIT | JSON | `licenses/cJSON.txt` |
| [stb_image, stb_image_write](https://github.com/nothings/stb) | 2.30, 1.16 | MIT or public domain | channel logo trimming | `licenses/stb.txt` |
| [minimp3](https://github.com/lieff/minimp3) | - | CC0-1.0 | MPEG audio to PCM | `licenses/minimp3.txt` |
| Chromium CDM interface headers (via Kodi inputstream.adaptive) | - | BSD-3-Clause | DRM (`src/app/cdm.cpp`) | `licenses/cdm.txt` |
| GCC runtime (libstdc++, libgcc) | SDK GCC, linked statically | GPL-3.0 with the GCC Runtime Library Exception | C++ runtime of `cdm.cpp` | - (the exception allows distribution without further conditions) |
| [Lato](https://www.latofonts.com) | 2.0 (Debian) | OFL-1.1 | user interface font | `res/fonts/LICENSE-Lato.txt` |
| [Bedstead](https://bjh21.me.uk/bedstead/) | - | CC0-1.0 | teletext font | `res/fonts/README-bedstead.txt` |
| [dash.js](https://github.com/Dash-Industry-Forum/dash.js) | 4.7.4 | BSD-3-Clause | MPEG-DASH in HbbTV apps | `res/hbbtv/LICENSE-dashjs.txt` |
| CA certificates (Mozilla CA list via Ubuntu's `ca-certificates`) | as of 2026-11-01 | MPL-2.0 | HTTPS trust store (`res/ca-certificates.crt`) | `licenses/MPL-2.0.txt` |

SDL2 and the Starfish media libraries are the TV's own and are not distributed with TVF.

Before a release: check OpenSSL 3.4.1 for new advisories (it is linked statically and
only updated with the SDK) and refresh `res/ca-certificates.crt` (e.g. from
`/etc/ssl/certs/ca-certificates.crt` of a current Debian or Ubuntu).
