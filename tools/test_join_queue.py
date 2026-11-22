"""The join queue without the game (server/plasma.py QUEUE, 0.4.7.4): a backend of its own on 127.0.0.5 (ports 18641,
18646, 18650; nothing of a running backend is touched), servers registered by hand, players from loopback addresses of
their own. Checks a full server's queue for games that say "queue": 1 (places in order, the same place asked again, its
turn, nobody past a waiting player, Close, a place given up when its game stops asking, a pick of another server, a
server that registers again), games without the queue (still "full", never a word read as a yes), the list's queue
counts, and the slot held for a player who got the server's address already (server_full, in process).

    python -I tools/test_join_queue.py
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'server'))
import plasma  # noqa: E402

PY = sys.executable
HOST, FESL, THEATER, REG = '127.0.0.5', 18641, 18646, 18650
NEW = {'version': '0.4.7.4', 'platform': 'windows', 'notice': 1, 'queue': 1}  # a game with the queue
OLD = {'version': '0.4.7.3', 'platform': 'windows', 'notice': 1}              # a 0.4.7.3 game
FAILS = []


def check(what, ok, detail=''):
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', what, '' if ok else '   ' + str(detail)[:400]))
    if not ok:
        FAILS.append(what)


def ask(obj, source):
    """One registry request from `source` (a loopback address of its own): the raw reply line."""
    s = socket.socket()
    s.settimeout(5)
    s.bind((source, 0))
    s.connect((HOST, REG))
    s.sendall((json.dumps(obj) + '\n').encode())
    line = s.makefile().readline()
    s.close()
    return line


def askj(obj, source):
    return json.loads(ask(obj, source))


class Game:
    """A player's game, signed in (FESL): its Quick Match is sent where the backend sends that player."""

    def __init__(self, pid, source):
        self.pid, self.source = pid, source
        self.s = socket.socket()
        self.s.settimeout(5)
        self.s.bind((source, 0))
        self.s.connect((HOST, FESL))
        self.buf = b''
        self.send('fsys', 1, {'TXN': 'Hello', 'clientString': 'beach-360', 'sku': 'xenon'})
        self.send('acct', 2, {'TXN': 'NuXBL360Login', 'gamertag': 'P' + pid,
                              'xuid': str(0x0009000000000000 | int(pid, 16))})
        self.wait('acct', 'NuXBL360Login')

    def send(self, kind, n, fields):
        self.s.sendall(plasma.build(kind, plasma.REQUEST, n, fields))

    def wait(self, kind, txn):
        while True:
            while len(self.buf) >= 12 and len(self.buf) >= int.from_bytes(self.buf[8:12], 'big'):
                length = int.from_bytes(self.buf[8:12], 'big')
                head, body, self.buf = self.buf[:4], self.buf[12:length], self.buf[length:]
                fields = plasma.parse_payload(body)
                if head.decode('latin-1') == kind and fields.get('TXN') == txn:
                    return fields
            data = self.s.recv(65536)
            if not data:
                return {}
            self.buf += data

    def reserve(self, gid, **more):
        return askj(dict(NEW, op='reserve', gid=gid, id=self.pid, **more), self.source)

    def quick_match(self):
        self.send('pnow', 7, {'TXN': 'Start', 'partition.partition': '/xenon/BEACH'})
        f = self.wait('pnow', 'Status')
        return [v for k, v in f.items() if k.startswith('props.{games}.') and k.endswith('.gid')]

    def close(self):
        self.s.close()


