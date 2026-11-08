# SPDX-License-Identifier: GPL-3.0-or-later
# Recordings/timers for uitest (recs.json) from a Kodi JSON-RPC (read-only):
# TVF's own on the TV or a Kodi, TVF_KODI=host:port (default localhost:8080).
# Fields like tvrec's recording/timer list, as far as they can be derived.
import calendar, json, os, sys, time, urllib.request

RPC = 'http://%s/jsonrpc' % os.environ.get('TVF_KODI', 'localhost:8080')
req = urllib.request.Request(RPC, json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'PVR.GetRecordings', 'params': {
    'properties': ['title', 'file', 'runtime', 'channel', 'starttime', 'plot', 'plotoutline', 'resume', 'playcount']}}).encode(),
    {'Content-Type': 'application/json'})
recs = json.load(urllib.request.urlopen(req, timeout=10))['result']['recordings']
out = []
for x in recs:
    st = calendar.timegm(time.strptime(x['starttime'], '%Y-%m-%d %H:%M:%S'))
    out.append({'id': x['file'].rsplit('/', 1)[-1], 'title': x['title'], 'subtitle': x.get('plotoutline', ''),
                'description': x.get('plot', ''), 'channelName': x['channel'], 'start': st, 'stop': st + x['runtime'],
                'duration': x['runtime'], 'size': x['runtime'] * 1.1e6, 'state': 'completed', 'error': '',
                'position': (x.get('resume') or {}).get('position', 0), 'watched': x.get('playcount', 0) > 0,
                'icon': '', 'video': '1920x1080 H264', 'audio': 'AC3'})
out.sort(key=lambda a: -a['start'])
now = int(time.time())
timers = [{'timerId': 5, 'id': 'beispiel', 'eventId': 123, 'title': 'Tatort', 'subtitle': 'Der Fall',
           'channelName': 'Das Erste HD', 'channelNumber': 21, 'start': now + 3600, 'stop': now + 9000,
           'state': 'scheduled', 'autorecId': None, 'seriesTitle': '', 'icon': ''}]
json.dump({'recordings': out, 'timers': timers}, sys.stdout, ensure_ascii=False)
