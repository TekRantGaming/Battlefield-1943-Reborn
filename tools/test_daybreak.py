"""Daybreak servers (server/gmhost.py World, 26 Nov 2026) without the game: a World on a stand in host and a stand in
clock, asked with the bytes world_sync.cpp sends.

Checks (TekRantGaming's rules after playing the first cut: held at 06:10, Night Ops' storm, less ground fog): the
world held at 06:10, clear, with a morning haze and a thin ground mist; the answer's flags (4 an event, 16 Daybreak)
and its last bytes (storm type 0, the surface fog 1.2 m deep in patches); Night Ops' build by tickets to the full
storm (100% weather by 80%, the same clouds, weather and distance fog as Night Ops' at every point), less ground fog
than Night Ops', the wind rising; never backwards in a round; a new round clear at 06:10; an admin's wind held (Daybreak
drives it); "Back to Daybreak"; with a running clock (BF_DEV_DAYBREAK_MINUTES) the sun waits while nobody is joined;
--night wins over --daybreak; Night Ops and a regular server answer as before (no flag 16, the games' own fog depth).

    python -I tools/test_daybreak.py
"""
import os
import struct
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'server'))
for k in ('BF_DEV_NIGHT_RAMP', 'BF_DEV_EVENT_RAMP', 'BF_DEV_DAYBREAK_MINUTES'):
    os.environ.pop(k, None)
import gmhost  # noqa: E402

FAILS = []
KEY = 'test-admin-key'


def check(what, ok, detail=''):
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', what, '' if ok else '   ' + str(detail)[:400]))
    if not ok:
        FAILS.append(what)


class Clock:
    """time.monotonic for gmhost, moved by hand."""
    def __init__(self):
        self.t = 1000.0

    def monotonic(self):
        return self.t


CLOCK = Clock()
gmhost.time = types.SimpleNamespace(monotonic=CLOCK.monotonic, time=lambda: 0.0)


class Host:
    def __init__(self):
        self.notes, self.sent, self.peers = [], [], {}
        self.transport = types.SimpleNamespace(sendto=lambda data, addr: self.sent.append(data))
        self.comms = types.SimpleNamespace(joined_player=lambda name, addr: False)

    def note(self, text):
        self.notes.append(text)
        print('      GMHOST ' + text)

    players = ['Tester']  # a player on the server (Daybreak's clock waits while nobody is)

    def player_names(self):
        return list(self.players)


def world(night=False, daybreak=False):
    host = Host()
    w = gmhost.World(host)
    args = types.SimpleNamespace(night=night, daybreak=daybreak, day_night=False, time=16.0, day_minutes=0,
                                 cloud_cover=0.35, cloud_rate=0.33, weather=0.0, fog=0.0, ground_fog=0.0, wind=0.3,
                                 cloud_height=450, match_file='', admins='', admin_key=KEY)
    w.configure(args)
    return w, host


def text(s):
    raw = s.encode('utf-8')
    return bytes([len(raw)]) + raw


def ask(key=KEY, change=None, touched=None, back=False, name='Admin'):
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
    return {'day_night': w.day_night, 'hour': w.hour, 'day_minutes': w.day_minutes, 'cover': w.cover, 'rate': w.rate,
            'weather': w.weather, 'fog': w.fog, 'ground_fog': w.ground_fog, 'wind': w.wind,
            'cloud_height': w.cloud_height}


def answer(w, host, body):
    host.sent.clear()
    w.receive(body, ('127.0.0.1', 5000))
    r = host.sent[0]
    assert r[:8] == b'BF43WRLR'
    return r


def parse(r):
    """As world_sync.cpp reads it: flags, the eight floats, and (n >= 80) the storm type, fog depth and sparseness."""
    vals = struct.unpack_from('>8f', r, 10)
    tail = (r[71],) + struct.unpack_from('>ff', r, 72) if len(r) >= 80 else None
    return r[9], vals, tail


T = gmhost.World.TOUCHED
near = lambda a, b, e=1e-4: abs(a - b) < e
D = gmhost.World

