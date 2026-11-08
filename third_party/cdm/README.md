# Chromium CDM API (webOS variant)

`content_decryption_module.h` and `content_decryption_module_export.h` are the
Chromium "library CDM" interface (BSD-3-Clause, The Chromium Authors) in the
form used by Kodi's inputstream.adaptive
(https://github.com/xbmc/inputstream.adaptive, `lib/cdm/cdm/media/cdm/api`),
which carries the webOS changes under `TARGET_WEBOS`:

- module entry point `InitializeCdmModule_10`,
- `ContentDecryptionModule_10/_11::Decrypt()` takes a third argument, the
  stream type.

The TV's CDMs implement this interface: `/usr/lib/libwidevine-wrapper.so`
(Widevine, talks to `com.webos.service.widevine`; `Decrypt()` returns the
sample with secure-video-path metadata appended) and
`/usr/lib/libplayreadycdm.so` (PlayReady). TVF's `src/app/cdm.cpp` is the host.