def shared_address(register, status):
    """Several players behind one address (a household, a LAN party): each has a pick, a slot on the way and a place
    in a queue of their own."""
    home, away = '127.0.0.31', '127.0.0.32'
    one, two = register('Home one', 18670, 24, 3), register('Home two', 18671, 24, 3)
    a, b = Game('AA000001', home), Game('BB000002', home)
    games = [a, b]
    try:
        a.reserve(one), b.reserve(two)
        got = a.quick_match(), b.quick_match()
        check('one address: each player goes to the server they picked', got == ([one], [two]), got)

        tight = register('Home tight', 18672, 3, 1)
        check('one address: two players pick the two free slots', a.reserve(tight)['op'] == 'reserved' and
              b.reserve(tight)['op'] == 'reserved')
        r = askj(dict(OLD, op='reserve', gid=tight), away)
        check('one address: both are on their way, so nobody else gets a slot', r['op'] == 'full', r)

        last = register('Home last', 18673, 2, 1)
        check('one address: the first player gets the last slot', a.reserve(last)['op'] == 'reserved')
        r = b.reserve(last)
        check('one address: the second does not get it as well, and waits', r['op'] == 'queued', r)
        got = a.quick_match()
        check('one address: the second waiting leaves the first\x27s pick', got == [last], got)
        check('one address: the same player asking again keeps their slot', a.reserve(last)['op'] == 'reserved')

        line = register('Home line', 18674, 2, 2)
        pa, pb = a.reserve(line), b.reserve(line)
        check('one address: two places in a queue', pa['op'] == pb['op'] == 'queued' and pa.get('token') != pb.get('token')
              and (pb.get('position'), pb.get('total')) == (2, 2), (pa, pb))
        check('one address: asked again, the same place', a.reserve(line).get('token') == pa.get('token'))
        ask(dict(op='queue_leave', token=pa.get('token', '')), home)
        r = askj(dict(NEW, op='queue', gid=line, token=pb.get('token', ''), id=b.pid), home)
        check('one address: one player leaving keeps the other\x27s place', r['op'] == 'queued' and r['position'] == 1, r)
        a.reserve(one)
        r = askj(dict(NEW, op='queue', gid=line, token=pb.get('token', ''), id=b.pid), home)
        check('one address: one player picking elsewhere keeps the other\x27s place', r['op'] == 'queued', r)
        status('Home line', 1)
        r = askj(dict(NEW, op='queue', gid=line, token=pb.get('token', ''), id=b.pid), home)
        check('one address: that player\x27s turn', r['op'] == 'reserved', r)
        got = a.quick_match(), b.quick_match()
        check('one address: the turn is theirs alone', got == ([one], [line]), got)

        full = register('Home full', 18675, 2, 2)
        r = askj(dict(OLD, op='reserve', gid=full, id=b.pid), home)
        got = a.quick_match()
        check('one address: a full server refusing one leaves the other\x27s pick', r['op'] == 'full' and got == [one], got)

        older = Game('CC000003', home)  # a game from before ids: it signs in, and its reserve names nobody
        games.append(older)
        askj(dict(NEW, op='reserve', gid=two), home)
        got = older.quick_match(), a.quick_match()
        check('one address: a game that says no id still gets its pick, and takes nobody\x27s', got == ([two], [one]), got)

        busy = register('Home busy', 18676, 24, 18)
        for i in range(12):
            askj(dict(OLD, op='reserve', gid=busy, id='D%07X' % i), '127.0.0.33')
        r = askj(dict(OLD, op='reserve', gid=busy), away)
        check('one address counts four picks on the way at most', r['op'] == 'reserved', r)
        crowd = register('Home crowd', 18677, 2, 2)
        ops = [askj(dict(NEW, op='reserve', gid=crowd, id='E%07X' % i), '127.0.0.34')['op'] for i in range(10)]
        check('one address holds eight places in queues at most', ops == ['queued'] * 8 + ['full'] * 2, ops)
        r = askj(dict(NEW, op='reserve', gid=crowd, id='F0000001'), away)
        check('and another address still gets its place', r['op'] == 'queued' and r['position'] == 9, r)
        for i in range(70):
            askj(dict(NEW, op='reserve', gid=two, id='A%07X' % i), '127.0.0.35')
        late = Game('A%07X' % 69, '127.0.0.35')
        early = Game('A%07X' % 0, '127.0.0.35')
        games.extend([late, early])
        got = late.quick_match(), early.quick_match()
        check('one address keeps 64 picks at most, the newest', got[0] == [two] and got[1] != [two], got)
    finally:
        for g in games:
            g.close()


A, B, C, D, E = '127.0.0.11', '127.0.0.12', '127.0.0.13', '127.0.0.14', '127.0.0.15'


