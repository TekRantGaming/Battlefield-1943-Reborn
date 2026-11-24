"""An admin's F9 on a Night Ops server (server/gmhost.py World, 24 Nov 2026) without the game: a World on a stand in
host, asked with the bytes world_sync.cpp sends.

Checks: a plain Night Ops round as before (19:00, the build by tickets, a new round clear); an admin's time, weather,
clouds and fog taking effect and staying while the build goes on (per setting: what they did not move still follows the
tickets); the answer's flags (4 Night Ops, 8 held); "Back to Night Ops" (ask flag 8); the next round a plain Night Ops
round again; a game from before the touched mask (moved = not the server's); a non admin's change ignored; a regular
server as before (every value taken, flags 4 and 8 never set).

    python -I tools/test_night_ops_admin.py
"""
import os
import struct
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'server'))
os.environ.pop('BF_DEV_NIGHT_RAMP', None)
import gmhost  # noqa: E402

FAILS = []
KEY = 'test-admin-key'


def check(what, ok, detail=''):
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', what, '' if ok else '   ' + str(detail)[:400]))
    if not ok:
        FAILS.append(what)


class Host:
    def __init__(self):
        self.notes, self.sent, self.peers = [], [], {}
        self.transport = types.SimpleNamespace(sendto=lambda data, addr: self.sent.append(data))
        self.comms = types.SimpleNamespace(joined_player=lambda name, addr: False)

    def note(self, text):
        self.notes.append(text)
        print('      GMHOST ' + text)

    def player_names(self):
        return []


def world(night):
    host = Host()
    w = gmhost.World(host)
    args = types.SimpleNamespace(night=night, day_night=False, time=16.0, day_minutes=0, cloud_cover=0.35,
                                 cloud_rate=0.33, weather=0.0, fog=0.0, ground_fog=0.0, wind=0.3, cloud_height=450,
                                 match_file='', admins='', admin_key=KEY)
    w.configure(args)
    return w, host


def text(s):
    raw = s.encode('utf-8')
    return bytes([len(raw)]) + raw


def ask(key=KEY, change=None, touched=None, back=False, name='Admin'):
    """world_sync.cpp's BF43WRLD body (after the 8 byte tag): a change is a dict of Settings, `touched` its mask."""
    flags = (1 if change else 0) | (8 if back else 0) | (0 if change or back else 4)
    b = bytes([1, flags]) + text(name) + text(key)
    if not change:
        return b + (b'\xff\xff' if flags & 4 else b'')
    c = change
    b += bytes([1 if c['day_night'] else 0]) + struct.pack('>8f', c['hour'], c['day_minutes'], c['cover'], c['rate'],
                                                           c['weather'], c['fog'], c['ground_fog'], c['wind'])
    b += struct.pack('>f', c['cloud_height']) + bytes([0, 0]) + struct.pack('>ff', 0.5, 0.7)
    if touched is not None:
        b += struct.pack('>H', touched)
    return b


def now(w):
    """The world as the panel holds it (an answer), as a dict."""
    return {'day_night': w.day_night, 'hour': w.hour, 'day_minutes': w.day_minutes, 'cover': w.cover, 'rate': w.rate,
            'weather': w.weather, 'fog': w.fog, 'ground_fog': w.ground_fog, 'wind': w.wind,
            'cloud_height': w.cloud_height}


def answer(w, host, body):
    host.sent.clear()
    w.receive(body, ('127.0.0.1', 5000))
    r = host.sent[0]
    assert r[:8] == b'BF43WRLR'
    vals = struct.unpack_from('>8f', r, 10)
    return r[9], vals


T = gmhost.World.TOUCHED
near = lambda a, b: abs(a - b) < 1e-4

# --- a plain Night Ops round, as before
w, host = world(True)
check('Night Ops starts at 19:00, clock held, clear', w.day_night and w.hour == 19.0 and w.day_minutes == 0 and
      near(w.cover, 0.15) and w.weather == 0 and w.fog == 0)
w.match(1, 1.0)
w.match(1, 0.5)  # half the tickets gone
c50, wx50, f50, _ = gmhost.World.night_ops_values(0.5)
check('the build at 50%', near(w.cover, c50) and near(w.weather, wx50) and near(w.fog, f50), now(w))
flags, _ = answer(w, host, ask(key='not-it'))
check('answer flags: Night Ops (4), not an admin, nothing held', flags & 4 and not flags & 2 and not flags & 8, flags)

# --- a non admin's change is ignored
before = now(w)
noon = dict(before, hour=12.0)
answer(w, host, ask(key='not-it', change=noon, touched=T['hour']))
check('a non admin cannot set the time', w.hour == 19.0)