# --- a Daybreak round: held at 06:10
w, host = world(daybreak=True)
check('Daybreak is held at 06:10: day and night on, time standing still', w.day_night and
      near(w.hour, 6.0 + 10 / 60.0) and w.day_minutes == 0, now(w))
check('first light: clear (15%), a haze (15%), a thin ground mist (35%), a light wind (15%)', near(w.cover, 0.15) and
      w.weather == 0 and near(w.fog, 0.15) and near(w.ground_fog, 0.35) and near(w.wind, 0.15), now(w))
flags, vals, tail = parse(answer(w, host, ask(key='not-it')))
check('answer flags: day and night (1), an event (4), Daybreak (16), not an admin, nothing held',
      flags & 1 and flags & 4 and flags & 16 and not flags & 2 and not flags & 8, flags)
check('answer is 80 bytes: storm type 0 (a thunderstorm), ground fog 1.2 m deep, sparseness 0.6',
      tail is not None and tail[0] == 0 and near(tail[1], 1.2) and near(tail[2], 0.6), tail)
w.match(1, 1.0)
CLOCK.t += 30 * 60
flags, vals, _ = parse(answer(w, host, ask(key='not-it')))
check('30 minutes later still 06:10', near(w.hour, 6.0 + 10 / 60.0) and near(vals[0], 6.0 + 10 / 60.0, 1e-4), w.hour)
check('a held clock never says the sun waits', not any('the sun waits' in n for n in host.notes))

# --- Night Ops' build by tickets
same = all(near(D.daybreak_values(p / 100.0)[k], D.night_ops_values(p / 100.0)[k]) for p in range(101) for k in (0, 1))
check("clouds and weather are Night Ops' at every point of the round", same)
check("distance fog is Night Ops' in the full storm (from 80%; a light daytime haze before it)",
      all(near(D.daybreak_values(p / 100.0)[2], D.night_ops_values(p / 100.0)[2]) for p in range(80, 101)))
check("less ground fog than Night Ops' in the storm (35% against 50%)",
      near(D.daybreak_values(1.0)[3], 0.35) and D.daybreak_values(1.0)[3] < D.night_ops_values(1.0)[2])
w.match(1, 0.6)  # 40% lost: still clear, the mist gone
check('at 40%: still clear, the mist burnt off (10%)', near(w.cover, 0.15) and w.weather == 0 and
      near(w.ground_fog, 0.10), now(w))
w.match(1, 0.8)  # a team's tickets back up: the build stays
check('never backwards within a round', near(w.ground_fog, 0.10), now(w))
w.match(1, 0.2)
check('at 80%: the full storm, weather 100%, clouds 90%, fog 50%, ground fog 35%, wind 80%',
      near(w.cover, 0.9) and near(w.weather, 1.0) and near(w.fog, 0.5) and near(w.ground_fog, 0.35) and
      near(w.wind, 0.8), now(w))
s, shost = world(daybreak=True)
shost.notes.clear()  # (the server's start says first light too)
s.match(1, 1.0)
for p in range(0, 101, 5):
    s.match(1, 1.0 - p / 100.0)
said = [x.split(': ')[2].split(',')[0] for x in shost.notes if 'of the round: ' in x]
check('each stage said once, in order', said == list(D.DAYBREAK_STAGE_NAMES), said)

# --- an admin's wind and time are held; Back to Daybreak
answer(w, host, ask(change=dict(now(w), wind=0.1), touched=T['wind']))
check("an admin's wind (10%) takes effect and is held (Daybreak drives it)", near(w.wind, 0.1) and
      'wind' in w.held, (w.wind, w.held))
answer(w, host, ask(change=dict(now(w), hour=12.0), touched=T['hour']))
check("an admin's time (12:00) takes effect and is held", near(w.hour, 12.0) and 'clock' in w.held, now(w))
flags, _, _ = parse(answer(w, host, ask(back=True)))
check("Back to Daybreak: 06:10, the build's wind, nothing held", near(w.hour, 6.0 + 10 / 60.0) and
      w.day_minutes == 0 and near(w.wind, 0.8) and not w.held and not flags & 8, (now(w), w.held))