def in_process():
    """server_full's count of players on their way (no backend: plasma's own tables)."""
    now = time.time()
    plasma.GAMES.clear(), plasma.PICKS.clear(), plasma.ARRIVED.clear(), plasma.QUEUE.clear()
    plasma.GAMES['x'] = {'GID': 'x', 'MAX-PLAYERS': 2, 'AP': 1, 'status_at': now}
    # A picked 5 s ago with one player in; its game got the address 4 s ago and the host has counted it since; then
    # another player left (still 1). Until 0.4.7.4 this counted A on its way: full for nobody.
    one, two, other = ('1.0.0.1', ''), ('1.0.0.2', ''), ('1.0.0.9', '')
    plasma.PICKS[one] = ('x', now - 5, 1, plasma.JOIN_WINDOW)
    plasma.ARRIVED[one] = ('x', now - 4)
    full, players, arriving, most = plasma.server_full('x', other)
    check('a pick whose game has the address and was counted is not on its way', not full and arriving == 0,
          (full, players, arriving, most))
    plasma.GAMES['x']['status_at'] = now - 3.5  # the host's last status came before the player could be in its count
    full, players, arriving, most = plasma.server_full('x', other)
    check('a pick whose game has the address but no status since is still on its way', full and arriving == 1,
          (full, players, arriving, most))
    plasma.ARRIVED.clear()
    plasma.GAMES['x']['status_at'] = now
    full, _, arriving, _ = plasma.server_full('x', other)
    check('a pick still without the address is on its way', full and arriving == 1, arriving)
    plasma.PICKS[one] = ('x', now - 40, 1, plasma.JOIN_WINDOW)
    check('a pick older than its window is not', not plasma.server_full('x', other)[0])
    plasma.PICKS[one] = ('x', now - 40, 1, plasma.ADMIT_WINDOW)
    check('a player let in from the queue is on its way for its longer window', plasma.server_full('x', other)[0])
    plasma.PICKS.clear()
    plasma.QUEUE.append({'token': 't', 'gid': 'x', 'address': two[0], 'id': '', 'server': ('h', 1), 'since': now,
                         'seen': now})
    check('a waiting player takes the free place from a player not waiting', plasma.server_full('x', other)[0])
    check('and not from itself (first in the queue)', not plasma.server_full('x', two, 0)[0])
    housemate = (two[0], 'BB000002')
    check('a waiting player takes it from a housemate too', plasma.server_full('x', housemate)[0])
    plasma.QUEUE.clear()
    # Two housemates picked; the first arrived and was counted: only the first's pick is off the way.
    plasma.GAMES['x'].update({'MAX-PLAYERS': 3, 'AP': 1, 'status_at': now})
    plasma.PICKS[('1.0.0.1', 'AA000001')] = ('x', now - 6, 1, plasma.JOIN_WINDOW)
    plasma.PICKS[('1.0.0.1', 'BB000002')] = ('x', now - 6, 1, plasma.JOIN_WINDOW)
    plasma.ARRIVED[('1.0.0.1', 'AA000001')] = ('x', now - 5)
    _, _, arriving, _ = plasma.server_full('x', other)
    check('one housemate arriving leaves the other on the way', arriving == 1, arriving)
    plasma.GAMES.clear(), plasma.PICKS.clear(), plasma.ARRIVED.clear()


