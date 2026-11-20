"""The server owner's dashboard without a game (server/dashboard.py, docs/DASHBOARD.md): a week of three servers played
by made-up players, through the real Dashboard and the real manager (its server states stood in for), then the
public stats page on a loopback address of its own (127.0.0.8; nothing of a running manager is touched).

Checks: playtime, sessions, maps, kills and deaths from the game's stats lines, the tick rate a server held, restarts,
the leaderboard, the charts' series and the busiest hours, 90 days kept (older days dropped), the history read back
after a restart, the public view (no player ids, no addresses, no names when the owner turns them off), the public
page serving only its page, its drawing and its numbers, with a rate limit.

    python -I tools/test_dashboard.py [--serve]   --serve: keeps the manager's page (127.0.0.1:18296) and the public
                                                  page (127.0.0.8:18297) up with live made-up players, for screenshots
"""
import json
import os
import random
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'server'))
import dashboard  # noqa: E402

FAILS = []
NAMES = ['TestPilot', 'SandBagger', 'Corsair_Kid', 'ZeroHour', 'Beachhead', 'FlagRunner', 'Tank_Topper', 'Medic_Mo',
         'IwoGhost', 'WakeWalker', 'CoralKing', 'Rifleman7', 'BazookaBob', 'SniperSue', 'JeepJockey', 'Radio_Ray',
         'DogfightDan', 'Bunker_Bill', 'Torpedo_Tom', 'GuadalGus', 'Paratrooper', 'MortarMax', 'Spotter_Sam', 'Ace_Ann']
MAPS = ['Wake Island', 'Guadalcanal', 'Iwo Jima']


def check(what, ok, detail=''):
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', what, '' if ok else '   ' + str(detail)[:500]))
    if not ok:
        FAILS.append(what)