# --- the admin moves the time: it stays, the storm still builds
flags, vals = answer(w, host, ask(change=noon, touched=T['hour']))
check('the admin\'s time takes effect (12:00) and is sent to every player', w.hour == 12.0 and near(vals[0], 12.0),
      vals)
check('answer flags: admin (2), Night Ops (4), held (8)', flags & 2 and flags & 4 and flags & 8, flags)
w.match(1, 0.3)
c70, wx70, f70, _ = gmhost.World.night_ops_values(0.7)
check('the time stays while the tickets go on', w.hour == 12.0)
check('the clouds, weather and fog still follow the tickets', near(w.cover, c70) and near(w.weather, wx70) and
      near(w.fog, f70), now(w))

# --- the admin moves the weather (an asker whose clouds are stale must not set them back)
stale = dict(now(w), cover=0.2, weather=0.1)
answer(w, host, ask(change=stale, touched=T['weather']))
check('the admin\'s weather (10%) takes effect', near(w.weather, 0.1))
check('a stale cloud cover in the same ask is not taken', near(w.cover, c70), w.cover)
w.match(1, 0.05)
c95, _, f95, _ = gmhost.World.night_ops_values(0.95)
check('weather stays at 10% to the round\'s end; clouds and fog still build', near(w.weather, 0.1) and
      near(w.cover, c95) and near(w.fog, f95), now(w))
check('held: clock and weather', w.held == {'clock', 'weather'}, w.held)

# --- Back to Night Ops
flags, _ = answer(w, host, ask(back=True))
check('Back to Night Ops: 19:00 again, the build at the round\'s progress', w.hour == 19.0 and w.day_minutes == 0
      and near(w.weather, gmhost.World.night_ops_values(0.95)[1]) and not w.held, now(w))
check('answer flags after it: nothing held', not flags & 8, flags)
answer(w, host, ask(key='not-it', back=True))

# --- an admin's settings last the round only
answer(w, host, ask(change=dict(now(w), hour=12.0, day_minutes=24.0, cover=0.0, fog=0.0, ground_fog=0.0),
                    touched=T['hour'] | T['day_minutes'] | T['cover'] | T['fog'] | T['ground_fog']))
check('held: clock, clouds, fog, ground fog', w.held == {'clock', 'clouds', 'fog', 'ground fog'}, w.held)
w.match(2, 1.0)
check('the next round is a plain Night Ops round: 19:00, clock held, clear',
      w.hour == 19.0 and w.day_minutes == 0 and w.day_night and near(w.cover, 0.15) and w.weather == 0 and
      w.fog == 0 and w.ground_fog == 0 and not w.held, now(w))
check('the round change says the admin\'s settings ended', any('admin settings ended' in n for n in host.notes))

# --- a game from before the touched mask
w.match(2, 0.4)
c60, wx60, f60, _ = gmhost.World.night_ops_values(0.6)
answer(w, host, ask(change=dict(now(w), weather=0.9)))
check('an older game: the moved weather is taken and held, the rest untouched',
      near(w.weather, 0.9) and w.held == {'weather'} and w.hour == 19.0 and near(w.cover, c60), now(w))

# --- cloud rate, wind and height stay the admin's, as before
answer(w, host, ask(change=dict(now(w), rate=0.8, wind=0.7, cloud_height=777.0),
                    touched=T['rate'] | T['wind'] | T['cloud_height']))
check('cloud rate, wind, cloud height set', near(w.rate, 0.8) and near(w.wind, 0.7) and near(w.cloud_height, 777))
check('they hold nothing of the build', w.held == {'weather'}, w.held)

# --- a regular server, as before
r, rhost = world(False)
flags, _ = answer(r, rhost, ask(key='not-it'))
check('a regular server never says Night Ops or held', not flags & 4 and not flags & 8, flags)
answer(r, rhost, ask(change=dict(now(r), hour=8.0, day_night=True, cover=0.9, weather=0.5, fog=0.2), touched=T['hour']))
check('a regular server takes every value of the ask (as before)', r.hour == 8.0 and r.day_night and
      near(r.cover, 0.9) and near(r.weather, 0.5) and near(r.fog, 0.2) and not r.held, now(r))
answer(r, rhost, ask(back=True))
check('Back to Night Ops does nothing on a regular server', r.hour == 8.0 and near(r.weather, 0.5))
r.match(1, 0.2)
check('tickets do nothing on a regular server', near(r.weather, 0.5))

print()
print('FAILED: %s' % ', '.join(FAILS) if FAILS else 'all passed')
sys.exit(1 if FAILS else 0)
