// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TVF: media playback of the HbbTV page via TVF. TVF's own renderer
 * (WebKit without media) only knows <video>/<audio> as empty elements. This
 * layer turns them into media elements with their own prototype: src/play/pause/
 * seek go to TVF (window.webkit.messageHandlers.tvf), the TV's media server
 * plays in the video plane beneath the page, the state comes back via
 * __tvfMedia.update(). Runs before the OIPF polyfill (polyfill.js): its
 * A/V objects use a <video> and, for DASH, dash.js - replaced here by
 * src = MPD (the media server can do DASH itself).
 *
 * Selective mode (__HBBTV_FWD.selective, the renderer plays media itself): nothing is
 * replaced globally - WebKit plays <video>/MSE. Only what drm.js redirects (A/V
 * objects after a PlayReady initiator of the OIPF DRM agent: only LG's pipeline
 * with the DRM client can do that) runs via TVF: __tvfMedia.adopt(el)
 * for an element, __tvfMedia.dashShim for dash.js.
 */
(function () {
  'use strict';
  if (window.__tvfMedia) return;
  function post(m) { try { window.webkit.messageHandlers.tvf.postMessage(m); } catch (e) { /* no TVF */ } }
  var M = window.__tvfMedia = { seq: 0, els: {}, active: 0 };
  var SEL = !!(window.__HBBTV_FWD && window.__HBBTV_FWD.selective);
  var NM = window.HTMLMediaElement && window.HTMLMediaElement.prototype; /* WebKit's own (selective mode) */
  var EP = Element.prototype;
  var nCreate0 = Document.prototype.createElement;
  var nGet = EP.getAttribute, nSet = EP.setAttribute, nRem = EP.removeAttribute, nHas = EP.hasAttribute;

  function isMedia(el) { return !!el && (el.tagName === 'VIDEO' || el.tagName === 'AUDIO'); }
  function abs(u) { try { return new URL(String(u), document.baseURI).href; } catch (e) { return String(u); } }
  function fire(el, name) {
    setTimeout(function () { try { el.dispatchEvent(new Event(name)); } catch (e) { /* ignore */ } }, 0);
  }
  function ranges(a) {
    return { length: a.length, start: function (i) { return a[i][0]; }, end: function (i) { return a[i][1]; } };
  }
  function trackList() {
    var l = [];
    l.addEventListener = l.removeEventListener = function () {};
    l.getTrackById = function () { return null; };
    return l;
  }

  function st(el) {
    var s = el.__tvf;
    if (!s) {
      var a = nGet.call(el, 'src');
      s = el.__tvf = { id: ++M.seq, src: a ? abs(a) : '', paused: true, time: 0, dur: NaN, ended: false, ready: 0,
                       err: null, rate: 1, vol: 1, muted: nHas.call(el, 'muted'), seeking: false, started: false,
                       text: trackList(), audio: trackList() };
      M.els[s.id] = el;
    }
    return s;
  }
  function srcOf(el) {
    var s = st(el);
    if (s.src) return s.src;
    var so = el.querySelector('source[src]');
    return so ? abs(nGet.call(so, 'src')) : '';
  }
  function mimeOf(el) {
    var so = el.querySelector('source[type]');
    return so ? nGet.call(so, 'type') : (nGet.call(el, 'type') || '');
  }
  function rect(el) {
    var r = el.getBoundingClientRect();
    return { x: r.left, y: r.top, w: r.width, h: r.height, vw: window.innerWidth, vh: window.innerHeight };
  }
  function stopIfStarted(el) {
    var s = st(el);
    if (s.started) {
      s.started = false;
      post({ type: 'tvf-media', op: 'stop', id: s.id });
    }
  }
  function setSrc(el, v) {
    var s = st(el), url = v ? abs(v) : '';
    stopIfStarted(el);
    s.src = url;
    s.time = 0;
    s.dur = NaN;
    s.ended = false;
    s.err = null;
    s.ready = 0;
    s.paused = true;
    s.meta = '';
    if (url) {
      fire(el, 'loadstart');
      if (nHas.call(el, 'autoplay')) el.play();
      else if (nGet.call(el, 'preload') !== 'none') preloadMeta(el, url);
    } else {
      fire(el, 'emptied');
    }
  }

  // Metadata in advance, like a browser when preloading: read the start of the
  // source (up to 64 KB) and report duration and readiness (loadedmetadata,
  // durationchange, loadeddata, canplay) without TVF playing yet. Some
  // players only call play() after that (hyoga/Tele 5 only requests the ads on
  // durationchange, Joyn only plays its ads after
  // canplay). Duration: DASH mediaPresentationDuration (dynamic: Infinity), HLS
  // sum of the EXTINF of the first variant (without EXT-X-ENDLIST: Infinity), MP4
  // from mvhd. If that fails (CORS, format, moov at the end), everything stays as
  // without preloading.
  function preloadMeta(el, url) {
    var s0 = st(el);
    if (s0.meta === url) return;
    s0.meta = url;
    function report(dur) {
      var s = st(el);
      if (srcOf(el) !== url || s.started || s.ready || !(dur > 0)) return;
      s.dur = dur;
      s.ready = 4;
      ['loadedmetadata', 'durationchange', 'loadeddata', 'canplay', 'canplaythrough'].forEach(function (n) { fire(el, n); });
    }
    function get(u, range, cb) {
      try {
        var x = new XMLHttpRequest();
        x.open('GET', u);
        x.responseType = 'arraybuffer';
        if (range) x.setRequestHeader('Range', 'bytes=0-65535');
        x.onload = function () {
          cb(x.status >= 200 && x.status < 300 ? new Uint8Array(x.response || new ArrayBuffer(0)) : null,
             String(x.getResponseHeader('Content-Type') || ''), x.responseURL || u, x.status);
        };
        x.onerror = function () { cb(null, '', u, 0); };
        x.send();
      } catch (e) { /* ignore */ }
    }
    function text(b) { try { return new TextDecoder('utf-8').decode(b); } catch (e) { return ''; } }
    function hlsDur(t) {
      var d = 0, m, re = /#EXTINF:\s*([\d.]+)/g;
      while ((m = re.exec(t))) d += parseFloat(m[1]);
      return /#EXT-X-ENDLIST/.test(t) ? d : Infinity;
    }
    function isoDur(v) {
      var m = /^P(?:([\d.]+)D)?(?:T(?:([\d.]+)H)?(?:([\d.]+)M)?(?:([\d.]+)S)?)?$/.exec(String(v || ''));
      return m ? (parseFloat(m[1] || 0) * 86400 + parseFloat(m[2] || 0) * 3600 + parseFloat(m[3] || 0) * 60 +
                  parseFloat(m[4] || 0)) : 0;
    }
    function mp4Dur(b) {
      var dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
      for (var i = 4; i + 32 < b.length; i++) {
        if (b[i] !== 0x6d || b[i + 1] !== 0x76 || b[i + 2] !== 0x68 || b[i + 3] !== 0x64) continue; /* 'mvhd' */
        var p = i + 8;
        if (b[i + 4] === 1) {
          var ts1 = dv.getUint32(p + 16);
          return ts1 ? (dv.getUint32(p + 20) * 4294967296 + dv.getUint32(p + 24)) / ts1 : 0;
        }
        var ts = dv.getUint32(p + 8);
        return ts ? dv.getUint32(p + 12) / ts : 0;
      }
      return 0;
    }
    function hls(t, base, full) {
      if (t.indexOf('#EXT-X-STREAM-INF') < 0) {
        /* media playlist: truncated (Range) -> fetch it in full */
        if (!full && t.indexOf('#EXT-X-ENDLIST') < 0) {
          get(url, false, function (b2) { if (b2) report(hlsDur(text(b2))); });
          return;
        }
        report(hlsDur(t));
        return;
      }
      var l = t.split(/\r?\n/), v = '';
      for (var i = 0; i < l.length && !v; i++) {
        if (l[i].indexOf('#EXT-X-STREAM-INF') !== 0) continue;
        for (var j = i + 1; j < l.length && !v; j++) if (l[j] && l[j].charAt(0) !== '#') v = l[j].trim();
      }
      if (!v) return;
      try { v = new URL(v, base).href; } catch (e) { return; }
      get(v, false, function (b2) { if (b2) report(hlsDur(text(b2))); });
    }
    get(url, true, function (b, ct, base, status) {
      if (!b) return;
      var head = text(b.subarray(0, Math.min(b.length, 4096)));
      if (/mpegurl/i.test(ct) || head.indexOf('#EXTM3U') >= 0) {
        hls(text(b), base, status === 200 && b.length < 65536);
      } else if (/dash\+xml/i.test(ct) || /<MPD[\s>]/.test(head)) {
        var all = text(b), mpd = /<MPD[^>]*>/.exec(all);
        if (!mpd) return;
        if (/type\s*=\s*["']dynamic["']/.test(mpd[0])) report(Infinity);
        else {
          var d = /mediaPresentationDuration\s*=\s*["']([^"']+)["']/.exec(mpd[0]);
          if (d) report(isoDur(d[1]));
          else {
            /* no total duration (e.g. stitched ad break): sum of the periods */
            var sum = 0, pm, pre = /<Period\b[^>]*\bduration\s*=\s*["']([^"']+)["']/g;
            while ((pm = pre.exec(all))) sum += isoDur(pm[1]);
            report(sum);
          }
        }
      } else {
        report(mp4Dur(b));
      }
    });
  }

  // ---- prototypes ----
  function Illegal() { throw new TypeError('Illegal constructor'); }
  var MediaEl = function HTMLMediaElement() { Illegal(); };
  MediaEl.prototype = Object.create(HTMLElement.prototype, { constructor: { value: MediaEl, writable: true, configurable: true } });
  var VideoEl = function HTMLVideoElement() { Illegal(); };
  VideoEl.prototype = Object.create(MediaEl.prototype, { constructor: { value: VideoEl, writable: true, configurable: true } });
  var AudioEl = function HTMLAudioElement() { Illegal(); };
  AudioEl.prototype = Object.create(MediaEl.prototype, { constructor: { value: AudioEl, writable: true, configurable: true } });
  [['NETWORK_EMPTY', 0], ['NETWORK_IDLE', 1], ['NETWORK_LOADING', 2], ['NETWORK_NO_SOURCE', 3], ['HAVE_NOTHING', 0],
   ['HAVE_METADATA', 1], ['HAVE_CURRENT_DATA', 2], ['HAVE_FUTURE_DATA', 3], ['HAVE_ENOUGH_DATA', 4]].forEach(function (c) {
    MediaEl[c[0]] = c[1];
    MediaEl.prototype[c[0]] = c[1];
  });

  var P = MediaEl.prototype;
  function prop(obj, name, get, set) {
    Object.defineProperty(obj, name, { configurable: true, enumerable: true, get: get, set: set });
  }
  // attribute mirrors (boolean or text)
  ['autoplay', 'controls', 'loop', 'playsInline', 'defaultMuted'].forEach(function (n) {
    var a = n === 'playsInline' ? 'playsinline' : n === 'defaultMuted' ? 'muted' : n;
    prop(P, n, function () { return nHas.call(this, a); },
         function (v) { if (v) nSet.call(this, a, ''); else nRem.call(this, a); });
  });
  ['preload', 'crossOrigin'].forEach(function (n) {
    var a = n.toLowerCase();
    prop(P, n, function () { return nGet.call(this, a); }, function (v) { nSet.call(this, a, String(v)); });
  });
  prop(VideoEl.prototype, 'poster', function () { var p = nGet.call(this, 'poster'); return p ? abs(p) : ''; },
       function (v) { nSet.call(this, 'poster', String(v)); });
  ['width', 'height'].forEach(function (n) {
    prop(VideoEl.prototype, n, function () { return parseInt(nGet.call(this, n), 10) || 0; },
         function (v) { nSet.call(this, n, String(v)); this.style[n] = (parseInt(v, 10) || 0) + 'px'; });
  });
  prop(VideoEl.prototype, 'videoWidth', function () { return st(this).ready ? 1280 : 0; });
  prop(VideoEl.prototype, 'videoHeight', function () { return st(this).ready ? 720 : 0; });

  prop(P, 'src', function () { return st(this).src; }, function (v) { setSrc(this, v); });
  prop(P, 'currentSrc', function () { return srcOf(this); });
  prop(P, 'currentTime', function () { return st(this).time; }, function (v) {
    var s = st(this);
    v = Number(v) || 0;
    s.time = v;
    s.ended = false;
    if (s.started) {
      s.seeking = true;
      fire(this, 'seeking');
      post({ type: 'tvf-media', op: 'seek', id: s.id, time: v });
    }
  });
  prop(P, 'duration', function () { return st(this).dur; });
  prop(P, 'paused', function () { return st(this).paused; });
  prop(P, 'ended', function () { return st(this).ended; });
  prop(P, 'seeking', function () { return st(this).seeking; });
  prop(P, 'readyState', function () { return st(this).ready; });
  prop(P, 'networkState', function () { return st(this).src ? 2 : 0; });
  prop(P, 'error', function () { return st(this).err; });
  prop(P, 'playbackRate', function () { return st(this).rate; }, function (v) { st(this).rate = Number(v) || 1; });
  prop(P, 'defaultPlaybackRate', function () { return 1; }, function () {});
  prop(P, 'volume', function () { return st(this).vol; }, function (v) { st(this).vol = Number(v); fire(this, 'volumechange'); });
  prop(P, 'muted', function () { return st(this).muted; }, function (v) { st(this).muted = !!v; fire(this, 'volumechange'); });
  prop(P, 'buffered', function () { var s = st(this); return ranges(s.ready >= 2 ? [[0, s.time]] : []); });
  prop(P, 'seekable', function () { var s = st(this); return ranges(isFinite(s.dur) ? [[0, s.dur]] : []); });
  prop(P, 'played', function () { return ranges([]); });
  prop(P, 'textTracks', function () { return st(this).text; });
  prop(P, 'audioTracks', function () { return st(this).audio; });
  prop(P, 'videoTracks', function () { return trackList(); });
  prop(P, 'mediaKeys', function () { return null; });

  P.canPlayType = function (t) {
    t = String(t || '').toLowerCase();
    return /(video|audio)\/(mp4|mpeg|mp2t|x-m4v|aac)|mpegurl|dash\+xml/.test(t) ? 'probably' : '';
  };
  P.addTextTrack = function () { return { mode: 'disabled', cues: [], addCue: function () {}, removeCue: function () {} }; };
  P.setMediaKeys = function () { return Promise.reject(new DOMException('no DRM', 'NotSupportedError')); };
  P.fastSeek = function (t) { this.currentTime = t; };
  P.load = function () {
    var s = st(this);
    stopIfStarted(this);
    s.time = 0;
    s.ended = false;
    s.err = null;
    s.ready = 0;
    s.paused = true;
    s.meta = '';
    if (srcOf(this)) {
      fire(this, 'loadstart');
      if (nHas.call(this, 'autoplay')) this.play();
      else if (nGet.call(this, 'preload') !== 'none') preloadMeta(this, srcOf(this));
    }
  };
  P.play = function () {
    var el = this, s = st(el), url = srcOf(el);
    if (!url) return Promise.reject(new DOMException('no source', 'NotSupportedError'));
    var was = s.paused;
    s.paused = false;
    if (was) fire(el, 'play');
    hole(el);
    if (!s.started) {
      s.started = true;
      M.active = s.id;
      post({ type: 'tvf-media', op: 'play', id: s.id, url: url, mime: mimeOf(el), time: s.time, rect: rect(el) });
    } else if (was) {
      post({ type: 'tvf-media', op: 'resume', id: s.id });
    }
    return Promise.resolve();
  };
  P.pause = function () {
    var s = st(this);
    if (s.paused) return;
    s.paused = true;
    if (s.started) post({ type: 'tvf-media', op: 'pause', id: s.id });
    fire(this, 'pause');
  };

  if (!SEL) {
    window.HTMLMediaElement = MediaEl;
    window.HTMLVideoElement = VideoEl;
    window.HTMLAudioElement = AudioEl;
    window.Audio = function Audio(src) {
      var a = document.createElement('audio');
      if (src !== undefined) a.src = src;
      return a;
    };
    window.Audio.prototype = AudioEl.prototype;
  }

  // ---- upgrading elements ----
  // without media, <video>/<audio> are unknown inline elements for WebKit,
  // without a size: default width like a real video (300), the height from the
  // aspect ratio of a playing video (16:9) - so height:auto or
  // 100% in a container without a height does not become 0 (hyoga/Tele 5 otherwise
  // waits forever until the element is visible). Page CSS takes precedence.
  if (!SEL) (function () {
    var css = nCreate0.call(document, 'style');
    css.textContent = 'video{display:inline-block;width:300px;aspect-ratio:16/9}audio{display:none}audio[controls]{display:inline-block}';
    var root = document.documentElement;
    if (root) root.insertBefore(css, root.firstChild);
  })();
  function sizeAttrs(el) {
    var w = nGet.call(el, 'width'), h = nGet.call(el, 'height');
    if (w && !el.style.width) el.style.width = /^\d+$/.test(w) ? w + 'px' : w;
    if (h && !el.style.height) el.style.height = /^\d+$/.test(h) ? h + 'px' : h;
  }
  // values set before the upgrade (element via innerHTML, right after that
  // el.src = ...) sit as own properties on the element and would otherwise
  // hide those of the prototype - src first, it resets the time
  var OWN = ['src', 'currentTime', 'muted', 'defaultMuted', 'volume', 'autoplay', 'loop', 'preload', 'poster', 'playbackRate',
             'controls', 'crossOrigin', 'playsInline', 'width', 'height'];
  function upgrade(el) {
    if (!isMedia(el) || el.__tvfUp) return;
    el.__tvfUp = true;
    if (SEL && NM) {
      /* WebKit may already have played the element (ad before it): release it */
      try { NM.pause.call(el); } catch (e) { /* ignore */ }
      if (nHas.call(el, 'src')) {
        nRem.call(el, 'src');
        try { NM.load.call(el); } catch (e) { /* ignore */ }
      }
    }
    var own = [];
    for (var i = 0; i < OWN.length; i++) {
      if (Object.prototype.hasOwnProperty.call(el, OWN[i])) {
        own.push([OWN[i], el[OWN[i]]]);
        delete el[OWN[i]];
      }
    }
    Object.setPrototypeOf(el, el.tagName === 'AUDIO' ? AudioEl.prototype : VideoEl.prototype);
    sizeAttrs(el);
    var s = st(el);
    for (var k = 0; k < own.length; k++) {
      try { el[own[k][0]] = own[k][1]; } catch (e) { /* ignore */ }
    }
    if (s.src && nHas.call(el, 'autoplay') && !s.started) setTimeout(function () { el.play(); }, 0);
  }
  function upgradeTree(n) {
    if (!n || n.nodeType !== 1) return;
    upgrade(n);
    if (n.getElementsByTagName) {
      var v = n.getElementsByTagName('video'), a = n.getElementsByTagName('audio'), i;
      for (i = 0; i < v.length; i++) upgrade(v[i]);
      for (i = 0; i < a.length; i++) upgrade(a[i]);
    }
  }
  var nCreate = Document.prototype.createElement;
  if (!SEL) Document.prototype.createElement = function (name) {
    var el = nCreate.apply(this, arguments);
    upgrade(el);
    return el;
  };
  /* selective mode: only redirected elements */
  function ours(el) { return isMedia(el) && (!SEL || el.__tvfUp); }
  EP.setAttribute = function (n, v) {
    if (ours(this) && String(n).toLowerCase() === 'src') { upgrade(this); setSrc(this, v); return; }
    return nSet.apply(this, arguments);
  };
  EP.removeAttribute = function (n) {
    if (ours(this) && String(n).toLowerCase() === 'src') { upgrade(this); setSrc(this, ''); return; }
    return nRem.apply(this, arguments);
  };
  EP.hasAttribute = function (n) {
    if (ours(this) && String(n).toLowerCase() === 'src' && this.__tvf) return !!this.__tvf.src;
    return nHas.apply(this, arguments);
  };
  new MutationObserver(function (ms) {
    for (var i = 0; i < ms.length; i++) {
      var add = ms[i].addedNodes;
      for (var k = 0; !SEL && k < add.length; k++) upgradeTree(add[k]);
    }
    // element removed from the document: stop playback
    for (var id in M.els) {
      var el = M.els[id];
      if (!el.isConnected) {
        if (el.__tvf.started) {
          el.__tvf.started = false;
          post({ type: 'tvf-media', op: 'stop', id: el.__tvf.id });
        }
        delete M.els[id];
      }
    }
  }).observe(document, { childList: true, subtree: true });
  if (!SEL && document.documentElement) upgradeTree(document.documentElement);
  /* redirect an element to TVF (selective mode: drm.js) */
  M.adopt = function (el) { upgrade(el); return el; };

  // ---- state from TVF: state playing|paused|buffering|ended|error, time/duration in s ----
  M.update = function (id, u) {
    var el = M.els[id];
    if (!el) return;
    var s = el.__tvf;
    if (typeof u.duration === 'number' && u.duration > 0 && u.duration !== s.dur) {
      var first = !isFinite(s.dur);
      s.dur = u.duration;
      if (first) { s.ready = Math.max(s.ready, 1); fire(el, 'loadedmetadata'); }
      fire(el, 'durationchange');
    }
    if (typeof u.time === 'number' && u.time >= 0) {
      s.time = u.time;
      if (s.seeking) { s.seeking = false; fire(el, 'seeked'); }
      fire(el, 'timeupdate');
    }
    if (u.state === 'playing') {
      if (s.ready < 4) {
        s.ready = 4;
        fire(el, 'loadeddata');
        fire(el, 'canplay');
        fire(el, 'canplaythrough');
      }
      if (!s.paused) fire(el, 'playing');
    } else if (u.state === 'buffering') {
      fire(el, 'waiting');
    } else if (u.state === 'ended') {
      s.ended = true;
      s.paused = true;
      s.started = false;
      fire(el, 'pause');
      fire(el, 'ended');
    } else if (u.state === 'error') {
      // MediaError: 2 network, 3 decode (DRM errors 5xx, HbbTV 9.6.7), 4 not supported
      var c = u.code >= 500 && u.code < 600 ? 3 : u.code === -1 ? 4 : 2;
      s.err = { code: c, message: 'TVF ' + u.code, MEDIA_ERR_NETWORK: 2, MEDIA_ERR_DECODE: 3, MEDIA_ERR_SRC_NOT_SUPPORTED: 4 };
      s.started = false;
      fire(el, 'error');
    }
  };

  // position of the playing element: the video plane follows
  setInterval(function () {
    var el = M.els[M.active];
    if (el && el.__tvf.started) {
      post({ type: 'tvf-media', op: 'rect', id: el.__tvf.id, rect: rect(el) });
      hole(el);
    } else {
      unhole();
    }
  }, 500);

  // Hole for the video plane beneath the page, as in a TV browser: the
  // video replaces the pixels of all layers below it, whatever lies above stays.
  // TVF's own renderer (bundle with "video-hole" in features, TVF reports it
  // as __HBBTV_FWD.hole) punches it out in the compositor: the element gets
  // its own layer (will-change) and the attribute tvf-hole, WebKit paints
  // its content box transparent and without blending. Older bundles: the
  // clip-path approximation further below (holeJS).
  var NATIVE = !!(window.__HBBTV_FWD && window.__HBBTV_FWD.hole), holed = null;
  function hole(el) {
    if (!NATIVE) { holeJS(el); return; }
    if (holed !== el) {
      unhole();
      holed = el;
      el.__tvfWc = [el.style.getPropertyValue('will-change'), el.style.getPropertyPriority('will-change')];
    }
    // own layer, even if the page has overwritten will-change in the meantime
    if (el.style.getPropertyValue('will-change') !== 'transform' || el.style.getPropertyPriority('will-change') !== 'important')
      el.style.setProperty('will-change', 'transform', 'important');
    if (!nHas.call(el, 'tvf-hole')) nSet.call(el, 'tvf-hole', '');
  }
  function unhole() {
    if (!NATIVE) { if (below.length) unholeJS(); return; }
    var el = holed;
    if (!el) return;
    holed = null;
    nRem.call(el, 'tvf-hole');
    var w = el.__tvfWc;
    if (w && w[0]) el.style.setProperty('will-change', w[0], w[1]);
    else el.style.removeProperty('will-change');
  }

  // Approximation without a hole in the compositor: the element and ancestors
  // of the same size transparent, larger ancestors with a background get
  // a copy with a cut-out behind them (like the video/broadcast object in the
  // OIPF polyfill), plus holeBelow()
  var BG = ['backgroundColor', 'backgroundImage', 'backgroundSize', 'backgroundPosition', 'backgroundRepeat'];
  function holeJS(el) {
    el.style.background = 'transparent';
    var r = el.getBoundingClientRect();
    if (r.width < 2 || r.height < 2) {
      if (below.length) unholeJS();
      return;
    }
    for (var p = el.parentElement; p; p = p.parentElement) {
      var cs = getComputedStyle(p), h = p.__tvfHole;
      var bg = cs.backgroundColor !== 'rgba(0, 0, 0, 0)' || cs.backgroundImage !== 'none';
      if (!bg && !h) continue;
      var q = p.getBoundingClientRect();
      if (!h && q.width <= r.width + 4 && q.height <= r.height + 4) {
        p.style.background = 'transparent';
        continue;
      }
      if (!h) {
        h = p.__tvfHole = nCreate.call(document, 'div');
        h.__tvfCopy = true;
        h.style.cssText = 'position:fixed;z-index:-1;pointer-events:none;';
        for (var i = 0; i < BG.length; i++) h.style[BG[i]] = cs[BG[i]];
        p.style.background = 'transparent';
      }
      if (!h.isConnected) (document.body || document.documentElement).appendChild(h);
      var s = h.style;
      s.left = q.left + 'px';
      s.top = q.top + 'px';
      s.width = q.width + 'px';
      s.height = q.height + 'px';
      s.webkitMaskImage = s.maskImage = 'linear-gradient(#000,#000),linear-gradient(#000,#000)';
      s.webkitMaskRepeat = s.maskRepeat = 'no-repeat';
      s.webkitMaskSize = s.maskSize = '100% 100%,' + r.width + 'px ' + r.height + 'px';
      s.webkitMaskPosition = s.maskPosition = '0 0,' + (r.left - q.left) + 'px ' + (r.top - q.top) + 'px';
      s.webkitMaskComposite = 'xor';
      s.maskComposite = 'exclude';
    }
    holeBelow(el, r);
  }

  // Punch out everything that lies in the video area BENEATH the element - like
  // the hole of a real HbbTV browser, where the video replaces the pixels
  // below. holeJS() takes care of the ancestors; this handles foreign elements, e.g.
  // RTL's black loading screen next to the player or, in full screen, the whole
  // page with the program selection. A grid of samples across the
  // video area (elementsFromPoint, on the TV ~0.1 ms per point) yields what
  // lies above and what lies beneath. For each element beneath, the highest
  // ancestor is looked up that contains neither the video nor anything above it;
  // that branch gets a clip-path with a cut-out (text and images included; a
  // mask would also cut off everything that sticks out of the element's box).
  // If the element itself already contains something above the video, only its
  // solid-color background becomes a frame of four areas around the cut-out.
  // Full sampling when the video moves, otherwise every 2 s. When playback
  // ends or an element no longer lies beneath, its style is
  // restored. window.__tvfNoHoleBelow = true turns this off.
  var below = []; // { el, saved: {property: [value, priority]}, color, mode: 'clip'|'frame' }
  var FRAME = ['background-color', 'background-image', 'background-repeat', 'background-origin', 'background-clip',
               'background-position', 'background-size'];
  var CLIP = ['clip-path'];
  var NX = 12, NY = 7, lastKey = '', tick = 0, sheet = null;
  function save(e, props) {
    var s = {};
    for (var i = 0; i < props.length; i++) s[props[i]] = [e.style.getPropertyValue(props[i]), e.style.getPropertyPriority(props[i])];
    return s;
  }
  function restore(b) {
    for (var p in b.saved) {
      if (b.saved[p][0]) b.el.style.setProperty(p, b.saved[p][0], b.saved[p][1]);
      else b.el.style.removeProperty(p);
    }
  }
  function unholeJS() {
    for (var i = 0; i < below.length; i++) restore(below[i]);
    below = [];
    lastKey = '';
  }
  function setI(e, p, v) { e.style.setProperty(p, v, 'important'); }
  function within(t, list) {
    for (var i = 0; i < list.length; i++) if (t.contains(list[i])) return true;
    return false;
  }
  // Samples: what lies above (up) and below (dn) el at the grid points? For this,
  // all elements are made hit-testable (pointer-events, via an adopted stylesheet
  // without DOM changes) and our own cut-outs are briefly lifted -
  // otherwise hit testing no longer sees cut-out areas
  function sample(el, r) {
    for (var i = 0; i < below.length; i++) if (below[i].mode === 'clip') restore(below[i]);
    var pv = el.style.getPropertyValue('pointer-events'), pp = el.style.getPropertyPriority('pointer-events');
    el.style.setProperty('pointer-events', 'auto', 'important');
    var adopted = null;
    try {
      if (!sheet) { sheet = new CSSStyleSheet(); sheet.replaceSync('*{pointer-events:auto!important}'); }
      // copy: adoptedStyleSheets is a live array
      adopted = [].slice.call(document.adoptedStyleSheets);
      document.adoptedStyleSheets = adopted.concat([sheet]);
    } catch (e) { adopted = null; }
    var up = [], dn = [];
    for (var gy = 0; gy < NY; gy++) {
      for (var gx = 0; gx < NX; gx++) {
        var l = document.elementsFromPoint(r.left + r.width * (gx + 0.5) / NX, r.top + r.height * (gy + 0.5) / NY);
        var k = l.indexOf(el);
        if (k < 0) continue;
        for (var j = 0; j < l.length; j++) {
          var e = l[j];
          if (j < k) { if (up.indexOf(e) < 0) up.push(e); }
          else if (j > k && !e.__tvfCopy && !e.contains(el) && dn.indexOf(e) < 0) dn.push(e);
        }
      }
    }
    if (adopted) document.adoptedStyleSheets = adopted;
    if (pv) el.style.setProperty('pointer-events', pv, pp);
    else el.style.removeProperty('pointer-events');
    return { up: up, dn: dn };
  }
  // scale of an element (pages that shrink via transform: scale())
  function scaleOf(e) {
    for (var a = e; a && a.nodeType === 1; a = a.parentElement) {
      if (a.offsetWidth > 0 && a.offsetHeight > 0) {
        var q = a.getBoundingClientRect();
        return [q.width / a.offsetWidth || 1, q.height / a.offsetHeight || 1];
      }
    }
    return [1, 1];
  }
  // clip-path: polygon around everything with the video rectangle as the hole (evenodd);
  // the coordinates may reach far beyond the element's box
  function clipOut(e, r) {
    var q = e.getBoundingClientRect(), k = scaleOf(e), B = 100000;
    var x1 = (r.left - q.left) / k[0], y1 = (r.top - q.top) / k[1];
    var x2 = x1 + r.width / k[0], y2 = y1 + r.height / k[1];
    var pts = [[-B, -B], [B, -B], [B, B], [-B, B], [-B, -B], [x1, y1], [x2, y1], [x2, y2], [x1, y2], [x1, y1]];
    setI(e, 'clip-path', 'polygon(evenodd,' + pts.map(function (p) { return p[0].toFixed(1) + 'px ' + p[1].toFixed(1) + 'px'; }).join(',') + ')');
  }
  function frame(e, b, r) {
    var q = e.getBoundingClientRect();
    var hx = Math.max(0, r.left - q.left), hy = Math.max(0, r.top - q.top);
    var hx2 = Math.min(q.width, r.left + r.width - q.left), hy2 = Math.min(q.height, r.top + r.height - q.top);
    if (hx2 <= hx || hy2 <= hy) return;
    var g = 'linear-gradient(' + b.color + ',' + b.color + ')', W = q.width, H = q.height;
    setI(e, 'background-color', 'transparent');
    setI(e, 'background-image', [g, g, g, g].join(','));
    setI(e, 'background-repeat', 'no-repeat');
    setI(e, 'background-origin', 'border-box');
    setI(e, 'background-clip', 'border-box');
    setI(e, 'background-position', '0 0,0 ' + hy2 + 'px,0 ' + hy + 'px,' + hx2 + 'px ' + hy + 'px');
    setI(e, 'background-size', W + 'px ' + hy + 'px,' + W + 'px ' + (H - hy2) + 'px,' + hx + 'px ' + (hy2 - hy) + 'px,' +
         (W - hx2) + 'px ' + (hy2 - hy) + 'px');
  }
  function holeBelow(el, r) {
    if (window.__tvfNoHoleBelow) { if (below.length) unholeJS(); return; }
    var key = [r.left, r.top, r.width, r.height].map(Math.round).join(',');
    if (key === lastKey && ++tick % 4) return;
    lastKey = key;
    tick = 0;
    var s = sample(el, r), now = [], mode = [];
    for (var n = 0; n < s.dn.length; n++) {
      var t = s.dn[n], md = 'clip';
      if (within(t, s.up)) md = 'frame';
      else while (t.parentElement && !t.parentElement.contains(el) && !within(t.parentElement, s.up)) t = t.parentElement;
      if (now.indexOf(t) < 0) { now.push(t); mode.push(md); }
    }
    for (var i = below.length - 1; i >= 0; i--) {
      var x = now.indexOf(below[i].el);
      if (x < 0 || mode[x] !== below[i].mode) { restore(below[i]); below.splice(i, 1); }
    }
    for (var m = 0; m < now.length; m++) {
      var e = now[m], b = null;
      for (var z = 0; z < below.length; z++) if (below[z].el === e) b = below[z];
      if (!b) {
        var cs = getComputedStyle(e);
        // without the children, only a solid-color background can be cut out
        if (mode[m] === 'frame' && !(cs.backgroundColor !== 'rgba(0, 0, 0, 0)' && cs.backgroundColor !== 'transparent' &&
                                     cs.backgroundImage === 'none')) continue;
        b = { el: e, saved: save(e, mode[m] === 'clip' ? CLIP : FRAME), color: cs.backgroundColor, mode: mode[m] };
        below.push(b);
      }
      if (b.mode === 'clip') clipOut(e, r);
      else frame(e, b, r);
    }
  }

  // No MSE in this WebKit: players are meant to use src. dash.js shim for
  // the OIPF polyfill (A/V objects with DASH): the MPD directly as src
  if (!SEL) try { window.MediaSource = undefined; window.WebKitMediaSource = undefined; window.ManagedMediaSource = undefined; } catch (e) { /* ignore */ }
  // dash.js shim, locked against overwriting: pages load their own
  // dash.js afterwards (ARD), which plays nothing without MSE - their calls end up here
  var MP = function () {
    return {
      create: function () {
        var v = null, url = '', auto = true, start = 0, cbs = {};
        function go() {
          if (!v || !url) return;
          upgrade(v); /* element possibly not upgraded yet (innerHTML) */
          v.src = url;
          if (start > 0) st(v).time = start; /* after src: setSrc resets the time */
          if (auto) v.play();
        }
        function emit(n, d) { (cbs[n] || []).slice().forEach(function (f) { try { f(d || { type: n }); } catch (e) { /* ignore */ } }); }
        var p = {
          initialize: function (view, src, ap) {
            if (view) v = view;
            if (src) url = String(src);
            if (ap !== undefined) auto = !!ap;
            if (v) {
              ['playing', 'pause', 'ended', 'error', 'timeupdate', 'loadedmetadata', 'canplay', 'seeked'].forEach(function (n) {
                v.addEventListener(n, function () {
                  var map = { playing: 'playbackPlaying', pause: 'playbackPaused', ended: 'playbackEnded', error: 'error',
                              timeupdate: 'playbackTimeUpdated', loadedmetadata: 'playbackMetaDataLoaded',
                              canplay: 'canPlay', seeked: 'playbackSeeked' };
                  emit(map[n], n === 'error' ? { type: 'error', error: { code: 27, message: 'TVF' } } : null);
                  if (n === 'loadedmetadata') { emit('streamInitialized'); emit('manifestLoaded'); }
                });
              });
            }
            go();
          },
          attachView: function (view) { v = view; },
          attachSource: function (src, t) { url = String(src || ''); if (t) start = Number(t) || 0; go(); },
          setAutoPlay: function (b) { auto = !!b; },
          getAutoPlay: function () { return auto; },
          getVideoElement: function () { return v; },
          play: function () { if (v) v.play(); },
          pause: function () { if (v) v.pause(); },
          isPaused: function () { return v ? v.paused : true; },
          seek: function (t) { if (v) v.currentTime = t; },
          time: function () { return v ? v.currentTime : 0; },
          duration: function () { return v ? v.duration : NaN; },
          isReady: function () { return !!v; },
          isSeeking: function () { return v ? v.seeking : false; },
          isDynamic: function () { return false; },
          setMute: function (m) { if (v) v.muted = !!m; },
          isMuted: function () { return v ? v.muted : false; },
          setVolume: function (x) { if (v) v.volume = x; },
          getVolume: function () { return v ? v.volume : 1; },
          reset: function () { if (v) v.removeAttribute('src'); v = null; url = ''; },
          destroy: function () { p.reset(); },
          on: function (n, f) { (cbs[n] = cbs[n] || []).push(f); },
          off: function (n, f) { cbs[n] = (cbs[n] || []).filter(function (g) { return g !== f; }); },
          updateSettings: function () {}, getSettings: function () { return { streaming: {} }; },
          getVersion: function () { return '4.7.4-tvf'; },
          getBitrateInfoListFor: function () { return []; }, getTracksFor: function () { return []; },
          getCurrentTrackFor: function () { return null; }, setCurrentTrack: function () {},
          setTextTrack: function () {}, enableText: function () {}, getDashMetrics: function () { return null; },
          getActiveStream: function () { return null; }, getSource: function () { return url; },
          setInitialMediaSettingsFor: function () {},
          // the page's DRM data: PlayReady license server -> TVF (override at the TV's DRM client)
          setProtectionData: function (pd) {
            try { post({ type: 'tvf-drm', op: 'protection', data: JSON.parse(JSON.stringify(pd || {})) }); } catch (e) { /* ignore */ }
          }
        };
        return p;
      }
    };
  };
  MP.events = { ERROR: 'error', PLAYBACK_ENDED: 'playbackEnded', PLAYBACK_PLAYING: 'playbackPlaying',
                PLAYBACK_PAUSED: 'playbackPaused', PLAYBACK_TIME_UPDATED: 'playbackTimeUpdated',
                PLAYBACK_METADATA_LOADED: 'playbackMetaDataLoaded', STREAM_INITIALIZED: 'streamInitialized',
                MANIFEST_LOADED: 'manifestLoaded', CAN_PLAY: 'canPlay', PLAYBACK_SEEKED: 'playbackSeeked',
                PLAYBACK_STARTED: 'playbackStarted', TEXT_TRACKS_ADDED: 'allTextTracksAdded' };
  MP.errors = {};
  var shim = { MediaPlayer: MP, Debug: { LOG_LEVEL_NONE: 0 }, supportsMediaSource: function () { return true; } };
  M.dashShim = MP; /* selective mode: drm.js uses it for A/V objects with PlayReady */
  if (!SEL) try {
    Object.defineProperty(window, 'dashjs', { configurable: false, get: function () { return shim; },
                                              set: function () { /* real dash.js without MSE: discarded */ } });
  } catch (e) { window.dashjs = shim; }

  // hls.js shim, locked likewise: hls.js needs MSE and reports
  // isSupported() = false here, players like hyoga (Tele 5) then get stuck. The
  // shim passes the playlist as src to the element - TVF plays it (its own
  // HLS player, or the TV's if that one cannot). Events like hls.js
  // (EventEmitter: on/once/off/emit, trigger(e, data) = emit(e, e, data)).
  var HE = { MEDIA_ATTACHING: 'hlsMediaAttaching', MEDIA_ATTACHED: 'hlsMediaAttached', MEDIA_DETACHING: 'hlsMediaDetaching',
             MEDIA_DETACHED: 'hlsMediaDetached', MANIFEST_LOADING: 'hlsManifestLoading', MANIFEST_LOADED: 'hlsManifestLoaded',
             MANIFEST_PARSED: 'hlsManifestParsed', LEVEL_SWITCHING: 'hlsLevelSwitching', LEVEL_SWITCHED: 'hlsLevelSwitched',
             LEVEL_LOADED: 'hlsLevelLoaded', LEVEL_UPDATED: 'hlsLevelUpdated', AUDIO_TRACKS_UPDATED: 'hlsAudioTracksUpdated',
             AUDIO_TRACK_SWITCHED: 'hlsAudioTrackSwitched', SUBTITLE_TRACKS_UPDATED: 'hlsSubtitleTracksUpdated',
             FRAG_LOADED: 'hlsFragLoaded', FRAG_CHANGED: 'hlsFragChanged', BUFFER_APPENDED: 'hlsBufferAppended',
             ERROR: 'hlsError', DESTROYING: 'hlsDestroying' };
  var HT = { NETWORK_ERROR: 'networkError', MEDIA_ERROR: 'mediaError', KEY_SYSTEM_ERROR: 'keySystemError',
             MUX_ERROR: 'muxError', OTHER_ERROR: 'otherError' };
  var HD = { MANIFEST_LOAD_ERROR: 'manifestLoadError', MANIFEST_PARSING_ERROR: 'manifestParsingError',
             LEVEL_LOAD_ERROR: 'levelLoadError', FRAG_LOAD_ERROR: 'fragLoadError', BUFFER_STALLED_ERROR: 'bufferStalledError' };
  function Hls(cfg) {
    var c = {}, k;
    for (k in Hls.DefaultConfig) c[k] = Hls.DefaultConfig[k];
    for (k in (cfg || {})) c[k] = cfg[k];
    this.config = c;
    this.userConfig = cfg || {};
    this.media = null;
    this.url = '';
    this.levels = [];
    this.currentLevel = this.nextLevel = this.loadLevel = this.startLevel = -1;
    this.autoLevelEnabled = true;
    this.audioTracks = [];
    this.audioTrack = -1;
    this.subtitleTracks = [];
    this.subtitleTrack = -1;
    this.audioTrackController = { audioTracks: [], audioTrack: -1 };
    this.subtitleTrackController = { subtitleTracks: [], subtitleTrack: -1 };
    this._l = {};
    this._go = '';
  }
  Hls.isSupported = Hls.isMSESupported = function () { return true; };
  Hls.getMediaSource = function () { return null; };
  Hls.version = '1.5.20-tvf';
  Hls.Events = HE;
  Hls.ErrorTypes = HT;
  Hls.ErrorDetails = HD;
  Hls.DefaultConfig = { autoStartLoad: true, startPosition: -1, debug: false, maxBufferLength: 30, maxMaxBufferLength: 600,
                        backBufferLength: Infinity, liveSyncDurationCount: 3, enableWorker: true, lowLatencyMode: false };
  var HP = Hls.prototype;
  HP.on = function (e, f, ctx) { (this._l[e] = this._l[e] || []).push([f, ctx, false]); return this; };
  HP.once = function (e, f, ctx) { (this._l[e] = this._l[e] || []).push([f, ctx, true]); return this; };
  HP.off = function (e, f) {
    if (!f) delete this._l[e];
    else this._l[e] = (this._l[e] || []).filter(function (x) { return x[0] !== f; });
    return this;
  };
  HP.removeAllListeners = function (e) { if (e) delete this._l[e]; else this._l = {}; };
  HP.listeners = function (e) { return (this._l[e] || []).map(function (x) { return x[0]; }); };
  HP.listenerCount = function (e) { return (this._l[e] || []).length; };
  HP.emit = function (e) {
    var a = [].slice.call(arguments, 1), self = this, l = (this._l[e] || []).slice();
    for (var i = 0; i < l.length; i++) {
      if (l[i][2]) self.off(e, l[i][0]);
      try { l[i][0].apply(l[i][1] || self, a); } catch (er) { /* like EventEmitter: carry on */ }
    }
    return l.length > 0;
  };
  HP.trigger = function (e, data) { return this.emit(e, e, data); };
  HP.__tvfLater = function (f) { var self = this; setTimeout(function () { f.call(self); }, 0); };
  HP.loadSource = function (u) {
    this.url = abs(u);
    this.__tvfLater(function () { this.trigger(HE.MANIFEST_LOADING, { url: this.url }); });
    this.__tvfStart();
  };
  HP.attachMedia = function (m) {
    this.media = m;
    this.trigger(HE.MEDIA_ATTACHING, { media: m });
    this.__tvfLater(function () { if (this.media === m) this.trigger(HE.MEDIA_ATTACHED, { media: m }); });
    this.__tvfStart();
  };
  // playlist and element known: set src, report manifest events
  HP.__tvfStart = function () {
    var v = this.media, self = this;
    if (!v || !this.url || this._go === this.url) return;
    this._go = this.url;
    upgrade(v); /* element possibly not upgraded yet (innerHTML) */
    v.__tvfHls = this;
    if (!v.__tvfHlsErr) {
      v.__tvfHlsErr = true;
      v.addEventListener('error', function () {
        var h = v.__tvfHls;
        if (h) h.trigger(HE.ERROR, { type: HT.NETWORK_ERROR, details: HD.MANIFEST_LOAD_ERROR, fatal: true, url: h.url });
      });
    }
    v.src = this.url;
    if (this.config.startPosition > 0) st(v).time = this.config.startPosition;
    if (this.config.autoStartLoad !== false) preloadMeta(v, this.url); /* like hls.js, also with preload=none */
    var lv = [{ bitrate: 0, width: 0, height: 0, name: 'tvf', url: [this.url], attrs: {}, details: null }];
    this.levels = lv;
    this.__tvfLater(function () {
      if (self._go !== self.url) return;
      self.trigger(HE.MANIFEST_LOADED, { levels: lv, audioTracks: [], subtitles: [], url: self.url, stats: {},
                                         networkDetails: null, sessionData: null });
      self.trigger(HE.MANIFEST_PARSED, { levels: lv, audioTracks: [], subtitleTracks: [], firstLevel: 0, stats: {},
                                         audio: true, video: true, altAudio: false });
      self.currentLevel = self.loadLevel = self.nextLevel = 0;
      self.trigger(HE.LEVEL_SWITCHED, { level: 0 });
    });
  };
  HP.startLoad = function () { this.__tvfStart(); };
  HP.stopLoad = HP.recoverMediaError = HP.swapAudioCodec = HP.removeLevel = function () {};
  HP.detachMedia = function () {
    var v = this.media;
    if (!v) return;
    this.trigger(HE.MEDIA_DETACHING, {});
    if (v.__tvfHls === this) {
      v.__tvfHls = null;
      v.removeAttribute('src'); /* stops playback in TVF */
    }
    this.media = null;
    this._go = '';
    this.trigger(HE.MEDIA_DETACHED, {});
  };
  HP.destroy = function () {
    this.trigger(HE.DESTROYING, {});
    this.detachMedia();
    this._l = {};
    this.url = '';
  };
  if (!SEL) try {
    Object.defineProperty(window, 'Hls', { configurable: false, get: function () { return Hls; },
                                           set: function () { /* real hls.js without MSE: discarded */ } });
  } catch (e) { window.Hls = Hls; }
})();