def roster_at(t, server, rnd):
    """Who is on a server at time t: busier in the evening (UTC), a fixed crowd per server."""
    hour = time.gmtime(t).tm_hour
    busy = max(0, int((8 if 17 <= hour <= 22 else 3 if 10 <= hour < 17 else 1) * (1.4 if server == 1 else 0.7)))
    pool = NAMES[server * 4 % len(NAMES):] + NAMES[:server * 4 % len(NAMES)]
    k = (int(t) // 1800 + server) % 5  # players change every half hour
    return [{'name': n, 'ping': 20 + (i * 17 + server * 11) % 140} for i, n in enumerate(pool[k:k + busy])]


def simulate(d, folders, start, end, step=30):
    rnd = random.Random(7)
    t = start
    maps = {n: 0 for n in (1, 2, 3)}
    while t < end:
        servers = []
        for n in (1, 2, 3):
            if int(t) % 3600 < step:
                maps[n] = (maps[n] + 1) % 3
            roster = roster_at(t, n, rnd) if n != 3 else []
            servers.append({'n': n, 'name': ['[EU] Wake 24/7', '[EU] All Maps', '[EU] Air Superiority'][n - 1],
                            'state': 'running', 'map': MAPS[maps[n]], 'next_map': MAPS[(maps[n] + 1) % 3],
                            'players': len(roster), 'max': 24, 'roster': roster, 'tick': 60, 'region': 'eu-central',
                            'since': start})
            if roster and int(t) % 300 < step:  # the game's stats lines (server_stats.cpp)
                rows = [{'name': r['name'], 'kills': rnd.randint(0, 6), 'deaths': rnd.randint(0, 5),
                         'captures': rnd.randint(0, 1)} for r in roster]
                rows.append({'name': '[BOT]Bot1', 'kills': 99})
                with open(os.path.join(folders[n], 'stats.txt'), 'a') as f:
                    f.write(json.dumps({'op': 'player_stats', 'rows': rows}) + '\n')
        d.tick(servers, {}, now=t, folders=folders)
        t += step


def unit(tmp):
    folders = {}
    for n in (1, 2, 3):
        folders[n] = os.path.join(tmp, 'server%d' % n)
        os.makedirs(folders[n])
        with open(os.path.join(folders[n], 'stats.txt'), 'w') as f:
            f.write(json.dumps({'op': 'player_stats', 'rows': [{'name': 'Before', 'kills': 50}]}) + '\n')
        with open(os.path.join(folders[n], 'server.log'), 'w') as f:
            f.write('[x] BF server ticks: %.1f a second over 30 s, longest gap 21.4 ms; the tick\'s own work 3.20 ms '
                    'average\n' % (59.8 if n != 2 else 48.0))
    folder = os.path.join(tmp, 'dashboard')
    os.makedirs(folder)
    with open(os.path.join(folder, 'day-2000-01-01.json'), 'w') as f:
        f.write('{"day": "2000-01-01", "players": {}, "samples": [], "events": []}')
    now = time.time()
    start = now - 7 * 86400
    d = dashboard.Dashboard(folder, now=now)
    check('a day older than 90 days is dropped at the start', not os.path.exists(os.path.join(folder, 'day-2000-01-01.json')))
    simulate(d, folders, start, now)
    d.note_restart(2, 'its game process ended', now - 3600)
    d.save(force=True)
    v = d.view(now=now)
    board = v['leaderboard']
    check('a week of play: a leaderboard by playtime', len(board) >= 10 and board[0]['seconds'] >= board[-1]['seconds'] and
          board[0]['seconds'] > 3600, [(r['name'], r['seconds']) for r in board[:5]])
    check('sessions counted at each join', all(r['sessions'] >= 1 for r in board))
    check('kills and deaths from the game\'s stats lines, never the lines from before the manager started, never bots',
          sum(r['kills'] for r in board) > 0 and not any(r['name'] in ('Before', '[BOT]Bot1') for r in board),
          [(r['name'], r['kills']) for r in board[:3]])
    check('a favourite map for each player', all(r['map'] in MAPS for r in board))
    check('the cards: peak, hours, sessions, tracked players', v['cards']['peak24'] > 0 and v['cards']['hours24'] > 0 and
          v['cards']['sessions24'] > 0 and v['cards']['tracked'] >= len(board), v['cards'])
    check('a restart in the last 24 hours is counted', v['cards']['restarts24'] == 1, v['cards'])
    check('players over 6 h, 24 h and 7 d', all(len(v['series'][k]) > 10 for k in ('6h', '24h', '7d')),
          {k: len(x) for k, x in v['series'].items()})
    check('the busiest hours: evenings (UTC) busier than nights', sum(v['heatmap'][0][18:22]) > sum(v['heatmap'][0][2:6]),
          v['heatmap'][0])
    check('maps played', set(v['maps']) <= set(MAPS) and len(v['maps']) == 3, v['maps'])
    check('the activity feed: joins, leaves, maps and the restart',
          {'join', 'leave', 'map', 'restart'} <= {e['kind'] for e in d.window(2, now)[0]['events'] + d.window(2, now)[-1]['events']},
          {e['kind'] for e in v['events']})
    check('the tick rate each server held, from its log', v['servers'][0]['held'] == 59.8 and v['servers'][1]['held'] == 48.0,
          [s.get('held') for s in v['servers']])
    check('each server\'s ping average', v['servers'][0]['ping_avg'] is not None)
    again = dashboard.Dashboard(folder, now=now)
    check('the history reads back the same after a restart', again.leaderboard(90, now)[0][0]['seconds'] == board[0]['seconds'])
    files = sorted(x for x in os.listdir(folder) if x.startswith('day-'))
    check('one file a day, 8 of them', len(files) == 8, files)
    raw = ''.join(open(os.path.join(folder, x), encoding='utf-8').read() for x in files)
    check('no player id or address in the history files', '127.0.0' not in raw and '"ip"' not in raw and '"id"' not in raw)
    pub = d.view(now=now, public=True)
    check('the public view has no machine processes or restart reasons', all('procs' not in s and 'reachable' not in s
                                                                           for s in pub['servers']) and
          not any('why' in e for e in pub['events']))
    anon = d.view(now=now, public=True, names=False)
    text = json.dumps(anon)
    check('names off: no player name anywhere', not any(n in text for n in NAMES), [n for n in NAMES if n in text][:3])
    return folders


def manager_part(tmp, serve=False):
    data = os.path.join(tmp, 'data')
    os.makedirs(os.path.join(data, 'logs'))
    with open(os.path.join(data, 'server.json'), 'w') as f:
        json.dump({'web_port': 18296, 'servers': [{'name': '[EU] Wake 24/7', 'port': 18440, 'autostart': False},
                                                  {'name': '[EU] All Maps', 'port': 18441, 'autostart': False},
                                                  {'name': '[EU] Air Superiority', 'port': 18442, 'autostart': False}],
                   'backend': '127.0.0.9', 'upnp': False,
                   'public_page': {'on': False, 'port': 18297, 'names': True, 'title': '[EU] Reborn Test Servers'}}, f)
    os.environ.update(BF_SERVER_DATA=data, BF_SERVER_ROOT=os.path.join(tmp, 'root'), BF_SERVER_PUBLIC_BIND='127.0.0.8',
                      BF_SERVER_NO_BROWSER='1', BF_SERVER_REGISTRY_PORT='9')
    os.makedirs(os.path.join(tmp, 'root'), exist_ok=True)
    import manager
    m = manager.Manager()
    # A week of history first (the dashboard's own simulation), then the live servers stood in for.
    now = time.time()
    m.dash.network = {'servers': [{'name': '[EU] Reborn Test Servers', 'players': 9, 'max': 24, 'map': 'Wake Island',
                                   'region': 'eu-central', 'tick': 60, 'official': False, 'promoted': True,
                                   'description': 'A test row'},
                                  {'name': 'BF1943 Reborn: All Maps 1', 'players': 14, 'max': 24, 'map': 'Iwo Jima',
                                   'region': 'eu-central', 'tick': 60, 'official': True, 'promoted': False,
                                   'description': ''}], 'players': 23, 'online': 31, 'at': int(now)}
    m.network_asked = now + 10 ** 9  # no main backend in this test
    folders = {n: m.server_dir(n) for n in (1, 2, 3)}
    for f in folders.values():
        os.makedirs(os.path.join(f, 'user'), exist_ok=True)
    with open(os.path.join(folders[1], 'user', 'match.txt'), 'w') as f:
        f.write('3 0.62')
    simulate(m.dash, folders, now - 7 * 86400, now - 60, step=60)

    def status(n):
        rnd = random.Random(int(time.time()) // 30 + n)
        roster = roster_at(time.time() + (14 - time.gmtime().tm_hour) * 3600 + 5 * 3600, n, rnd) if n != 3 else []
        for i, r in enumerate(roster):
            r.update(id='%08X' % (0xA0000000 + n * 100 + i), ip='203.0.113.%d' % (10 + i), account='',
                     since=int(time.time()) - 600 - 97 * i)
        return {'n': n, 'name': m.cfg['servers'][n - 1]['name'], 'port': 18439 + n, 'wanted': True,
                'state': 'running' if n != 3 else 'restarting', 'players': len(roster), 'names': [r['name'] for r in roster],
                'map': MAPS[n - 1], 'next_map': MAPS[n % 3], 'why': '' if n != 3 else 'its game process ended; starting it again',
                'reachable': n != 2, 'since': int(time.time()) - 3 * 3600 - n * 600, 'listed': True, 'roster': roster}
    m.server_status = status
    m.dash.note_restart(3, 'its game process ended')
    m.dash_tick()
    view = m.dashboard()
    check('the manager\'s dashboard: its servers, live, with players', len(view['servers']) == 3 and
          view['servers'][0]['roster'] and view['servers'][0]['round'] == {'round': 3, 'left': 0.62}, view['servers'][0])
    words = ' '.join(a['text'] for a in view['alerts'])
    check('alerts: a server down, one not reachable', 'is down' in words and 'cannot be reached' in words, view['alerts'])
    m.set_config({'public_page': {'on': True, 'port': 18297, 'names': True, 'title': '[EU] Reborn Test Servers'}})
    base = 'http://127.0.0.8:18297'

    def get(path):
        try:
            with urllib.request.urlopen(base + path, timeout=5) as r:
                return r.status, r.read().decode('utf-8'), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode('utf-8', 'replace'), dict(e.headers)
        except OSError as e:
            return 0, str(e), {}

    ok = False
    for _ in range(20):
        code, body, headers = get('/')
        if code == 200:
            ok = True
            break
        time.sleep(0.2)
    check('the public page answers on its own port', ok and 'dash.js' in body, (code, body[:200]))
    code, js, _ = get('/dash.js')
    check('and its drawing', code == 200 and 'window.Dash' in js)
    code, body, headers = get('/stats.json')
    stats = json.loads(body) if code == 200 else {}
    check('its numbers: the servers, the cards, the leaderboard, the network', code == 200 and stats.get('servers') and
          stats.get('leaderboard') and stats.get('network') and stats.get('title') == '[EU] Reborn Test Servers',
          (code, body[:200]))
    check('no address, no player id, no process, no alert in them', '203.0.113' not in body and 'A0000' not in body and
          '"procs"' not in body and '"alerts"' not in body and '"ip"' not in body, body[:300])
    check('a content security policy, no framing elsewhere', "default-src 'none'" in headers.get('Content-Security-Policy', ''))
    for path in ('/api/state', '/api/moderation', '/api/dashboard', '/index.html/../manager.py', '/../server.json'):
        code, body, _ = get(path)
        check('the public page serves nothing else: %s' % path, code == 404, (code, body[:100]))
    if not serve:
        codes = [get('/stats.json')[0] for _ in range(130)]
        check('at most 120 requests a minute from one address', 429 in codes and codes.count(200) <= 120,
              codes.count(429))
    m.set_config({'public_page': {'on': True, 'port': 18297, 'names': False, 'title': ''}})
    time.sleep(5.5)
    body = get('/stats.json')[1] if not serve else ''
    if not serve:
        check('names off: the public numbers name nobody', not any(n in body for n in NAMES),
              [n for n in NAMES if n in body][:3])
    if serve:
        m.set_config({'public_page': {'on': True, 'port': 18297, 'names': True, 'title': '[EU] Reborn Test Servers'}})
        import threading
        srv = manager.Server(('127.0.0.1', 18296), manager.Handler)  # the manager's page answers on 127.0.0.1 only
        manager.Handler.manager = m
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        print('manager page http://127.0.0.1:18296/  public page http://127.0.0.8:18297/  (Ctrl+C to stop)', flush=True)
        try:
            while True:
                time.sleep(2)
                m.dash_tick()
        except KeyboardInterrupt:
            pass
    m.cfg['public_page']['on'] = False
    m.public_page()


def main():
    serve = '--serve' in sys.argv
    tmp = tempfile.mkdtemp(prefix='bf-dashboard-')
    try:
        unit(os.path.join(tmp, 'unit'))
        manager_part(tmp, serve)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('%d failed' % len(FAILS) if FAILS else 'all passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
