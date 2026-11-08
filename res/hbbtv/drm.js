// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TVF: OIPF DRM agent (application/oipfDrmAgent) for HbbTV pages in TVF's own
 * renderer, mapped onto LG's PlayReady service in TVF (hbbdrm.c). Runs
 * after the OIPF polyfill (polyfill.js): its object prototype gets
 * sendDRMMessage & co., the capability list reports PlayReady (ORF ON checks for
 * a DASH video_profile with DRMSystemID). The TV's DRM client fetches the license
 * itself - the page only supplies initiators (license URL, CustomData).
 */

/*
 * Joyn (HbbTV app live.prd.ott.s.joyn.de, DE and AT): the current player
 * (Shaka) needs MSE and EME, which this WebKit lacks - it gets stuck at
 * "Gleich geht's los" ("starting shortly"). Joyn still has the OIPF path (<video> or
 * A/V object, oipfDrmAgent with PlayReady), via a feature override (the app reads it
 * from localStorage at startup and deletes it): oipf-player reports the
 * device with drmType playready - otherwise Joyn issues a Widevine license token
 * that the TV's PlayReady client cannot redeem (400 "License
 * acquisition failed"); future-player off so that the old selection applies.
 */
/*
 * With EME (bundle with "eme", __HBBTV_FWD.eme): WebKit has Widevine via the
 * TV's CDM (TVF) - Joyn then uses its own player (Shaka) as in the
 * TV's HbbTV browser. But Joyn picks the DRM by the manufacturer in the
 * User-Agent (LG -> PlayReady, Samsung and unknown ones -> Widevine) and fetches
 * sources and license URL only for that one; PlayReady is not available for EME (not
 * possible in the app process, see TVF cdm.cpp), Shaka then aborts with 6012
 * (no license server for Widevine). Hence Joyn's own switch
 * __dev-drm-widevine (same override path). For PlayReady the agent stays
 * with LG's DRM service.
 */
(function () {
  try {
    if (/(^|\.)joyn\.(de|at)$/.test(location.hostname) && /[?&]platform=hbbtv\b/.test(location.search)) {
      localStorage.setItem('ducklings-params-override', window.__HBBTV_FWD && window.__HBBTV_FWD.eme
        ? 'feature___dev-drm-widevine=true'
        : 'feature_oipf-player=true&feature_hbbtv-playback=true&feature_future-player=false');
    }
  } catch (e) { /* no localStorage */ }
})();

