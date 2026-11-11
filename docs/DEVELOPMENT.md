# TVF – developer notes

Technical notes for people who build, test or change TVF. The user-facing overview,
installation and usage are in the [README](../README.md).

## Build options

- `local.env` next to `build.sh` (not in the repository): `TVF_DEFAULT_TVH_HOST=<address>` makes
  that server the default until one is entered in the settings. Also `cmake -DTVF_DEFAULT_TVH_HOST=…`.
- `cmake -DTVF_PMT_FALLBACK=OFF`: leave out the PMT lookup for a tvh without the HTSP extensions
  (see the teletext entry under "Features in detail"); default ON.
- `res/hbbtv/egress.local.json`: own HbbTV egress rules (which channels or hosts go through which
  proxy; format in `res/hbbtv/egress.example.json`). `package.sh` packs it instead of
  `res/hbbtv/egress.json`, which sends everything directly.

## Features in detail

* Live TV as an HTSP subscription to tvh (`tvsrc.c`, IPTV channels too), zapping with CH+/CH−, digits (on release), 0 = previous channel. One HTSP connection per TV carries the metadata and the live subscription, like Kodi's pvr.hts (`src/net/htsp.c` routes the subscription's messages to the player)
* Channel banner: number, name, logo, now/next with progress, clock, video/audio track info
* Channel list/EPG: now/next and 3-hour grid, all groups, group selection
* Audio track selection (remembered per channel) and subtitles (teletext 777, DVB) via yellow
* Recording dialog (REC, Info/REC in the EPG): record/cancel, series
* Recordings (green): folders, scheduled, resume; recordings, timers and resume
  positions directly from tvh via HTSP (`tvrec.c`, positions as tvh `playposition`;
  if tvh refuses the write, they are kept on the TV); playback directly from `/dvrfile`,
  seeking via PCR samples of the file (`recindex.c`, time → byte)
* Teletext (blue), drawn with Bedstead: from an own page memory of the HTSP subscription
  (`sidedata.c`: `ttxdec.c`, `dvbsubdec.c` – C ports of the dvb-buffer decoders), filled
  with whatever tvh delivers (live or from the timeshift buffer). Teletext subtitle pages offered: those of the
  channel's PMT (teletext descriptor type 2/5). A plain tvh (without the HTSP extensions) sends neither the PMT
  nor the page of its TEXTSUB stream, nor component tags, the subtitling type of DVB subtitles, the
  initial teletext page or the audio description/receiver-mix flags of AC-3/AAC: TVF then opens the
  channel once briefly as HTTP stream with the pass profile, reads PAT/PMT with its stream demuxer
  (`pmtinfo.c`, merged into the stream list by `player_merge_pmt()`; same service, no second
  tuner). A tvh with the extensions needs no such lookup (TVF notices from the stream list).
  A shared teletext (ORF) carries the subtitle bit on other programmes' pages too; without PMT
  pages all pages with the bit are offered as before. Build option `TVF_PMT_FALLBACK` (CMake,
  default ON) – OFF leaves the lookup out
* Audio: AC-3/E-AC-3/AAC directly, MP2 via minimp3 as PCM
* First picture after switching ~0.4–0.5 s (free-to-air), ~0.65–0.85 s (HD+);
  recording start/seek ~0.45 s

* Timeshift: tvh's own timeshift on the same HTSP subscription (pause/±30 s/live as
  subscriptionSpeed/Skip/Live; needs timeshift enabled in tvh, TVF asks for up to 2 h);
  IPTV channels have none. The launch parameter `"timeshift":0` disables it. The
  "−m:ss behind live" display stays visible while paused; during time-shifted
  playback it disappears 5 s after the last input (←/→/OK show it again).
* Magic Remote: pointer (pointing selects, click activates, progress bar in
  recordings with target time/dragging), wheel (lists, teletext pages, ±30 s, live channel ±1)
* Subtitle offset and sound offset (yellow menu; left/right in 25 ms steps, the wheel too; per
  channel, a channel without its own value takes the last one set; the sound offset shifts the
  time stamps of the audio fed, + = sound later, up to ±2 s, stored when adjusting ends),
  banner with synopsis and scrolling title