# --- a new round: clear at 06:10
w.match(2, 1.0)
check('round 2: 06:10, clear, the mist back', near(w.hour, 6.0 + 10 / 60.0) and near(w.cover, 0.15) and
      w.weather == 0 and near(w.ground_fog, 0.35) and not w.held, now(w))
check('the round change says so', any('Daybreak round 2 starts at sunrise' in n for n in host.notes))

# --- a running clock (a test's, or a later rule): the sun waits while nobody is joined
os.environ['BF_DEV_DAYBREAK_MINUTES'] = '1440'
e, ehost = world(daybreak=True)  # (the switch stays set: each round start reads it)
check('BF_DEV_DAYBREAK_MINUTES=1440: the clock runs, a day in 1440 minutes', near(e.day_minutes, 1440.0), e.day_minutes)
H0 = 6.0 + 10 / 60.0
ehost.players = []
e.match(1, 1.0)
CLOCK.t += 3600
answer(e, ehost, ask(key='not-it'))
check('nobody on the server: an hour later still 06:10', near(e.hour, H0), e.hour)
check('it says so', any('the sun waits' in n for n in ehost.notes), ehost.notes[-3:])
ehost.players = ['Tester']
answer(e, ehost, ask(key='not-it'))
CLOCK.t += 600
answer(e, ehost, ask(key='not-it'))
check('a player joins: the sun rises from there (10 minutes: 06:20)', near(e.hour, H0 + 10 / 60.0, 1e-4), e.hour)
answer(e, ehost, ask(change=dict(now(e), weather=0.5), touched=T['weather']))
answer(e, ehost, ask(back=True))
check('Back to Daybreak counts only the time with players (06:20)', near(e.hour, H0 + 10 / 60.0, 1e-3), e.hour)
ehost.players = []
answer(e, ehost, ask(change=dict(now(e), hour=12.0), touched=T['hour']))
CLOCK.t += 600
answer(e, ehost, ask(key='not-it'))
check("an admin's clock runs whoever is on (not held still by the empty server)", e.hour >= 12.0 and 'clock' in e.held,
      (e.hour, e.held))
os.environ.pop('BF_DEV_DAYBREAK_MINUTES')

# --- --night wins over --daybreak
b, bhost = world(night=True, daybreak=True)
flags, _, tail = parse(answer(b, bhost, ask(key='not-it')))
check('--night and --daybreak: a Night Ops server (18:30, held still, no flag 16)', b.night_ops and not b.daybreak and
      near(b.hour, 18.5) and b.day_minutes == 0 and flags & 4 and not flags & 16, (now(b), flags))

# --- Night Ops and a regular server answer as before, with the games' own defaults at the end
n, nhost = world(night=True)
flags, _, tail = parse(answer(n, nhost, ask(key='not-it')))
check('Night Ops: flag 4, no 16; storm 0, fog 0.5 m, sparseness 0.7 (what the games drew before)',
      flags & 4 and not flags & 16 and tail == (0, 0.5, struct.unpack('>f', struct.pack('>f', 0.7))[0]), (flags, tail))
n.match(1, 1.0)
n.match(1, 0.0)
check('Night Ops\' wind stays the admin\'s (not driven)', near(n.wind, 0.3), n.wind)
r, rhost = world()
flags, _, tail = parse(answer(r, rhost, ask(key='not-it')))
check('a regular server: no 4, no 16; the games\' own fog defaults', not flags & 4 and not flags & 16 and tail[0] == 0
      and near(tail[1], 0.5) and near(tail[2], 0.7), (flags, tail))
r.match(1, 0.2)
check('tickets do nothing on a regular server', r.weather == 0 and near(r.wind, 0.3))

print()
print('FAILED: %s' % ', '.join(FAILS) if FAILS else 'all passed')
sys.exit(1 if FAILS else 0)