(function () {
  'use strict';
  if (window.__tvfDrm || typeof HTMLObjectElement === 'undefined') return;
  function post(m) { try { window.webkit.messageHandlers.tvf.postMessage(m); } catch (e) { /* no TVF */ } }
  var PR = 'urn:dvb:casystemid:19219';
  var D = window.__tvfDrm = { seq: 0, pending: {} };
  var proto = HTMLObjectElement.prototype;

  function later(fn) { setTimeout(function () { try { fn(); } catch (e) { /* page error */ } }, 0); }
  // OIPF callback (onX) and DOM event (X) with fields
  function fire(el, name, fields, args) {
    later(function () {
      var h = el['on' + name];
      if (typeof h === 'function') h.apply(el, args);
    });
    later(function () {
      var ev = document.createEvent('Event');
      ev.initEvent(name, false, false);
      for (var k in fields) ev[k] = fields[k];
      el.dispatchEvent(ev);
    });
  }

  proto.sendDRMMessage = function (msgType, msg, systemId) {
    var id = String(++D.seq);
    var sid = String(systemId);
    if (sid !== PR) {
      // PlayReady only: 5 = unknown DRM system
      fire(this, 'DRMMessageResult', { msgID: id, resultMsg: '', resultCode: 5 }, [id, '', 5]);
      return id;
    }
    D.pending[id] = this;
    D.pr = true; /* from now on: A/V objects via LG's pipeline (selective mode, see below) */
    post({ type: 'tvf-drm', op: 'send', id: id, msgType: String(msgType), msg: String(msg), systemId: sid });
    return id;
  };
  proto.setActiveDRM = function (sid) {
    return sid === null || sid === undefined || sid === PR || sid === 'urn:hbbtv:oipfdrm:inactive';
  };
  proto.canPlayContent = function () { return false; };
  proto.canRecordContent = function () { return false; };

  // result from TVF (hbbweb.c)
  D.result = function (id, code, resultMsg) {
    var el = D.pending[id];
    delete D.pending[id];
    if (el) fire(el, 'DRMMessageResult', { msgID: id, resultMsg: resultMsg, resultCode: code }, [id, resultMsg, code]);
  };
  // rights error (DRM service subscription): to agent, A/V and broadcast objects
  D.rights = function (state, contentId, rightsUrl) {
    var list = document.querySelectorAll('object');
    for (var i = 0; i < list.length; i++) {
      var t = (list[i].getAttribute('type') || '').toLowerCase();
      if (/oipfdrmagent|video\/|audio\/|dash\+xml|mpegurl/.test(t))
        fire(list[i], 'DRMRightsError', { errorState: state, contentID: contentId, DRMSystemID: PR, rightsIssuerURL: rightsUrl },
             [state, contentId, PR, rightsUrl]);
    }
  };

  // Capabilities: like the polyfill, plus the DRM for DASH (HbbTV 10.2.4). Without
  // EME, PlayReady via the agent: clear video profiles and PlayReady profiles.
  // With EME, Widevine (WebKit can do that via the TV's CDM) and then ALL
  // video profiles with DRMSystemID: ORF (HbbTV.Platform.getSupportedDrmSystem
  // for Bitmovin) checks PLAYREADY before WIDEVINE and counts a profile without
  // DRMSystemID as a match for every system - with clear profiles it would stay
  // with PlayReady, which WebKit does not have (Bitmovin 2006 DRM_NO_KEY_SYSTEM)
  var EME = !!(window.__HBBTV_FWD && window.__HBBTV_FWD.eme);
  var DRMID = EME ? 'urn:dvb:casystemid:19156' : PR;
  function video(dash, drm) {
    var a = ' type="video/mp4"' + (dash ? ' transport="dash"' : '') + (drm ? ' DRMSystemID="' + DRMID + '"' : '') + '/>';
    return '<video_profile name="MP4_AVC_SD_25_HEAAC"' + a + '<video_profile name="MP4_AVC_HD_25_HEAAC"' + a;
  }
  var caps = null;
  try {
    Object.defineProperty(proto, 'xmlCapabilities', {
      get: function () {
        if (!caps)
          caps = new DOMParser().parseFromString(
            '<profilelist><ui_profile name="OITF_HD_UIPROF+DVB_S+TRICKMODE+DRM"><ext><parentalcontrol schemes="dvb-si">true' +
            '</parentalcontrol></ext></ui_profile><download_profile name="DL_MP4"/>' +
            '<audio_profile name="MPEG1_L3" type="audio/mpeg"/>' +
            '<audio_profile name="MP4_HEAAC" type="audio/mp4" transport="dash" DRMSystemID="' + DRMID + '"/>' +
            (EME ? video(false, true) + video(true, true)
                 : video(false, false) + video(true, false) + video(true, true)) +
            '<drm DRMSystemID="' + DRMID + '">MP4_CENC</drm></profilelist>', 'text/xml');
        return caps;
      }, configurable: true
    });
  } catch (e) { /* ignore */ }
  var has = proto.hasCapability;
  proto.hasCapability = function (c) {
    return /\+DRM|PLAYREADY/i.test(String(c)) || (typeof has === 'function' ? has.call(this, c) : false);
  };
  /*
   * The renderer plays media itself (media.js in selective mode): PlayReady via the
   * agent only works in LG's pipeline with the TV's DRM client (for WebKit there
   * is no PlayReady CDM - not possible in the app process). If the page has sent a
   * PlayReady initiator, A/V objects therefore play via TVF, as
   * in the TV's HbbTV browser: the polyfill's dash.js players become
   * media.js's shim player (MPD as src to LG's pipeline), media.js redirects src on
   * the videos of the A/V objects (data-hbbtv-av: MP4, HLS).
   */
  var SELECTIVE = !!(window.__HBBTV_FWD && window.__HBBTV_FWD.selective);
  function routeDash(dj) {
    if (!dj || !dj.MediaPlayer || dj.__tvfRoute) return dj;
    dj.__tvfRoute = true;
    var MP = dj.MediaPlayer;
    var W = function () {
      var f = MP.apply(this, arguments), create = f.create;
      f.create = function () {
        var TM = window.__tvfMedia;
        if (D.pr && TM && TM.dashShim) {
          console.log('[drm] dash.js after PlayReady initiator: LG pipeline');
          return TM.dashShim().create();
        }
        return create.apply(this, arguments);
      };
      return f;
    };
    for (var k in MP) if (Object.prototype.hasOwnProperty.call(MP, k)) W[k] = MP[k];
    dj.MediaPlayer = W;
    return dj;
  }
  if (SELECTIVE) {
    var dj = routeDash(window.dashjs);
    try {
      Object.defineProperty(window, 'dashjs', {
        configurable: true,
        get: function () { return dj; },
        set: function (v) { dj = routeDash(v); }
      });
    } catch (e) { /* no redirection */ }
    var NMP = window.HTMLMediaElement && HTMLMediaElement.prototype;
    var srcDesc = NMP && Object.getOwnPropertyDescriptor(NMP, 'src');
    if (srcDesc && srcDesc.set) {
      Object.defineProperty(NMP, 'src', {
        configurable: true, enumerable: srcDesc.enumerable, get: srcDesc.get,
        set: function (v) {
          var TM = window.__tvfMedia;
          if (D.pr && TM && TM.adopt && this.getAttribute('data-hbbtv-av') && !this.__tvfUp) {
            console.log('[drm] A/V object after PlayReady initiator: LG pipeline');
            TM.adopt(this);
            this.src = v; /* now media.js's setter */
            return;
          }
          srcDesc.set.call(this, v);
        }
      });
    }
  }

  if (window.oipfObjectFactory) {
    window.oipfObjectFactory.createDrmAgentObject = function () {
      var o = document.createElement('object');
      o.type = 'application/oipfDrmAgent';
      return o;
    };
  }
})();