def main():
    in_process()
    tmp = tempfile.mkdtemp(prefix='bf-queue-')
    backend = subprocess.Popen([PY, '-I', os.path.join(ROOT, 'server', 'plasma.py'), '--host', HOST, '--port', str(FESL),
                                '--theater-port', str(THEATER), '--registry-port', str(REG), '--login-delay', '0',
                                '--no-create', '--log', os.path.join(tmp, 'plasma.log')],
                               cwd=ROOT, stdout=open(os.path.join(tmp, 'plasma.out'), 'w'), stderr=subprocess.STDOUT)
    servers = {}
    try:
        for _ in range(50):
            try:
                socket.create_connection((HOST, REG), timeout=1).close()
                break
            except OSError:
                time.sleep(0.2)

        def register(name, port, most, players):
            s = socket.create_connection((HOST, REG), timeout=5)
            s.sendall((json.dumps({'op': 'register', 'name': name, 'port': port, 'map': 'Wake Island',
                                   'max_players': most, 'tick': 60}) + '\n').encode())
            f = s.makefile()
            reply = json.loads(f.readline())
            servers[name] = (s, f)
            status(name, players)
            return reply['gid']

        def status(name, players):
            servers[name][0].sendall((json.dumps({'op': 'status', 'players': players, 'map': 'Wake Island'}) +
                                      '\n').encode())
            time.sleep(0.3)

        def drop(name):  # the server's link ends (its file too, or the socket stays open)
            s, f = servers.pop(name)
            f.close(), s.close()
            time.sleep(0.5)

        full = register('Full one', 18660, 2, 2)
        other = register('Other one', 18661, 24, 3)
        time.sleep(0.3)

        raw = ask(dict(OLD, op='reserve', gid=full), C)
        check('a game without the queue: full, as before', json.loads(raw)['op'] == 'full', raw)
        a = askj(dict(NEW, op='reserve', gid=full), A)
        check('a game with the queue: a place, 1 of 1', a['op'] == 'queued' and (a['position'], a['total']) == (1, 1), a)
        raw = ask(dict(NEW, op='reserve', gid=full), B)
        b = json.loads(raw)
        check('the next one: 2 of 2', b['op'] == 'queued' and (b['position'], b['total']) == (2, 2), b)
        check('no queue reply holds the word a 0.4.6 game reads as a yes', 'reserved' not in raw, raw)
        again = askj(dict(NEW, op='reserve', gid=full), A)
        check('asking again keeps the place', again['op'] == 'queued' and again['token'] == a['token'] and
              again['position'] == 1, again)
        p = askj({'op': 'queue', 'gid': full, 'token': b['token']}, B)
        check('the queue op says the place', p['op'] == 'queued' and (p['position'], p['total']) == (2, 2), p)
        p = askj({'op': 'queue', 'gid': full, 'token': b['token']}, A)
        check('a token from another address is no place', p['op'] == 'queue_lost', p)
        listed = askj(dict(NEW, op='list'), C)
        row = [s for s in listed['servers'] if s['gid'] == full]
        check('the list says how many wait', row and row[0].get('queue') == 2, row)
        raw = ask(dict(OLD, op='reserve', gid=full), C)
        check('a game without the queue is told players wait', json.loads(raw)['op'] == 'full' and
              '2 players are waiting' in raw and 'reserved' not in raw, raw)

        status('Full one', 1)  # a place opens
        p = askj({'op': 'queue', 'gid': full, 'token': b['token']}, B)
        check('the second in the queue does not get the first one\'s place', p['op'] == 'queued' and p['position'] == 2, p)
        raw = ask(dict(OLD, op='reserve', gid=full), C)
        check('nor does a game without the queue', json.loads(raw)['op'] == 'full', raw)
        raw = ask(dict(NEW, op='reserve', gid=full), D)
        d = json.loads(raw)
        check('nor a new pick with the queue: it waits at the end, 3 of 3', d['op'] == 'queued' and
              (d['position'], d['total']) == (3, 3), d)
        p = askj({'op': 'queue', 'gid': full, 'token': a['token']}, A)
        check('the first one\'s turn: reserved', p['op'] == 'reserved' and p['gid'] == full, p)
        p = askj({'op': 'queue', 'gid': full, 'token': b['token']}, B)
        check('the others move up while it is on its way (1 of 2)', p['op'] == 'queued' and
              (p['position'], p['total']) == (1, 2), p)
        status('Full one', 2)  # it arrived

        p = askj({'op': 'queue_leave', 'token': b['token']}, B)
        check('Close leaves the queue', p['op'] == 'queue_left', p)
        p = askj({'op': 'queue', 'gid': full, 'token': d['token']}, D)
        check('the one behind moves up (1 of 1)', p['op'] == 'queued' and (p['position'], p['total']) == (1, 1), p)
        p = askj({'op': 'queue', 'gid': full, 'token': b['token']}, B)
        check('a place left is gone', p['op'] == 'queue_lost', p)

        e = askj(dict(NEW, op='reserve', gid=full), E)
        check('E waits 2 of 2', e['op'] == 'queued' and e['position'] == 2, e)
        p = askj(dict(NEW, op='reserve', gid=other), E)
        check('a pick of another server with room: reserved, and the queue left', p['op'] == 'reserved', p)
        p = askj({'op': 'queue', 'gid': full, 'token': e['token']}, E)
        check('its old place is gone', p['op'] == 'queue_lost', p)

        # A server whose link drops and registers again (a new game id): its queue waits on for it.
        drop('Full one')
        p = askj({'op': 'queue', 'gid': full, 'token': d['token']}, D)
        check('while its server is away the place stays', p['op'] == 'queued' and p['position'] == 1, p)
        full2 = register('Full one', 18660, 2, 2)
        p = askj({'op': 'queue', 'gid': full, 'token': d['token']}, D)
        check('the server registered again: the place is in its new game', p['op'] == 'queued' and p['gid'] == full2, p)

        # A place whose game stops asking: given up after QUEUE_TIMEOUT.
        print('waiting %d s for a place to time out' % (plasma.QUEUE_TIMEOUT + 2))
        time.sleep(plasma.QUEUE_TIMEOUT + 2)
        listed = askj(dict(NEW, op='list'), C)
        row = [s for s in listed['servers'] if s['gid'] == full2]
        check('the game that stopped asking lost its place', row and row[0].get('queue') == 0, row)
        p = askj({'op': 'queue', 'gid': full2, 'token': d['token']}, D)
        check('and is told so (it asks with reserve again)', p['op'] == 'queue_lost', p)

        drop('Other one')
        p = askj({'op': 'queue', 'gid': other, 'token': 'nope'}, D)
        check('asking for a place in a server that is gone: its words', p['op'] == 'gone' and p.get('message'), p)
        log = open(os.path.join(tmp, 'plasma.log'), encoding='utf-8').read()
        for words in ('queued for game', 'let in to game', 'left the queue of game', 'gave up its place'):
            check('the log says "%s"' % words, words in log)
        shared_address(register, status)
    finally:
        for s, f in servers.values():
            f.close(), s.close()
        backend.kill()
    print('backend log:', os.path.join(tmp, 'plasma.log'))
    print('%d failed' % len(FAILS) if FAILS else 'all passed')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