* Programme info window (long OK press in the live picture, from 0.6 s, with the
  Magic Remote pointer hidden): the banner
  enlarged upwards with title, subtitle, additional details (genre, episode, year,
  age rating, duration), the full long description and the image from the tvh EPG,
  if there is one; below the logo the reception as in the banner (provider, SNR and signal
  bars in the colour of the quality, a quality line with status, BER and UNC, the
  decryption) and the DVB ids (ONID · TSID · SID), updated while it is open;
  right/left switch between current ↔ next programme, up/down
  scrolls, REC records the programme shown, Back/OK/yellow close it. A short OK
  press keeps its old meaning but acts on release. TVF decodes JPEGs itself (stb, including
  progressive – LVGL's TJPGD only handles baseline)
* Reception info in the banner below the logo (tvh via HTSP): provider, SNR and signal
  as bars with value (colour = quality according to tvh: green good, yellow weak, red
  bad/no signal), descrambling as a box (card system and CAID), below that ECM
  time and reader or source. The source is tvh itself: the status messages of the
  live subscription (`sourceinfo`, `signalStatus`, `descrambleInfo`)
* A live stream that drops or fails to start (tvh restart, network, channel without
  reception) is reconnected, with a notice at the top ("is not responding – retrying
  in 4 s"); switching channels cancels a hanging request immediately. Dropped means:
  no message in the running subscription for 10 s (IPTV: no data for 8 s over
  HTTP), or no new picture for 10 s although data is arriving (picture watchdog; not
  for radio and pause) – then "picture frozen". Retries after 1, 2, 4, 8, then every 10 s
* HbbTV preload: after 5 s on a channel TVF loads its HbbTV app invisibly (also again 5 s
  after closing it); all keys stay with TVF, red then shows the app and passes the red
  key on at once (RTL/ORF react within ~0.1–0.5 s instead of 2–2.5 s). Zapping through
  loads nothing; a channel change discards the preloaded app.
* HbbTV (red in the live picture) only in TVF's own renderer (WPE WebKit from the
  package `com.lsd.tvf.web`, see below). The former overlay (HTML5 app
  `com.lsd.tvf.hbbtv` placed on top of TVF) is no longer supported.

## Launch parameters

Parameters (`ares-launch -p '{"key":value,…}'`): `log` (UDP target),
`debug` (UDP port for test commands, see below),
`lang` (UI language such as `de` or `en`, `auto` = TV menu language,
see "Languages"), `tvhUser`/`tvhPass`, `hbbtvEgress` (`rules`, `direct` or a proxy URL),
`webBundle` (path of a
WebKit bundle with `run.sh`, e.g. for testing a new build; `auto` = the
package `com.lsd.tvf.web`, or without the package the old location
`/media/developer/temp/tvf-web-sdk/tvf-web-sdk-armhf`; both are remembered,
a remembered path without `run.sh` falls back to `auto`) – these setting
parameters are remembered like the settings page,
`dvrReadOnly` (1 = for tests: timers and resume positions are not written to tvh, only logged),
`tvhTest` (for tests: `{"host":…,"user":…,"pass":…}` – tvh server and login of this run only,
nothing is saved; a member that is left out keeps the stored value, `"host":""` = no server,
then TVF searches the network as at the first start and shows the result instead of taking it),
`stats` (ms, player statistics), player tuning knobs: `ctype`, `margin`,
`vmax`/`amax`, `mpa` (`pcm` or a Starfish name such as `MPEG`), `alang`, `apid`,
`maxfps`, `fps`, `keymode`, `dump`/`dumpmb` (raw data capture via TCP).
Only for experiments on webOS 4: `alpha` (0 = window surface as SDL sets it up),
`acbtype` (ACB player type, default 10 = MSE), `acbwin` (0 = full-screen window,
1 = video size → full screen, 2 = none), `acbvdata` (0 = no video data to ACB).


## Keys

Two key layouts can be chosen in the settings ("Key layout"); the table shows
"Standard", the differences of "HbbTV first" follow below it.

| Key | Live | Channel list | Recording playback |
|---|---|---|---|
| CH+ / CH− | Channel ± | Group ± | Live TV, channel ± |
| Digits | Channel number (2 s), 0 = previous channel | – | Channel number |
| OK | Short: banner, with the banner visible/in timeshift pause/resume; long (0.6 s): programme info window | Switch to channel | Pause/resume |
| ↑ / ↓ | Channel list | Select | +5 / −5 min |
| ← / → | Timeshift −30 s / in timeshift +30 s (live at the end) | Page | −10 / +30 s |
| Back | In timeshift: back to live, otherwise dismiss input/banner, otherwise channel list | Close | Recording list |
| Wheel | Channel ±1, in timeshift ±30 s | Up/down | ±30 s |
| Red | HbbTV | −2 h | – |
| Green | Short: recordings, long: settings | +2 h | – |
| Yellow | Audio track / subtitles | Grid ↔ now/next | Audio track |
| Blue | Teletext | Groups | – |
| REC | Recording dialog | Recording dialog | – |
| Stop | Quit app | – | Recording list |

**HbbTV first:** after 5 s on a channel its HbbTV app starts visibly, as on an
HbbTV TV: without the opening red key (a red-button app shows its hint) and
without error messages. Closed, it does not come back by itself on that channel;
red shows it again at once. The colour keys then work as described next for an
open HbbTV app. Without a visible app green/yellow/blue open recordings, audio
and teletext directly (short or long), red opens HbbTV, a long red press opens the
settings. Long OK opens the programme info window as long as the app does not take OK.

While HbbTV is open in its own renderer, the colour keys belong to the HbbTV app:
short presses go to it (if its keyset requests them, otherwise only a hint is
shown), long presses (from 0.6 s) on green/yellow/blue open TVF's
recordings/audio/teletext, a long press on red closes HbbTV. webOS decides short/long
for red itself: held, no red key arrives, only scancode 453 (and webOS shows the
pointer); TVF takes 453 as long red. The other colour keys come as down/up without
repeats; long fires after 0.6 s while still held. An open TVF window
(teletext, audio menu, recordings, EPG) gets the keys before the HbbTV app.

TV picture behind an HbbTV app (TS 102 796 6.2.2.6, A.2.4.1): the OIPF replacement
reports `video/broadcast` `stop()` and `setChannel(null)` (TV picture off),
`bindToCurrentChannel()` / `setChannel(channel)` (back), and starting an app by http(s)
URL (`createApplication`, broadcast-independent: no TV picture). TVF keeps this state
for the whole HbbTV session, across page loads and after the app's own videos, until
the app brings the picture back or HbbTV is closed – catch-up services no longer show the
running programme between their videos.

The channel object of `video/broadcast` carries the DVB ids (onid, tsid, sid; broadcaster
apps pick their content by them). A tvh with the HTSP extension sends them in the
`sourceinfo` of `subscriptionStart`; otherwise the PMT lookup (`pmtinfo.c`, build option
`TVF_PMT_FALLBACK`) reads them from PAT and SDT of the pass stream. They are reset with
every playback, so a recording's ids do not stay with the live channels after it.

Selecting the current channel again (channel list, EPG, digits) does not restart
it as long as it is playing cleanly – only the banner appears (in timeshift
playback stays time-shifted). If the stream has dropped, has no picture yet, or
a recording or HbbTV playback is running, it is restarted as before.

Wheel and pointer while HbbTV is open: the wheel never changes the channel. If the
page declares pointer support (`keyset.supportsPointer = true`, OIPF DAE 7.2.5.2),
it receives pointer, click and wheel as real mouse events (page coordinates
1280x720); otherwise a click acts as OK and the wheel as ↑/↓, provided its keyset
requests navigation (otherwise they have no effect).

The Magic Remote of the 2024 models has no play/pause or fast-forward/rewind keys; it does
have Info and EPG, but webOS keeps both for itself (they never reach the app and
cannot be remapped). The code for these keys stays for other remotes/keyboards.
Unknown keys are logged with their scancode.

## Settings

Green held (0.6 s) in live TV without HbbTV opens the settings page; a short press
still opens the recordings. With the key layout "HbbTV first" a long red press
opens it (while no HbbTV app is shown). Up/down select a row, OK or left/right change a choice,
OK on a text row opens an on-screen keyboard (arrows + OK, the remote's digits type
directly, Back discards). Changed values are shown in yellow; "Save and restart"
writes them to the settings file and restarts TVF in place (same process, the
launch parameters except the setting ones are kept), Back/Cancel discards.

| Setting | Default | Notes |
|---|---|---|
| Language | Automatic (TV) | every `res/lang/*.json` plus English |
| Key layout | Standard | Standard: green long = settings; HbbTV first: HbbTV starts visibly, settings with a long red press (see "Keys") |
| Search for servers | – | OK: mDNS query for `_htsp._tcp` (tvh's Avahi announcement, `mdns.c`), 1.5 s; left/right picks a server found and fills address and ports |
| TVheadend server address, HTTP/HTSP port | – (build option `TVF_DEFAULT_TVH_HOST`), 9981, 9982 | without an address TVF searches at the start: exactly one server is taken (restart), else the page opens |
| User, password | – | tvh login for HTSP and streams (password masked); a refused login (tvh `noaccess`, not the connection limit) opens the page here once per start |
| Internet access (HbbTV) | Rules per channel | `egress.json`; or direct, or one fixed proxy – for the channels without a proxy tag from tvh |

The proxies and the channel → proxy rules (`egress.json`, empty by default, own
rules from `res/hbbtv/egress.local.json`, see "Build options") are not editable on the TV.

Proxy per channel from tvh channel tags (`tvsrc.c` → `egress.c`): a tag whose name
starts with `HbbTV-Proxy` (any case) assigns its channels a proxy – the rest of the
name (`hbbtv-proxy http://192.0.2.10:8906`, `hbbtv-proxy 192.0.2.10:8906`,
`HbbTV-Proxy DE` with an `egress` name, `hbbtv-proxy direct`) or, if set, the tag's
icon field. Write `host:port` without `http://` there: tvh turns a URL in the icon
field into an `imagecache/<n>` link over HTSP. The tag wins over the *Internet
access* setting, which applies to the channels without one; with *Rules per
channel* only the host exceptions (`overrides`) of `egress.json` come first. TVF
gets the tag over HTSP (tagAdd: name, icon, members) and does not show it as a
channel group. TVF reads the proxy tags on the HTTP requests of the HbbTV page, its
videos and licences (the WebKit page always goes through TVF's relay, also with a
fixed proxy).

Which tags reach TVF:

- **tvheadend fork `L-S-D/tvheadend`** (branch `pr/htsp-internal-tags`, in `v3/*`):
  make the tag *internal*. TVF asks for internal tags (`internalTags` in
  `enableAsyncMetadata`, `htsp.c`); tvh sends them only to clients that ask, so Kodi
  and other HTSP clients see neither the tag nor any change to their channels and
  groups. Internal tags are never shown as channel groups in TVF.
- **plain tvheadend**: internal tags are not sent over HTSP. Mark the tag *private*
  instead and allow it in the TV user's access entry; keep in mind that tvh merges
  all access entries matching user *and* address, and that other clients of that
  user see the tag as a group.

With HTSP the HbbTV AIT comes from the subscription too: tvh sends the AIT PID as a
stream of type `HBBTV` whose packets are the raw AIT sections (without CRC);
`sidedata.c` parses them (`local:ttx/hbbtv`).
If TVF cannot reach the server, the message says to hold green (or red, with "HbbTV
first") for the settings.

## Kore (Kodi remote)

TVF itself answers Kodi's JSON-RPC (`src/app/kodi.c`), so Kore controls the TV
directly: HTTP on port 8080
(`/jsonrpc`, `/image/…` = logos and programme images from tvh's image cache, only
those) and TCP on port 9090 (JSON-RPC plus the notifications `Player.OnPlay`,
`OnPause`, `OnResume`, `OnStop`). TVF announces itself via mDNS like a Kodi
(`_xbmc-jsonrpc-h._tcp`, `_xbmc-jsonrpc._tcp`, `src/net/mdns.c`), so Kore's search
finds it under its name; manually: the TV's IP address, HTTP port 8080, TCP port
9090. Name, ports, user and password are on the settings page ("Kodi remote (Kore)",
empty port = off).

User/password (empty = no sign-in): HTTP (`/jsonrpc` and `/image/`) then requires
Basic authentication like Kodi's web server (enter the same in Kore's host settings),
otherwise 401. Kodi's TCP protocol has no sign-in, so TVF accepts TCP connections only
from addresses that have signed in over HTTP (Kore does that when adding the host and
when loading logos); the list is kept in the settings file (`kore.trusted`, at most 16
addresses) and dropped when user/password change.

Web pages cannot use the interface: like Kodi, `POST /jsonrpc` needs `Content-Type:
application/json` (otherwise 415); a request with an `Origin` header is refused (403)
unless the origin is a private IPv4 address, an IPv6 address, a name without a dot or a
`.local` name; `GET /jsonrpc?request=…` runs only queries (`*.Get*`, `JSONRPC.Ping`,
`Version`, `Introspect`); on TCP anything but JSON values drops the connection. Limits:
16 KiB HTTP header, 256 KiB per request, 32 MiB unread replies per client, 256 queued
requests.

Waking: Kore reads the TV's MAC address (`XBMC.GetInfoLabels` `Network.MacAddress`)
and can switch the TV on with Wake-on-LAN (TV setting "Turn on via Wi-Fi/LAN" must be
on). TVF keeps running while the TV is off ("Active Standby"); it watches the power
state (`com.webos.service.tvpower/power/getPowerState`) and the foreground app. If a
Kore request (Kore polls every few seconds while connected) arrives within 30 s of
the TV being switched on and another app is in front, TVF brings itself to the front
once (`applicationManager/launch` of itself). A channel change from Kore (Player.Open
of a channel, Player.GoTo, channel ±) or playing a recording from Kore always brings
TVF to the front. Debug command `TVON` simulates the switch-on for tests.

What Kore sees and can do: channel groups and channels with now/next and logos,
the EPG per channel (with timer marks and programme images), recordings (resume
position, watched) and timers; switch channels, play a recording (resumes), pause,
seek (recordings; pause in live TV = timeshift), stop (recording → live TV), schedule
or cancel recordings and series (PVR.AddTimer/DeleteTimer/ToggleTimer/Record, result
from tvh), and the remote pad/keys (`Input.*`, `Input.ExecuteAction`: arrows, OK,
back, info, colour keys, digits, channel ±).

Home Assistant uses the same interface (Kodi integration, or plain JSON-RPC over
HTTP): `Player.Open` with `{"item":{"channelid":<channel number>}}` switches
channels and brings TVF to the front, `Player.Stop`, `Input.*` and the other calls
above work as for Kore.

## Languages

The user interface is multilingual. Source texts are English (written in the code
as `_("English text")`); translations live in `res/lang/<language>.json`, a flat
JSON object `{"English text": "translation"}` (German: `res/lang/de.json`). The
language comes from the launch parameter `lang` (e.g. `"lang":"de"`, remembered;
`"auto"` = TV menu language), otherwise from the TV's menu language, with English
as the fallback. Only the part before `-` or `_` counts (`de-AT` → `de`). A missing
file or entry keeps the English text; the log shows the active language (`[i18n]`).

Adding a language:

1. Copy `res/lang/de.json` to e.g. `res/lang/fr.json` and translate the values.
   The keys stay English – they are the source texts the app looks up.
2. Keep the printf conversions (`%s`, `%d`, `%u`, `%ld`, `%02d` …) in the same
   order; date formats such as `"%s %02d/%02d"` are translated the same way.
3. Run `tools/i18n_check.py` (`--missing` lists the missing texts): it reports
   missing entries (stay English), stale entries (text no longer in the sources)
   and printf conversions that differ between text and translation (exit code 1).

`package.sh` packages every `res/lang/*.json`. Test with e.g.
`ares-launch -d <tv> com.lsd.tvf -p '{"lang":"fr"}'`. Every language
except English uses a decimal comma (`i18n_decimal()` in `src/core/i18n.c`).

## Testing without a remote

With `"debug":5556` the app accepts commands via UDP. The port has no
authentication and is only opened with this launch parameter - use it for testing
in a trusted network only.

    echo "KEY GREEN" | socat - UDP:<tv>:5556          # key (names as in key_name)
    socat -u TCP-LISTEN:5558 OPEN:shot.png,creat &             # receiver
    echo "SHOT <pc>:5558" | socat - UDP:<tv>:5556   # UI as PNG (without video)
    echo "WHEEL 1" | socat - UDP:<tv>:5556                  # wheel
    echo "AOFF clear 208" | socat - UDP:<tv>:5556           # sound offset of a channel: ms, or clear (also the last one set)
    echo "VDROP 20" | socat - UDP:<tv>:5556                 # test: discard 20 s of video (picture watchdog)
    echo "CUT 2" | socat - UDP:<tv>:5556                    # test: cut the recording's connection, fail 2 reconnects (5 = give up)
    echo "TVON" | socat - UDP:<tv>:5556                     # test: as if the TV was just switched on (Kore rule)
    echo "LOCAL ttx/subtitles?page=777&since=0" | socat - UDP:<tv>:5556  # own teletext/subtitle memory (HTSP) -> log, with picture PTS
    echo "KEYSHORT YELLOW" | socat - UDP:<tv>:5556          # colour key or OK, short/long as if pressed (KEYLONG ...)
    echo "PTR UP 300 480" | socat - UDP:<tv>:5556          # pointer: MOVE/DOWN/UP x y
    echo 'LUNA luna://com.webos.settingsservice/getSystemSettings {"keys":["localeInfo"]}' \
        | socat - UDP:<tv>:5556                            # system service, reply in the log

HbbTV in its own renderer needs no HbbTV gateway: TVF injects the OIPF replacement
(`res/hbbtv/polyfill.js`) itself, and TVF's local proxy
serves dash.js for its A/V objects (`res/hbbtv/dash.all.min.js`, BSD,
`LICENSE-dashjs.txt`) at `http://127.0.0.1:8199/f/hbbtv-dash.js`. As before, WebKit
blocks http scripts on https pages (mixed content).

Media in WebKit (bundle with `media` in `features`, wpe-webos media build): with
`WEBMEDIA 1` (setting `web.media`, takes effect from the next renderer start)
WebKit plays `<video>`/MSE itself; its sinks send the streams to the
media server `src/app/webmedia.c` (protocol: wpe-webos `PROTOCOL-MEDIA.md`), which
feeds them into the decoder; `media.js` then only runs in selective mode (A/V objects
following a PlayReady initiator of the OIPF DRM agent keep playing via LG's
pipeline, `hbbmedia.c`). The picture size for the exported video window then comes
from that pipeline's VIDEO_INFO, not from the last live picture. `WEBMEDIACT LIVE|none` sets
the `contentsType` of these pipelines. Test bench without a TV: `tools/webmedia-test`
(media server with an emulated decoder, run against WebKit under qemu).

EME (bundle with `eme` in `features`): TVF is the CDM host for WebKit
(`src/app/cdm.cpp`, `webdrm.c`; counterpart: libocdm in wpe-webos). The TV's CDMs
are Chromium library CDMs (interface in `third_party/cdm`). Widevine
(`/usr/lib/libwidevine-wrapper.so`, via `com.webos.service.widevine`) works from
within the app; `Decrypt()` returns the sample for the secure video path (header
`ibpi` in front of the unmodified data), which goes into the BUFFERSTREAM pipeline
together with `option.drm.type` and `svpVersion`. PlayReady (`libplayreadycdm.so`) does not run in the
app process: the porting kit needs the secure partition and
root-only devices (`/dev/lg/crypto0`); TVF therefore does not offer it for EME
(`TVF_CDM_PLAYREADY=1` for experiments), and PlayReady stays with the OIPF DRM agent and
LG's DRM service. Pages that choose the DRM system based on the device get Widevine
(`res/hbbtv/drm.js`): the capability list carries the Widevine DRMSystemID on all
video profiles (ORF also counts a profile without a DRMSystemID as PlayReady-capable),
and Joyn gets its switch `__dev-drm-widevine` (otherwise LG → PlayReady).
Testing without WebKit:

    echo 'CDMTEST com.widevine.alpha https://cwip-shaka-proxy.appspot.com/no_auth <PSSH base64>' \
        | socat - UDP:<tv>:5556     # session, license, keys, Decrypt -> log [cdmtest]

Diagnostics: with `cmake -DTVF_DRM_DEBUG=ON` TVF exports `PmLogGetContext`/`_PmLogMsgKV`
and `open` (`src/app/drmdebug.c`) - the DRM modules' messages and
failed accesses to `/dev/...` then show up in the log. Test bench:
`tools/webmedia-test` has an emulated CDM (`fake_cdm.c`), counterpart:
wpe-webos `sdk/tests/eme`.

The UI can also be rendered on a PC (LVGL into a buffer, PNG):
`tools/uitest` (usage in the header of `tools/uitest/build.sh`; the test data is fetched
read-only by `mkgrid.py`/`mkrecs.py` from a running TVF's Kodi JSON-RPC, `TVF_KODI=<tv>:8080`).

## Structure

| Path | Contents |
|---|---|
| `src/app` | `main.c` (event loop, keys, pointer, wheel, foreground/background, settings defaults), `tvsrc.c` (channels, EPG, live via HTSP), `tvrec.c` (recordings/timers/positions via HTSP), `recplay.c` (recording playback), `recindex.c` (time→byte from PCR samples), `tshift.c` (timeshift), `sidedata.c` (teletext/subtitle/AIT memory of the HTSP subscription), `pmtinfo.c` (teletext subtitle pages from the PMT), `tvhstat.c` (reception info from the live subscription), `kodi.c` (Kodi JSON-RPC for Kore and Home Assistant, with `src/net/mdns.c`), `luna.c` (system services via libhelpers), `keys_sdl.c`, `debugctl.c` (test commands); HbbTV: `webr.c` (WebKit renderer process), `hbbweb.c` (HbbTV session, keys), `hbbtv.c` (entry rewrites, `res/hbbtv/entry.json`), `egress.c` (proxy rules), `hbbmedia.c` (media via media.js), `hbbdrm.c` (OIPF DRM agent), `webmedia.c` (media server for WebKit's own playback), `webdrm.c`/`cdm.cpp` (EME: CDM host for the TV's DRM modules), `cdmtest.c`, `drmdebug.c` (build option) |
| `src/player` | TS demuxer, Starfish binding (`smp_api.cpp`, ACB on webOS 4), HTSP and HTTP feeders, HLS, MP2→PCM |
| `src/net` | HTSP (`htsp.c`: one connection, subscription routing; `htsmsg.c`), HTTP client (Digest, chunked, Range, TLS, proxy), `fetch.c` (asynchronous GETs), `relay.c` (local proxy for WebKit), `mdns.c`, MD5, SHA-1 |
| `src/core` | Log, `prefs.c` (settings file), `i18n.c` (UI translations), `ttxdec.c`/`dvbsubdec.c` (teletext and DVB subtitle decoders) |
| `src/ui` | `ui.c` (fonts, images, building blocks), `ui_sdl.c` (display), `osd.c`, `info.c`, `epg.c`, `choice.c`, `recdlg.c`, `recbrowser.c`, `settings.c`, `ttx.c`, `subs.c`, `imgtrim.c`, `keys.c`, `lv_conf.h` |
| `third_party` | LVGL (submodule), cJSON, minimp3, stb, Chromium CDM headers |
| `res/fonts` | Lato (OFL), Bedstead (CC0, teletext) |
| `res/hbbtv` | OIPF replacement (`polyfill.js`), `media.js`, `drm.js`, dash.js, `entry.json`, `egress.json` (+ example) |
| `res/lang`, `res/langnames` | UI translations (`de.json`), see "Languages"; language names |
| `licenses`, `THIRD_PARTY_NOTICES.md` | third-party licences (also packed into the IPK) |
| `tools/tsprobe.c` | Check the TS part on a PC (file or URL) |
| `tools/uitest` | Render the UI on a PC (PNG), without SDL/TV |
| `tools/webmedia-test` | Test bench for the WebKit media server and CDM host |
| `tools/i18n_check.py` | Check the translation files against the sources |

The SDL surface lies above the video layer; LVGL draws in ARGB8888, and whatever is
not drawn stays transparent (the window surface requests 8-bit alpha for this).
On webOS 5+ the video layer is attached to an exported SDL window (webOS 25
no longer has ACB). webOS 4 has no exported windows: there ACB
(`libAcbAPI`, via `dlopen`) binds each pipeline to the video layer — mediaId and LOADED
after `Load()`, then PLAYING, the video data from VIDEO_INFO via
`setMediaVideoData`. The app decides at startup which route to use, based on whether
the TV's SDL has `SDL_webOSCreateExportedWindow`.

## Pitfalls

* Never load H.264 with `adaptiveStreaming` 3840x2160@60: error 601 on webOS 25.
* Look up optional functions (exported windows, newer Starfish methods) with `dlsym`,
  never via weak symbols: in the executable (no PIE) their
  address is the PLT entry, never NULL — if the function is missing, the call jumps to 0.
* webOS 4: `AcbAPI_setMediaVideoData` writes a task number (third
  parameter `long *`), unlike the SDK header says — without it, a crash in `createTask`.
* webOS 4: `Play()` already arrives from LOADCOMPLETED, while `Load()` is still running.
  ACB only accepts PLAYING after mediaId+LOADED (otherwise the audio is only released after
  ~7 s) — `smp_api` holds it back until then.
* webOS 4: the system only hides the Magic Remote pointer if the app sets an
  idle time (`SDL_HINT_WEBOS_CURSOR_SLEEP_TIME`, 5 s here) — otherwise it stays
  on screen, and a CURSOR_HIDE never arrives.
* Crash: the app writes the signal, pc/lr including the library, and a backtrace to the log;
  resolve addresses in `tvf` with `addr2line` from the SDK.
* Wi-Fi: with a poor connection a webOS 4 TV only got ~3 Mbit/s
  (ORF1 HD needs ~10) — stuttering and slow start; reconnecting helped
  (13–18 Mbit/s). Checking without the app: `curl` in the TV's developer mode.
* `ares-launch` does not restart a running app — use `--close` first.
* HTSP stream: teletext and subtitles come from the subscription (tvh sends the
  TELETEXT and DVBSUB streams without a patch). The page memory starts with the
  subscription – right after a channel change a page is there once it was broadcast
  (page 100 within seconds, rare pages after a full cycle). It is kept for timeshift jumps
  and reconnects of the same channel; subtitle events are dropped then (new time base,
  `epoch` in the replies). Teletext subtitle pages (777, 150) are listed once one was
  broadcast; a remembered choice starts then. tvh stamps teletext with the PCR instead of
  the PES PTS – measured on ORF1 the PTS is only ~60 ms later, so subtitles are not early
  noticeably. A plain tvh's HTSP does not carry the DVB subtitling type; TVF reads it from the PMT
  (see above) for the "hard of hearing" label.
* tvh timeshift option "Include teletext" (Configuration → Recording → Timeshift, expert)
  works the wrong way round (tvh commit 349fa78cb, 2017): when it is ON, teletext packets
  are *not* written to the timeshift buffer – after a jump back there is no teletext and
  no teletext subtitles until live again (DVB subtitles are not affected). Leave it OFF.
* TVF uses only HTSP and tvh's HTTP server (PMT via `/stream/channelid?profile=pass`,
  `imagecache`, `/dvrfile`). A plain tvheadend (checked with upstream 941d132fd, HTSP 44) works; a tvh with
  the HTSP extensions (component tag, subtitling type, teletext pages, receiver mix) saves the
  PMT lookup, and one with an instant-zap buffer (keyframe from a ring buffer, warm muxes)
  speeds up the HTSP start without any setting in TVF.
* Lato has no arrow/check-mark symbols: use Montserrat (LV_SYMBOL_*) for them in separate
  labels, otherwise tiny_ttf searches for each character in vain and logs errors.

Logo: `logo.svg` (the font Lato Black must be installed), icons generated from it:
`rsvg-convert -w 80 -h 80 logo.svg -o icon.png` and
`rsvg-convert -w 130 -h 130 logo.svg -o largeIcon.png`.
