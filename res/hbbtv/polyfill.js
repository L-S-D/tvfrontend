// SPDX-License-Identifier: GPL-3.0-or-later
// HbbTV/OIPF polyfill for the HbbTV apps in TVF's own renderer: TVF injects
// it as a user script before the page's scripts (src/app/hbbweb.c),
// dash.js for the A/V objects is served by TVF's relay
// (__HBBTV_FWD.dash, 127.0.0.1). Emulates the plugin objects that a
// normal browser lacks:
//   application/oipfApplicationManager  getOwnerApplication() -> Application
//                                        (keyset, show/hide, create/destroy)
//   video/broadcast                      bindToCurrentChannel, setFullScreen,
//                                        currentChannel - TVF shows the picture
//                                        itself beneath the page
//   application/oipfConfiguration        configuration / localSystem
//   A/V control (video/mp4, DASH, HLS)   play/stop/seek/playState via a
//                                        <video> next to the object
// Keys arrive via postMessage from the app ({type:'key', keyCode}), the
// position of the TV picture and "exit app" go back via postMessage.
// Channel info: window.name (JSON, set by the app).
(function () {
  'use strict';
  // forward proxy (hbbtv-fwd-proxy.js): the page has its real address,
  // nothing is rewritten - the redirections below are skipped
  var FWD = !!window.__HBBTV_FWD;
  var parentWin = window.parent;
  function post(msg) {
    try { parentWin.postMessage(msg, '*'); } catch (e) { /* ignore */ }
  }
  // Chromium shows "Couldn't load plugin" for unknown plugin objects:
  // the OIPF objects invisible (their position stays measurable); A/V objects
  // are shown by a <video> next to them (see below)
  try {
    var st = document.createElement('style');
    // HbbTV devices show no scrollbars
    st.textContent = 'object[type^="video/"],object[type^="audio/"],object[type^="application/"]' +
                     '{visibility:hidden !important}::-webkit-scrollbar{display:none}';
    (document.head || document.documentElement).appendChild(st);
  } catch (e) { /* ignore */ }

  // device detection of the apps (e.g. SAT.1/ProSieben): HbbTV in the User-Agent
  var ua = navigator.userAgent;
  if (!/HbbTV\//.test(ua)) {
    ua += ' HbbTV/1.5.1 (+DRM; LGE; webOS; WEBOS6.0; 1; ;)';
    try { Object.defineProperty(navigator, 'userAgent', { get: function () { return ua; } }); } catch (e) { /* ignore */ }
  }

  // channel: inserted by the gateway (name, number, DVB ID onid/tsid/sid -
  // broadcaster apps choose their content by it, e.g. the ARD start bar),
  // otherwise from window.name (set by the app, some pages clear it)
  var channel = {};
  if (FWD && window.__HBBTV_FWD.channel) channel = window.__HBBTV_FWD.channel;
  else {
    try { channel = JSON.parse(window.name || '{}'); } catch (e) { channel = {}; }
  }

  // ---- keys ----
  var K = {
    VK_RED: 403, VK_GREEN: 404, VK_YELLOW: 405, VK_BLUE: 406,
    VK_UP: 38, VK_DOWN: 40, VK_LEFT: 37, VK_RIGHT: 39, VK_ENTER: 13, VK_BACK: 461,
    VK_PLAY: 415, VK_PAUSE: 19, VK_PLAY_PAUSE: 463, VK_STOP: 413, VK_FAST_FWD: 417, VK_REWIND: 412,
    VK_PAGE_UP: 33, VK_PAGE_DOWN: 34, VK_INFO: 457
  };
  for (var d = 0; d <= 9; d++) K['VK_' + d] = 48 + d;
  window.KeyEvent = window.KeyEvent || {};
  for (var k in K) if (!(k in window.KeyEvent)) window.KeyEvent[k] = K[k];
  for (var k2 in K) if (!(k2 in window)) window[k2] = K[k2];

  var keyset = {
    RED: 0x1, GREEN: 0x2, YELLOW: 0x4, BLUE: 0x8, NAVIGATION: 0x10, VCR: 0x20,
    SCROLL: 0x40, INFO: 0x80, NUMERIC: 0x100, ALPHA: 0x200, OTHER: 0x400,
    value: 0x1,
    setValue: function (v) { this.value = v; this.set = true; post({ type: 'hbbtv-keyset', value: v }); },
    getKey: function () { return null; }
  };
  // pointer control (OIPF DAE 7.2.5.2): the page sets supportsPointer = true
  // when it handles mouse events - only then does TVF pass the Magic Remote's
  // pointer and wheel on as a mouse (otherwise click = OK, wheel = up/down)
  var pointer = false;
  try {
    Object.defineProperty(keyset, 'supportsPointer', {
      enumerable: true,
      get: function () { return pointer; },
      set: function (v) {
        v = !!v;
        if (v === pointer) return;
        pointer = v;
        post({ type: 'hbbtv-pointer', on: v });
      }
    });
  } catch (e) { keyset.supportsPointer = false; }

  // key/code like a real key press (pages that check event.key)
  var KEY_NAMES = { 13: 'Enter', 37: 'ArrowLeft', 38: 'ArrowUp', 39: 'ArrowRight', 40: 'ArrowDown',
    461: 'GoBack', 403: 'ColorF0Red', 404: 'ColorF1Green', 405: 'ColorF2Yellow', 406: 'ColorF3Blue',
    415: 'MediaPlay', 19: 'MediaPause', 413: 'MediaStop', 417: 'MediaFastForward', 412: 'MediaRewind',
    463: 'MediaPlayPause', 457: 'Info' };
  function keyEvent(type, code) {
    var name = KEY_NAMES[code] || (code >= 48 && code <= 57 ? String(code - 48) : '');
    var ev;
    try {
      ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key: name,
        code: code >= 48 && code <= 57 ? 'Digit' + name : name });
    } catch (e) {
      ev = document.createEvent('KeyboardEvent');
      ev.initEvent(type, true, true);
    }
    return ev;
  }

  function sendKey(code) {
    var ev = keyEvent('keydown', code);
    try { Object.defineProperty(ev, 'keyCode', { get: function () { return code; } }); } catch (e) { /* ignore */ }
    try { Object.defineProperty(ev, 'which', { get: function () { return code; } }); } catch (e) { /* ignore */ }
    ev.__hbbtvSynthetic = true;
    var target = document.activeElement || document.body || document;
    target.dispatchEvent(ev);
    // a synthetic Enter triggers no default action: click the focused
    // button/link if the page has not handled it itself
    if (code === 13 && !ev.defaultPrevented && target && target.click &&
        /^(BUTTON|A|INPUT|SELECT|LABEL)$/.test(target.tagName))
      target.click();
    try {
      var up = keyEvent('keyup', code);
      Object.defineProperty(up, 'keyCode', { get: function () { return code; } });
      Object.defineProperty(up, 'which', { get: function () { return code; } });
      up.__hbbtvSynthetic = true;
      target.dispatchEvent(up);
    } catch (e) { /* ignore */ }
  }

  // Real key presses with the focus in this page (it navigates with
  // DOM focus, e.g. Tele 5 consent): they stay here, with the browser's default
  // actions - except for the TV's keys (CH+/-, Stop, Back without a
  // keyset, everything outside the keyset): those go to the webOS app
  function keyMask(k) {
    return k === 403 ? 0x1 : k === 404 ? 0x2 : k === 405 ? 0x4 : k === 406 ? 0x8
      : (k >= 37 && k <= 40) || k === 13 || k === 461 ? 0x10
      : k === 415 || k === 19 || k === 413 || k === 417 || k === 412 || k === 463 ? 0x20
      : k === 457 ? 0x80
      : k >= 48 && k <= 57 ? 0x100 : 0x400;
  }
  function hostKey(k) {
    if (k === 33 || k === 34 || k === 427 || k === 428 || k === 413) return true;
    if (!keyset.set) return k === 461;
    return (keyset.value & keyMask(k)) === 0;
  }
  if (window.top !== window) {
    // emergency exit: Back three times within 2 s ends the HbbTV app, even if the
    // page handles Back itself and hangs (Tele 5 player after an error);
    // the first two go to the page as usual
    var backTimes = [];
    window.addEventListener('keydown', function (e) {
      if (e.keyCode !== 461 || e.__hbbtvSynthetic) return;
      var now = Date.now();
      backTimes = backTimes.filter(function (t) { return now - t < 2000; });
      backTimes.push(now);
      if (backTimes.length < 3) return;
      backTimes = [];
      e.preventDefault();
      e.stopImmediatePropagation();
      try { window.top.postMessage({ type: 'hbbtv-escape' }, '*'); } catch (x) { /* ignore */ }
    }, true);
    window.addEventListener('keydown', function (e) {
      if (e.__hbbtvSynthetic || !hostKey(e.keyCode)) return;
      e.preventDefault();
      e.stopImmediatePropagation();
      try { window.top.postMessage({ type: 'hbbtv-hostkey', keyCode: e.keyCode }, '*'); } catch (x) { /* ignore */ }
    }, true);
  }

  window.addEventListener('message', function (e) {
    var m = e.data;
    if (m && m.type === 'key') sendKey(m.keyCode);
  });

  // absolute URL of another server -> /hbbtv/<scheme>/<host>/...
  function viaProxy(u) {
    try {
      var x = new URL(u, document.baseURI);
      // proxy URLs "normalized" to https by apps: "https://" +
      // "/hbbtv/https/host/..." and https to the proxy itself
      if (x.hostname === 'hbbtv' && /^\/https?\//.test(x.pathname))
        return '/hbbtv' + x.pathname + x.search + x.hash;
      if (x.host === window.location.host && x.protocol !== window.location.protocol)
        return x.pathname + x.search + x.hash;
      if (/^https?:$/.test(x.protocol) && x.origin !== window.location.origin)
        return '/hbbtv/' + x.protocol.replace(':', '') + '/' + x.host + x.pathname + x.search + x.hash;
      return x.href;
    } catch (e) {
      return u;
    }
  }

  // Real server of this page (from /hbbtv/<scheme>/<host>/...; without
  // prefix: the remembered one). In the HbbTV main frame the prefix is removed from
  // the address before the app scripts run: single-page apps route via
  // location.pathname (new ZDF app: React Router). The proxy then maps
  // root-relative requests to the server via the hbbtvup cookie.
  var upstream = null;
  (function () {
    if (FWD) return;
    var m = /^\/hbbtv\/(https?)\/([^\/]+)(\/.*)?$/.exec(window.location.pathname);
    // main frame: directly beneath the webOS app (or alone in the browser);
    // window.name is no good, apps reset it (ZDF Start)
    var appFrame = window.parent === window.top;
    try {
      if (m) sessionStorage.setItem('hbbtvUpstream', m[1] + '|' + m[2]);
      else m = [null].concat((sessionStorage.getItem('hbbtvUpstream') || '').split('|'));
    } catch (e) { /* no storage */ }
    if (m && m[1] && m[2]) upstream = { protocol: m[1] + ':', host: m[2] };
    if (upstream && appFrame) {
      document.cookie = 'hbbtvup=' + encodeURIComponent(m[1] + '|' + m[2]) + '; path=/';
      if (m[0] !== null) {   // prefix was in the path
        try {
          history.replaceState(history.state, '', (m[3] || '/') + window.location.search + window.location.hash);
        } catch (e) { /* address stays */ }
      }
    }
  })();

  // target of the location accesses rewritten by the proxy
  var L = window.location;
  window.__hbbtvLoc = {
    get href() { return L.href; },
    set href(u) { L.href = viaProxy(String(u)); },
    replace: function (u) { L.replace(viaProxy(String(u))); },
    assign: function (u) { L.assign(viaProxy(String(u))); },
    reload: function () { L.reload(); },
    get protocol() { return upstream ? upstream.protocol : L.protocol; },
    get host() { return upstream ? upstream.host : L.host; },
    get hostname() { return upstream ? upstream.host.replace(/:\d+$/, '') : L.hostname; },
    get port() { var p = upstream ? /:(\d+)$/.exec(upstream.host) : null; return upstream ? (p ? p[1] : '') : L.port; },
    get origin() { return L.origin; },
    get pathname() { return L.pathname; },
    get search() { return L.search; },
    get hash() { return L.hash; },
    set hash(h) { L.hash = h; },
    toString: function () { return L.href; }
  };

  // postMessage to this window (the function belongs to the receiver): it
  // comes via the proxy, its origin is the proxy's. A target other
  // than "*" - the real server (location.protocol + "//" + location.host),
  // an address rewritten by the proxy ("/hbbtv/https/app.usercentrics.eu/
  // ...") - was discarded by the browser: ORF player, Usercentrics consent
  (function () {
    if (FWD) return;
    var pm = window.postMessage;
    window.postMessage = function (msg, target) {
      var args = Array.prototype.slice.call(arguments);
      if (typeof target === 'string' && target !== '*' && target !== window.location.origin) {
        var o = null;
        try { o = new URL(target, document.baseURI).origin; } catch (e) { o = null; }
        if (o !== window.location.origin) args[1] = window.location.origin;
      }
      return pm.apply(window, args);
    };
  })();

  // XHR/fetch/sendBeacon to other servers (URLs assembled at runtime, e.g. from
  // location.origin): via the proxy, otherwise CORS/mixed content
  (function () {
    if (FWD) return;
    var open = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function (method, url) {
      var args = Array.prototype.slice.call(arguments);
      args[1] = viaProxy(String(url));
      return open.apply(this, args);
    };
    if (navigator.sendBeacon) {
      var beacon = navigator.sendBeacon;
      navigator.sendBeacon = function (url, data) { return beacon.call(navigator, viaProxy(String(url)), data); };
    }
    if (window.fetch) {
      var f = window.fetch;
      window.fetch = function (input, init) {
        if (typeof input === 'string') input = viaProxy(input);
        else if (input && input.url && window.Request && input instanceof window.Request) {
          var u = viaProxy(input.url);
          if (u !== input.url) input = new window.Request(u, input);
        }
        return f.call(window, input, init);
      };
    }
  })();

  // ---- Application Manager ----
  var app = {
    keyset: keyset,
    visible: false,
    privateData: { keyset: keyset, currentChannel: null },
    show: function () { this.visible = true; post({ type: 'hbbtv-show' }); return true; },
    hide: function () { this.visible = false; post({ type: 'hbbtv-hide' }); return true; },
    createApplication: function (url) {
      // dvb://current.ait/<org>.<app>: an app from the broadcaster's AIT - the
      // webOS app resolves and loads it. If it is not in the AIT, the
      // launch fails (null) as on a TV: the ARD start bar then loads the
      // section itself via http (Film/Serien Highlights: 13.14 is missing)
      var m = /^dvb:\/\/current\.ait\/([0-9a-f]+)\.([0-9a-f]+)/i.exec(String(url));
      if (m && FWD && window.__HBBTV_FWD.channel && window.__HBBTV_FWD.channel.ait) {
        var org = parseInt(m[1], 16), id = parseInt(m[2], 16);
        var known = window.__HBBTV_FWD.channel.ait.some(function (a) { return a.org === org && a.app === id; });
        if (!known) return null;
      }
      // another HbbTV app (e.g. a catch-up service): in the same iframe, via the
      // proxy (otherwise this polyfill is missing there)
      post({ type: 'hbbtv-launch', url: String(url) });
      if (m || /^dvb:/i.test(String(url))) return app;
      window.location.href = FWD ? String(url) : viaProxy(String(url));
      return app;
    },
    destroyApplication: function () { post({ type: 'hbbtv-exit' }); },
    activate: function () {}, deactivate: function () {},
    addEventListener: function () {}, removeEventListener: function () {}
  };
  app['private'] = app.privateData;

  // ---- video/broadcast ----
  var ch = {
    channelType: 0, idType: 15, name: channel.name || '', ccid: 'ccid:' + (channel.sid || 0),
    onid: channel.onid || 0, tsid: channel.tsid || 0, sid: channel.sid || 0, nid: channel.onid || 0,
    dsd: '', majorChannel: channel.number || 0
  };
  app.privateData.currentChannel = ch;

  // The TV picture lies beneath the whole iframe; an HbbTV device punches a
  // hole into the graphics at the position of the video/broadcast object. Here:
  //  - ancestors with the same area (black letterbox, SAT.1): background
  //    transparent
  //  - larger ancestors with a background (gradient across the whole page): the
  //    background moves to a fixed copy behind them (z-index -1) with
  //    a hole (mask) at the object's position; children and layout stay
  var BG_PROPS = ['backgroundColor', 'backgroundImage', 'backgroundSize', 'backgroundPosition', 'backgroundRepeat'];
  function punchHole(el, r) {
    for (var p = el.parentElement; p; p = p.parentElement) {
      var cs = getComputedStyle(p), h = p.__hbbtvHole;
      var hasBg = cs.backgroundColor !== 'rgba(0, 0, 0, 0)' || cs.backgroundImage !== 'none';
      if (!hasBg && !h) continue;
      var q = p.getBoundingClientRect();
      if (!h && q.width <= r.width + 4 && q.height <= r.height + 4) {
        p.style.background = 'transparent';
        continue;
      }
      if (!h) {
        h = p.__hbbtvHole = document.createElement('div');
        var st = 'position:fixed;z-index:-1;pointer-events:none;';
        h.style.cssText = st;
        for (var i = 0; i < BG_PROPS.length; i++) h.style[BG_PROPS[i]] = cs[BG_PROPS[i]];
        p.style.background = 'transparent';
      }
      if (!h.isConnected) (document.body || document.documentElement).appendChild(h);
      var s = h.style;
      s.left = q.left + 'px';
      s.top = q.top + 'px';
      s.width = q.width + 'px';
      s.height = q.height + 'px';
      var m = 'linear-gradient(#000,#000),linear-gradient(#000,#000)';
      s.webkitMaskImage = s.maskImage = m;
      s.webkitMaskRepeat = s.maskRepeat = 'no-repeat';
      s.webkitMaskSize = s.maskSize = '100% 100%,' + r.width + 'px ' + r.height + 'px';
      s.webkitMaskPosition = s.maskPosition = '0 0,' + (r.left - q.left) + 'px ' + (r.top - q.top) + 'px';
      s.webkitMaskComposite = 'xor';
      s.maskComposite = 'exclude';
    }
  }

  function videoRect(el) {
    try {
      var r = el.getBoundingClientRect();
      punchHole(el, r);
      post({ type: 'hbbtv-video', x: r.left, y: r.top, w: r.width, h: r.height,
             vw: window.innerWidth, vh: window.innerHeight });
    } catch (e) { /* ignore */ }
  }

  // ---- A/V control objects (OIPF DAE 7.14) ----
  // Catch-up services and ad spots play via <object type="video/mp4|
  // application/dash+xml|...">: ZDF-Mediathek, RTL player (HbbTV 1.5),
  // SAT.1 spot overlay. Emulated with a <video> as a sibling of the
  // object (same position, same stacking context). The object itself stays
  // invisible and loads nothing: data is intercepted (property, attribute,
  // parser/innerHTML via MutationObserver). DASH via dash.js (relay).
  var AV_TYPE = /^(video\/(mp4|mpeg|mpeg4|mp2t|x-mpegurl)|audio\/(mp4|mpeg|mp3|x-mpegurl)|application\/(dash\+xml|vnd\.apple\.mpegurl|x-mpegurl))\s*(;.*)?$/i;
  var nativeGetAttr = Element.prototype.getAttribute;
  var nativeSetAttr = Element.prototype.setAttribute;
  var nativeRemoveAttr = Element.prototype.removeAttribute;
  var avEls = [];

  function isAv(el) {
    return !!el && el.tagName === 'OBJECT' && AV_TYPE.test(nativeGetAttr.call(el, 'type') || '');
  }

  function avOf(el) { return el.__av && isAv(el) ? el.__av : null; }

  function av(el) {
    if (!el.__av) {
      el.__av = { url: '', state: 0, speed: 0, error: undefined, video: null, dash: null,
                  loaded: false, loadId: 0, full: false, timer: null };
      avEls.push(el);
    }
    return el.__av;
  }

  function later(fn) { setTimeout(function () { try { fn(); } catch (e) { /* app error */ } }, 0); }

  function fire(el, name, prop, value) {
    later(function () {
      var h = el['on' + name];
      if (typeof h === 'function') h.call(el, value);
    });
    later(function () {
      var ev = document.createEvent('Event');
      ev.initEvent(name, false, false);
      ev[prop] = value;
      el.dispatchEvent(ev);
    });
  }

  function setState(el, s, err) {
    var a = el.__av;
    if (err !== undefined) a.error = err;
    if (a.state === s) return;
    a.state = s;
    fire(el, 'PlayStateChange', 'state', s);
    mediaCheck();
  }

  function setUrl(el, v) {
    var a = av(el), url = v === null || v === undefined ? '' : String(v);
    if (url) { try { url = new URL(url, document.baseURI).href; } catch (e) { /* as given */ } }
    if (url === a.url) return;
    if (a.state !== 0) avStop(el);
    a.url = url;
  }

  // object from parser/innerHTML/.type=: take over the data attribute before
  // the browser loads it as a plugin
  function adopt(el) {
    if (!isAv(el)) return;
    var d = nativeGetAttr.call(el, 'data');
    av(el);
    if (d !== null) {
      nativeRemoveAttr.call(el, 'data');
      setUrl(el, d);
    }
  }

  function place(el) {
    var a = el.__av, v = a.video;
    if (!v) return;
    if (!el.isConnected) {
      avStop(el);
      if (v.parentNode) v.parentNode.removeChild(v);
      clearInterval(a.timer);
      a.timer = null;
      return;
    }
    if (v.parentNode !== el.parentNode || v.previousSibling !== el) el.parentNode.insertBefore(v, el.nextSibling);
    var shown = a.loaded && el.getClientRects().length > 0 && el.style.visibility !== 'hidden';
    var s = v.style;
    s.display = shown ? '' : 'none';
    if (a.full) {
      s.position = 'fixed';
      s.left = s.top = '0px';
      s.width = '100%';
      s.height = '100%';
    } else {
      s.position = 'absolute';
      s.left = el.offsetLeft + 'px';
      s.top = el.offsetTop + 'px';
      s.width = el.offsetWidth + 'px';
      s.height = el.offsetHeight + 'px';
    }
    var z = '';
    try { z = getComputedStyle(el).zIndex; } catch (e) { z = ''; }
    s.zIndex = z && z !== 'auto' ? z : '';
  }

  function mediaError(v) {
    // OIPF error: 0 format not supported, 1 no connection, 2 other error
    var c = v.error ? v.error.code : 0;
    return c === 4 ? 0 : c === 2 ? 1 : 2;
  }

  function ensureVideo(el) {
    var a = av(el);
    if (!a.video) {
      var v = document.createElement('video');
      nativeSetAttr.call(v, 'data-hbbtv-av', '1');
      v.style.cssText = 'position:absolute;background:#000;pointer-events:none;object-fit:contain;display:none';
      v.controls = false;
      v.autoplay = false;
      v.addEventListener('playing', function () { if (a.loaded && a.speed !== 0) setState(el, 1); });
      v.addEventListener('waiting', function () { if (a.state === 1) setState(el, 4); });
      v.addEventListener('canplay', function () { if (a.loaded && a.speed === 0 && a.state === 3) setState(el, 2); });
      v.addEventListener('ended', function () { if (a.loaded) { a.speed = 0; setState(el, 5); } });
      v.addEventListener('error', function () { if (a.loaded && v.error) setState(el, 6, mediaError(v)); });
      v.addEventListener('seeked', function () {
        if (a.loaded) fire(el, 'PlayPositionChanged', 'position', Math.floor(v.currentTime * 1000));
      });
      a.video = v;
    }
    if (!a.timer) a.timer = setInterval(function () { place(el); }, 250);
    place(el);
    return a.video;
  }

  var dashWait = null;
  function withDash(cb) {
    if (window.dashjs && window.dashjs.MediaPlayer) { cb(true); return; }
    if (dashWait) { dashWait.push(cb); return; }
    dashWait = [cb];
    // dash.js is UMD: if the page uses AMD/CommonJS, there is no window.dashjs
    var saved = { define: window.define, exports: window.exports, module: window.module };
    window.define = window.exports = window.module = undefined;
    var s = document.createElement('script');
    function done(ok) {
      window.define = saved.define;
      window.exports = saved.exports;
      window.module = saved.module;
      var w = dashWait;
      dashWait = null;
      for (var i = 0; i < w.length; i++) w[i](ok && !!window.dashjs);
    }
    s.onload = function () { done(true); };
    s.onerror = function () { done(false); };
    s.src = FWD ? window.__HBBTV_FWD.dash : '/hbbtv-dash.js';
    (document.head || document.documentElement).appendChild(s);
  }

  function unload(a) {
    a.loadId++;
    a.loaded = false;
    if (a.dash) {
      try { a.dash.reset(); } catch (e) { /* ignore */ }
      a.dash = null;
    }
    if (a.video) {
      try { a.video.pause(); } catch (e) { /* ignore */ }
      if (a.video.hasAttribute('src')) {
        nativeRemoveAttr.call(a.video, 'src');
        try { a.video.load(); } catch (e) { /* ignore */ }
      }
    }
  }

  function load(el) {
    var a = el.__av, v = a.video, id;
    unload(a);
    a.loaded = true;
    id = a.loadId;
    if (/dash\+xml/i.test(nativeGetAttr.call(el, 'type') || '') || /\.mpd(\?|$)/i.test(a.url)) {
      withDash(function (ok) {
        if (id !== a.loadId) return;       // stopped in the meantime
        if (!ok) { setState(el, 6, 2); return; }
        try {
          a.dash = window.dashjs.MediaPlayer().create();
          a.dash.on(window.dashjs.MediaPlayer.events.ERROR, function () {
            if (id === a.loadId) setState(el, 6, 1);
          });
          a.dash.initialize(v, a.url, a.speed > 0);
        } catch (e) {
          setState(el, 6, 2);
        }
      });
    } else {
      v.src = a.url;
    }
  }

  function avPlay(el, speed) {
    adopt(el);   // innerHTML + immediate play(): the observer would come too late
    var a = av(el), v;
    speed = speed === undefined ? 1 : Number(speed);
    if (!a.url || isNaN(speed) || speed < 0) return false;   // no rewind
    v = ensureVideo(el);
    if (!a.loaded || a.state === 0 || a.state === 6) {
      a.speed = speed;
      load(el);
      setState(el, 3);
      place(el);
    } else if (a.state === 5) {
      v.currentTime = 0;
    }
    if (a.speed !== speed) {
      a.speed = speed;
      fire(el, 'PlaySpeedChanged', 'speed', speed);
    }
    if (speed === 0) {
      v.pause();
      if (a.state === 1 || a.state === 4) setState(el, 2);
    } else {
      v.playbackRate = Math.min(speed, 4);
      if (!a.dash && v.hasAttribute('src')) {
        var p = v.play();
        if (p && p.catch) p.catch(function () { /* the 'error' event reports it */ });
      } else if (a.dash) {
        try { a.dash.play(); } catch (e) { /* not ready yet: autoplay */ }
      }
    }
    return true;
  }

  function avStop(el) {
    var a = el.__av;
    if (!a) return true;
    unload(a);
    a.speed = 0;
    if (a.video) a.video.style.display = 'none';
    setState(el, 0);
    return true;
  }

  // Playback in the HbbTV app (A/V object or its own <video>, even
  // paused): the webOS app stops its TV picture meanwhile; "off" only after
  // 1 s of quiet (ad -> content)
  var mediaOn = false, mediaOffTimer = null;
  function mediaCheck() {
    var on = false, i, m, list = document.querySelectorAll('video,audio');
    for (i = 0; i < avEls.length && !on; i++) {
      m = avEls[i].__av;
      if (m.state >= 1 && m.state <= 4) on = true;
    }
    for (i = 0; i < list.length && !on; i++) {
      m = list[i];
      if ((m.currentSrc || m.src) && !m.ended && !m.error && m.getClientRects().length > 0) on = true;
    }
    if (on) {
      clearTimeout(mediaOffTimer);
      mediaOffTimer = null;
      if (!mediaOn) { mediaOn = true; post({ type: 'hbbtv-media', playing: true }); }
    } else if (mediaOn && !mediaOffTimer) {
      mediaOffTimer = setTimeout(function () {
        mediaOffTimer = null;
        mediaOn = false;
        post({ type: 'hbbtv-media', playing: false });
      }, 1000);
    }
  }
  ['play', 'playing', 'pause', 'ended', 'emptied', 'error'].forEach(function (n) {
    document.addEventListener(n, function () { setTimeout(mediaCheck, 0); }, true);
  });
  setInterval(mediaCheck, 1000);

  // Components of the TV picture (OIPF DAE 7.16.5 AVComponent): video, audio,
  // subtitles of the HTSP subscription with Component Tag (__HBBTV_FWD.channel.components,
  // tvh with HTSP extension). Selecting goes to TVF (this session only).
  var comps = (channel.components || []).map(function (c) {
    var o = { componentTag: c.tag, pid: c.index, type: c.kind, encoding: c.enc || '', encrypted: false };
    if (c.kind === 0) o.aspectRatio = 1.78;
    if (c.kind === 1) {
      o.language = c.lang || '';
      o.audioDescription = !!c.ad;
      o.audioChannels = c.ch || 2;
    }
    if (c.kind === 2) {
      o.language = c.lang || '';
      o.hearingImpaired = !!c.hi;
      o.label = '';
    }
    // internal, not enumerable: subtitle ID for TVF, active
    Object.defineProperty(o, '__sub', { value: c.sub || '', enumerable: false });
    Object.defineProperty(o, '__active', { value: !!c.active, enumerable: false, writable: true });
    return o;
  });
  function compList(a) {
    a.item = function (i) { return a[i]; };
    return a;
  }
  function ofType(type) {
    return comps.filter(function (c) { return type === undefined || type === null || c.type === Number(type); });
  }
  function components(el, type) {
    return compList(isAv(el) ? [] : ofType(type)); // A/V objects: media tracks are done by media.js
  }
  function activeComponents(el, type) {
    return compList(isAv(el) ? [] : ofType(type).filter(function (c) { return c.__active; }));
  }
  // selectComponent(AVComponent) or (type): audio switches, subtitles on;
  // unselectComponent: subtitles off, the audio stays (a device always plays one)
  function selectComponent(el, c, on) {
    if (isAv(el)) return;
    var t = null;
    if (c !== null && typeof c === 'object') {
      for (var i = 0; i < comps.length && !t; i++)
        if (comps[i].type === c.type && comps[i].pid === c.pid) t = comps[i];
    } else if (!on) {
      ofType(c).forEach(function (x) { if (x.type === 2 && x.__active) selectComponent(el, x, false); });
      return;
    }
    if (!t || t.type === 0 || (t.type === 1 && !on)) return;
    comps.forEach(function (x) { if (x.type === t.type) x.__active = on && x === t; });
    post({ type: 'hbbtv-component', kind: t.type, index: t.pid, sub: t.__sub, on: !!on });
    fire(el, 'SelectedComponentChanged', 'componentType', t.type);
  }

  new MutationObserver(function (ms) {
    for (var i = 0; i < ms.length; i++) {
      var m = ms[i];
      if (m.type === 'attributes') {
        if (m.target.tagName === 'OBJECT') adopt(m.target);
        continue;
      }
      for (var j = 0; j < m.addedNodes.length; j++) {
        var n = m.addedNodes[j];
        if (n.nodeType !== 1) continue;
        if (n.tagName === 'OBJECT') adopt(n);
        var inner = n.getElementsByTagName ? n.getElementsByTagName('object') : [];
        for (var k = 0; k < inner.length; k++) adopt(inner[k]);
      }
    }
  }).observe(document, { childList: true, subtree: true, attributes: true, attributeFilter: ['type', 'data'] });

  // ---- DSM-CC stream events (see addStreamEventListener) ----
  var seRegs = [], seDescs = {};
  function seLog(reg, state) {
    post({ type: 'hbbtv-streamevent', url: reg.url, name: reg.name, state: state,
           tag: reg.tag === null ? -1 : reg.tag, id: reg.id === null ? -1 : reg.id });
  }
  function seFire(reg, status, hex, text) {
    var ev = { type: 'StreamEvent', name: reg.name, data: hex || '', text: text || '', status: status };
    try { reg.listener.call(reg.obj, ev); } catch (e) { /* the app's own error */ }
  }
  function seFail(reg, why) {
    seLog(reg, why);
    setTimeout(function () {
      var i = seRegs.indexOf(reg);
      if (i < 0) return;
      seRegs.splice(i, 1); /* an error ends the registration */
      seFire(reg, 'error');
    }, 0);
  }
  // stream event description (TS 102 809 8.2): <dsmcc_object component_tag=..>
  // <stream_event stream_event_id=.. stream_event_name=../>, any namespace
  function seParse(doc) {
    var map = {}, all = doc ? doc.getElementsByTagName('*') : [];
    for (var i = 0; i < all.length; i++) {
      var o = all[i];
      if (o.localName !== 'dsmcc_object') continue;
      var tag = parseInt(o.getAttribute('component_tag'), 10), evs = o.getElementsByTagName('*');
      for (var k = 0; k < evs.length; k++) {
        if (evs[k].localName !== 'stream_event') continue;
        var name = evs[k].getAttribute('stream_event_name'), id = parseInt(evs[k].getAttribute('stream_event_id'), 10);
        if (name !== null && !isNaN(id)) map[name] = { tag: isNaN(tag) ? null : tag, id: id };
      }
    }
    return map;
  }
  function seLoad(url, done) {
    var d = seDescs[url];
    if (d && d.map) return done(d.map);
    if (d) return d.wait.push(done);
    d = seDescs[url] = { map: null, wait: [done] };
    function finish(map) {
      if (map) d.map = map; else delete seDescs[url]; /* a later registration tries again */
      var w = d.wait;
      d.wait = [];
      for (var i = 0; i < w.length; i++) w[i](map);
    }
    var x = new XMLHttpRequest();
    x.onreadystatechange = function () {
      if (x.readyState !== 4) return;
      var map = null;
      try {
        if (x.status >= 200 && x.status < 300 || x.status === 0 && x.responseText)
          map = seParse(x.responseXML || new DOMParser().parseFromString(x.responseText, 'text/xml'));
      } catch (e) { map = null; }
      finish(map);
    };
    try { x.open('GET', url, true); x.send(); } catch (e) { x.onreadystatechange = null; finish(null); }
  }
  // TVF: a new stream event of the channel (component tag -1 = unknown, data hex)
  window.__tvfSE = function (tag, id, npt, hex) {
    var text = '';
    try {
      var b = new Uint8Array(hex.length / 2);
      for (var i = 0; i < b.length; i++) b[i] = parseInt(hex.substr(2 * i, 2), 16);
      text = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-8').decode(b)
                                                 : decodeURIComponent(hex.replace(/(..)/g, '%$1'));
    } catch (e) { text = ''; }
    var regs = seRegs.slice();
    for (var k = 0; k < regs.length; k++) {
      var r = regs[k];
      if (!r.bound || r.id !== id || (r.tag !== null && tag >= 0 && r.tag !== tag)) continue;
      if (!r.fired++) seLog(r, 'first event');
      seFire(r, 'trigger', hex, text);
    }
  };

  var proto = window.HTMLObjectElement && window.HTMLObjectElement.prototype;
  if (proto) {
    // data/attributes of the A/V objects (even if type is still missing: the order
    // data before type does occur)
    var nativeData = Object.getOwnPropertyDescriptor(proto, 'data');
    Object.defineProperty(proto, 'data', {
      get: function () {
        adopt(this);
        return this.__av ? this.__av.url : nativeData.get.call(this);
      },
      set: function (v) {
        if (isAv(this) || !nativeGetAttr.call(this, 'type')) setData(this, v);
        else nativeData.set.call(this, v);
      },
      configurable: true
    });
    // the set value wins: an attribute from parser/innerHTML (also data="")
    // is dropped, otherwise adopt() would take it later (play(), observer) and
    // clear the URL - Anixe: '<object type="video/mp4" data="">', then
    // vid.data = url; vid.play(1)
    function setData(el, v) {
      nativeRemoveAttr.call(el, 'data');
      setUrl(el, v);
    }
    proto.setAttribute = function (n, v) {
      var ln = String(n).toLowerCase();
      if (ln === 'data' && (isAv(this) || !nativeGetAttr.call(this, 'type'))) { setData(this, v); return; }
      nativeSetAttr.call(this, n, v);
      if (ln === 'type') adopt(this);
    };
    proto.getAttribute = function (n) {
      if (String(n).toLowerCase() === 'data') adopt(this);
      if (this.__av && String(n).toLowerCase() === 'data') return this.__av.url || null;
      return nativeGetAttr.call(this, n);
    };
    proto.removeAttribute = function (n) {
      if (this.__av && String(n).toLowerCase() === 'data') { setUrl(this, ''); return; }
      nativeRemoveAttr.call(this, n);
    };
    Object.defineProperty(proto, 'playState', {
      get: function () { var a = avOf(this); return a ? a.state : (this.__ps || 0); },
      set: function (v) { this.__ps = v; },
      configurable: true
    });
    Object.defineProperty(proto, 'playPosition', {
      get: function () {
        var a = avOf(this);
        return a && a.video && a.loaded ? Math.floor(a.video.currentTime * 1000) : 0;
      },
      configurable: true
    });
    Object.defineProperty(proto, 'playTime', {
      get: function () {
        var a = avOf(this), d = a && a.video && a.loaded ? a.video.duration : 0;
        return isFinite(d) ? Math.floor(d * 1000) : 0;
      },
      configurable: true
    });
    Object.defineProperty(proto, 'speed', {
      get: function () { var a = avOf(this); return a ? a.speed : 0; },
      configurable: true
    });
    Object.defineProperty(proto, 'error', {
      get: function () { var a = avOf(this); return a ? a.error : undefined; },
      configurable: true
    });
    proto.play = function (speed) { return avPlay(this, speed); };
    proto.seek = function (ms) {
      var a = avOf(this);
      if (!a || !a.video || !a.loaded) return false;
      a.video.currentTime = Math.max(0, Number(ms) / 1000);
      return true;
    };
    proto.setVolume = function (v) {
      var a = av(this);
      a.volume = Math.max(0, Math.min(100, Number(v)));
      if (a.video) a.video.volume = a.volume / 100;
      return true;
    };
    proto.getVolume = function () { return this.__av && this.__av.volume !== undefined ? this.__av.volume : 100; };
    proto.queue = function () { return false; };
    proto.getComponents = function (type) { return components(this, type); };
    proto.getCurrentActiveComponents = function (type) { return activeComponents(this, type); };
    proto.selectComponent = function (c) { selectComponent(this, c, true); };
    proto.unselectComponent = function (c) { selectComponent(this, c, false); };
    proto.COMPONENT_TYPE_VIDEO = 0;
    proto.COMPONENT_TYPE_AUDIO = 1;
    proto.COMPONENT_TYPE_SUBTITLE = 2;

    proto.getOwnerApplication = function () { return app; };
    // TV picture behind the app (TS 102 796 6.2.2.6, A.2.4.1): stop() ends
    // the presentation, bindToCurrentChannel() brings it back, setChannel(null)
    // makes the app broadcast-independent (then no TV picture at all). TVF
    // keeps the state for the whole session (also across page changes).
    proto.bindToCurrentChannel = function () {
      this.playState = 2;
      post({ type: 'hbbtv-broadcast', on: true, why: 'bindToCurrentChannel' });
      videoRect(this);
      watch(this);
      return ch;
    };
    proto.setChannel = function (c) {
      if (isAv(this)) return;
      if (c === null) {
        this.playState = 0;
        post({ type: 'hbbtv-broadcast', on: false, why: 'setChannel(null)' });
      } else if (c) {
        this.playState = 2;
        post({ type: 'hbbtv-broadcast', on: true, why: 'setChannel' });
      }
    };
    proto.prevChannel = function () {};
    proto.nextChannel = function () {};
    proto.setFullScreen = function (f) {
      this.fullScreen = !!f;
      if (isAv(this)) {
        av(this).full = !!f;
        place(this);
        fire(this, 'FullScreenChange', 'fullScreen', !!f);
      } else {
        videoRect(this);
      }
    };
    proto.stop = function () {
      if (isAv(this)) return avStop(this);
      this.playState = 3;
      post({ type: 'hbbtv-broadcast', on: false, why: 'stop' });
      return true;
    };
    proto.release = function () {
      if (isAv(this)) return avStop(this);
      this.playState = 0;
      return true;
    };
    proto.getChannelConfig = function () { return { channelList: [ch], currentChannel: ch }; };
    // DSM-CC stream events (ETSI TS 102 796 8.2.1.2, OIPF DAE 7.13.21): the
    // tvheadend fork passes the stream descriptor sections of the channel, TVF
    // calls __tvfSE (below) for each new event. targetURL names a stream event
    // description file (XML) that maps the event names to component tag and
    // event id. StreamEvent objects in the object carousel (dvb: URLs) are not
    // available: status "error", as for an unknown name.
    proto.addStreamEventListener = function (url, name, listener) {
      if (typeof listener !== 'function') return;
      var reg = { obj: this, url: String(url), name: String(name), listener: listener, tag: null, id: null,
                  bound: false, fired: 0 };
      for (var i = 0; i < seRegs.length; i++) {
        var r = seRegs[i];
        if (r.obj === reg.obj && r.url === reg.url && r.name === reg.name && r.listener === listener) return;
      }
      seRegs.push(reg);
      if (/^dvb:/i.test(reg.url)) return seFail(reg, 'object carousel - not available');
      var abs = reg.url;
      try { abs = new URL(reg.url, location.href).href; } catch (e) { /* as it is */ }
      seLoad(abs, function (map) {
        if (seRegs.indexOf(reg) < 0) return;
        var ev = map && map[reg.name];
        if (!ev) return seFail(reg, map ? 'no such event' : 'description not loaded');
        reg.tag = ev.tag;
        reg.id = ev.id;
        reg.bound = true;
        seLog(reg, 'registered');
      });
    };
    proto.removeStreamEventListener = function (url, name, listener) {
      for (var i = seRegs.length - 1; i >= 0; i--) {
        var r = seRegs[i];
        if (r.obj === this && r.url === String(url) && r.name === String(name) && r.listener === listener)
          seRegs.splice(i, 1);
      }
    };
    // application/oipfCapabilities
    proto.hasCapability = function (c) { return /HBBTV|DL|DASH/i.test(String(c)); };
    try {
      var caps = null;
      Object.defineProperty(proto, 'xmlCapabilities', {
        get: function () {
          if (!caps)
            caps = new DOMParser().parseFromString(
              '<profilelist><ui_profile name="OITF_HD_UIPROF+DVB_S+TRICKMODE"><ext><parentalcontrol schemes="dvb-si">true</parentalcontrol>' +
              '</ext></ui_profile><download_profile name="DL_MP4"/><audio_profile name="MPEG1_L3" type="audio/mpeg"/>' +
              '<video_profile name="MP4_AVC_SD_25_HEAAC" type="video/mp4"/><video_profile name="MP4_AVC_HD_25_HEAAC" type="video/mp4"/>' +
              '<video_profile name="MP4_AVC_SD_25_HEAAC" type="video/mp4" transport="dash"/>' +
              '<video_profile name="MP4_AVC_HD_25_HEAAC" type="video/mp4" transport="dash"/>' +
              '</profilelist>', 'text/xml');
          return caps;
        }, configurable: true
      });
      Object.defineProperty(proto, 'extraSDVideoDecodes', { get: function () { return 0; }, configurable: true });
      Object.defineProperty(proto, 'extraHDVideoDecodes', { get: function () { return 0; }, configurable: true });
    } catch (e) { /* ignore */ }
    // video/broadcast: currentChannel, playState
    try {
      Object.defineProperty(proto, 'currentChannel', { get: function () { return ch; }, configurable: true });
    } catch (e) { /* ignore */ }
    // oipfConfiguration
    try {
      Object.defineProperty(proto, 'configuration', {
        get: function () {
          // languages follow TVF's UI language (__HBBTV_FWD.lang, ISO 639-2), country stays
          var l = (window.__HBBTV_FWD && window.__HBBTV_FWD.lang) || 'deu';
          return { preferredAudioLanguage: l, preferredSubtitleLanguage: l, preferredUILanguage: l,
                   countryId: 'DEU', regionId: 0 };
        }, configurable: true
      });
      Object.defineProperty(proto, 'localSystem', {
        get: function () { return { deviceID: 'tvh-webos', modelName: 'webOS', vendorName: 'LGE', softwareVersion: '1' }; },
        configurable: true
      });
    } catch (e) { /* ignore */ }
  }

  // Track the position of the TV picture (the apps scale it via style): one
  // timer for the most recently bound object - an earlier one that the app
  // removed would otherwise keep reporting 0x0 (full screen) alternately (SAT.1)
  var watched = null, watchTimer = null;
  function currentBroadcast() {
    if (watched && watched.isConnected) return watched;
    var list = document.querySelectorAll('object[type="video/broadcast"]');
    for (var i = 0; i < list.length; i++)
      if (list[i].isConnected) return list[i];
    return null;
  }
  function watch(el) {
    watched = el;
    if (!watchTimer) {
      watchTimer = setInterval(function () {
        var b = currentBroadcast();
        if (b) videoRect(b);
      }, 400);
    }
  }

  window.oipfObjectFactory = {
    isObjectSupported: function () { return true; },
    createApplicationManagerObject: function () { var o = document.createElement('object'); o.type = 'application/oipfApplicationManager'; return o; },
    createVideoBroadcastObject: function () { var o = document.createElement('object'); o.type = 'video/broadcast'; return o; },
    createConfigurationObject: function () { var o = document.createElement('object'); o.type = 'application/oipfConfiguration'; return o; },
    createCapabilitiesObject: function () { var o = document.createElement('object'); o.type = 'application/oipfCapabilities'; return o; },
    createVideoMpegObject: function () { var o = document.createElement('object'); o.type = 'video/mp4'; return o; }
  };

  // transparent background: the webOS app's TV picture lies beneath
  document.addEventListener('DOMContentLoaded', function () {
    try {
      document.documentElement.style.background = 'transparent';
      document.body.style.background = document.body.style.background || 'transparent';
      var vb = document.querySelector('object[type="video/broadcast"]');
      if (vb) watch(vb);
    } catch (e) { /* ignore */ }
    // include a keyset already set during loading (otherwise all keys apply)
    post({ type: 'hbbtv-loaded', url: String(window.location.href), keyset: keyset.set ? keyset.value : null,
          pointer: pointer });
  });
  // the page is being left: if a page without the polyfill follows (https - the
  // polyfill cannot get in there, and only there is DRM/EME available), it gets
  // the real keys
  window.addEventListener('pagehide', function () { post({ type: 'hbbtv-unload' }); });
  window.close = function () { post({ type: 'hbbtv-exit' }); };
})();
