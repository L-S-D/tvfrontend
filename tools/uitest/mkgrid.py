# SPDX-License-Identifier: GPL-3.0-or-later
# Builds an EPG grid like tvsrc (groups, channels, broadcasts) from a
# Kodi JSON-RPC (read-only): TVF's own on the TV or a Kodi, TVF_KODI=host:port
# (default localhost:8080) - test data for the PC test.
import json, os, sys, time, urllib.request, urllib.parse, calendar

KODI = os.environ.get('TVF_KODI', 'localhost:8080')
RPC = 'http://%s/jsonrpc' % KODI
def rpc(method, params):
    req = urllib.request.Request(RPC, json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params}).encode(),
                                 {'Content-Type': 'application/json'})
    return json.load(urllib.request.urlopen(req, timeout=10))['result']

def ts(s):
    return calendar.timegm(time.strptime(s, '%Y-%m-%d %H:%M:%S')) if s else 0

now = time.time()
start = int(now // 1800 * 1800)
dur = 10800
ngroups = int(sys.argv[1]) if len(sys.argv) > 1 else 4
groups = [g for g in rpc('PVR.GetChannelGroups', {'channeltype': 'tv'})['channelgroups'] if g['channelgroupid'] != 'alltv']
out_groups, channels = [], []
for g in groups[:ngroups]:
    chs = rpc('PVR.GetChannels', {'channelgroupid': g['channelgroupid'], 'properties': ['channelnumber', 'thumbnail', 'channel']})['channels']
    if not chs:
        continue
    out_groups.append({'id': g['channelgroupid'], 'label': g['label'], 'start': len(channels)})
    for ch in chs[:40]:
        bc = rpc('PVR.GetBroadcasts', {'channelid': ch['channelid'], 'properties': ['title', 'starttime', 'endtime', 'episodename', 'plot']})
        bl = []
        for b in bc.get('broadcasts', []):
            s, e = ts(b.get('starttime')), ts(b.get('endtime'))
            if e > start and s < start + dur:
                bl.append({'id': b['broadcastid'], 'title': b.get('title', ''), 'subtitle': b.get('episodename', ''),
                           'plot': b.get('plot', ''), 'start': s, 'stop': e, 'rec': None, 'series': False})
        thumb = ch.get('thumbnail', '')
        icon = 'http://%s/image/' % KODI + urllib.parse.quote(thumb, safe='') + '?trim' if thumb else ''
        channels.append({'id': ch['channelnumber'], 'name': ch['channel'], 'icon': icon, 'broadcasts': bl})
# one recording marker for testing
for c in channels[:3]:
    if c['broadcasts']:
        c['broadcasts'][0]['rec'] = 'scheduled'
if len(channels) > 1 and channels[1]['broadcasts']:
    channels[1]['broadcasts'][0]['series'] = True
msg = {'command': 'epgGrid', 'groupid': 'groups', 'groups': out_groups, 'start': start, 'duration': dur,
       'currentChannel': channels[0]['id'] if channels else None, 'channels': channels}
json.dump(msg, sys.stdout, ensure_ascii=False)
