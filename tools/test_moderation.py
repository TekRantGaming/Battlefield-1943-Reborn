"""Server moderation without the game (server/bans.py, docs/MODERATION.md): a backend of its own on 127.0.0.5 (ports
18741, 18746, 18750; nothing of a running backend is touched) with a Reborn network ban list, real hosts (gmhost.py,
no game behind them) with a server's own ban list, players logged in from loopback addresses of their own (FESL login,
the registry's "reserve", Theater's EGAM), and a LAN only backend (127.0.0.6, ports 18841, 18846, 18850) standing for a
server on its own backend. A TEST official key is made here.

Checks: the ban list (matching by id, account and address, lengths, an address's 30 days, kicks of 5 minutes, texts the
games can show), the network ban refused at reserve and at EGAM on official and community servers but not on the LAN
backend, a server's own ban refused on that server only, a kick's minutes, the host told of refusals (its moderation
log), a player on a server removed when a network ban is made, the signed admin lookup, and the manager's commands.

    python -I tools/test_moderation.py [--keep]
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'server'))
import bans  # noqa: E402
import gmhost  # noqa: E402
import plasma  # noqa: E402

PY = sys.executable
HOST, FESL, THEATER, REG = '127.0.0.5', 18741, 18746, 18750
LAN, LAN_FESL, LAN_THEATER, LAN_REG = '127.0.0.6', 18841, 18846, 18850
CLIENT = {'version': '0.4.8', 'platform': 'windows', 'notice': 1, 'queue': 1}
FAILS = []


def check(what, ok, detail=''):
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', what, '' if ok else '   ' + str(detail)[:500]))
    if not ok:
        FAILS.append(what)


def read(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def wait_for(test, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            if test():
                return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def ask(obj, source, host=HOST, port=REG):
    s = socket.socket()
    s.settimeout(5)
    s.bind((source, 0))
    s.connect((host, port))
    s.sendall((json.dumps(obj) + '\n').encode())
    line = s.makefile().readline()
    s.close()
    return json.loads(line)


# ---- a player's game: FESL login and Theater's EGAM, as the game sends them
def packets(sock):
    buf = b''
    while True:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            return
        if not data:
            return
        buf += data
        while len(buf) >= 12:
            length = struct.unpack('>I', buf[8:12])[0]
            if len(buf) < length:
                break
            kind = buf[:4].decode('latin-1')
            yield kind, plasma.parse_payload(buf[12:length])
            buf = buf[length:]


class Player:
    def __init__(self, name, pid_hex, source, host=HOST, fesl=FESL, theater=THEATER):
        self.name, self.pid, self.source, self.host, self.theater_port = name, pid_hex, source, host, theater
        self.fesl = socket.socket()
        self.fesl.settimeout(5)
        self.fesl.bind((source, 0))
        self.fesl.connect((host, fesl))
        self.fesl.sendall(plasma.build('fsys', plasma.REQUEST, 1, {'TXN': 'Hello', 'clientString': 'beach-360',
                                                                    'sku': 'xenon'}))
        xuid = 0x0009000000000000 | int(pid_hex, 16)
        self.fesl.sendall(plasma.build('acct', plasma.REQUEST, 2, {'TXN': 'NuXBL360Login', 'gamertag': name,
                                                                    'xuid': str(xuid)}))
        self.lkey = ''
        for kind, f in packets(self.fesl):
            if kind == 'acct' and f.get('lkey'):
                self.lkey = f['lkey']
                break

    def egam(self, gid):
        """Theater's EGAM for that game: the EGEG fields (the server's address), or None when none came in 3 s."""
        t = socket.socket()
        t.settimeout(3)
        t.bind((self.source, 0))
        t.connect((self.host, self.theater_port))
        t.sendall(plasma.build('CONN', 0x40000000, 0, {'TID': 1, 'PROT': 2}))
        t.sendall(plasma.build('USER', 0x40000000, 0, {'TID': 2, 'LKEY': self.lkey}))
        t.sendall(plasma.build('EGAM', 0x40000000, 0, {'TID': 3, 'LID': 1, 'GID': gid, 'PORT': 1000,
                                                        'R-XNADDR': ''}))
        got = None
        for kind, f in packets(t):
            if kind == 'EGEG':
                got = f
                break
        t.close()
        return got

    def reserve(self, gid, host=HOST, port=REG):
        return ask(dict(CLIENT, op='reserve', gid=gid), self.source, host, port)

    def close(self):
        self.fesl.close()


def unit():
    now = 1_800_000_000
    tmp = tempfile.mkdtemp(prefix='bf-bans-')
    path = os.path.join(tmp, 'bans.json')
    bl = bans.BanList(path)
    e = bl.add('ban', ids=['1a2b3c4d'], ips=['10.0.0.9'], names=['Cheater'], reason='Aimbot', by='admin',
               seconds=7 * 86400, now=now)
    check('a ban by id matches its id (any case)', (bl.match('1A2B3C4D', now=now + 1) or {}).get('id') == e['id'])
    check('and its address', (bl.match('', '', '10.0.0.9', now=now + 1) or {}).get('id') == e['id'])
    check('not another player', bl.match('99999999', '', '10.0.0.10', now=now + 1) is None)
    check('not after it ends', bl.match('1a2b3c4d', now=now + 7 * 86400 + 1) is None)
    p = bl.add('ban', ids=['00000042'], ips=['10.0.0.20'], reason='', seconds=0, now=now)
    check('a permanent ban holds a year later by id', (bl.match('42', now=now + 365 * 86400) or {}).get('id') == p['id'])
    check('its address only for 30 days', (bl.match('', '', '10.0.0.20', now=now + 29 * 86400) or {}).get('id') == p['id'] and
          bl.match('', '', '10.0.0.20', now=now + 31 * 86400) is None)
    k = bl.add('kick', ids=['00000077'], seconds=999999, now=now)
    check('a kick lasts 5 minutes whatever was asked', k['until'] == now + 300, k)
    m = bl.add('mute', ids=['00000088'], now=now, seconds=3600)
    check('a mute does not keep a player out', bl.match('00000088', now=now) is None and
          (bl.match('00000088', kinds=('mute',), now=now) or {}).get('id') == m['id'])
    check('a ban for the same player replaces the old one', len([x for x in bl.add('ban', ids=['1a2b3c4d'], now=now,
                                                                                    reason='again') and bl.entries
                                                                  if '1A2B3C4D' in x['ids']]) == 1)
    a = bl.add('ban', accounts=['ab' * 32], now=now, servers=[2])
    check('an account ban for server 2 holds there', (bl.match('', 'AB' * 32, server=2, now=now) or {}).get('id') == a['id'])
    check('and not on server 1', bl.match('', 'ab' * 32, server=1, now=now) is None)
    for bad, why in (('say "hi"', 'quotes'), ('{x}', 'braces'), ('x' * 121, 'too long'), ('a reserved seat', 'reserved')):
        try:
            bl.add('ban', ids=['00000099'], reason=bad, now=now)
            check('a reason with %s is refused' % why, False)
        except ValueError:
            check('a reason with %s is refused' % why, True)
    try:
        bl.add('ban', ids=['zz'], now=now)
        check('a ban with no player is refused', False)
    except ValueError:
        check('a ban with no player is refused', True)
    check('lengths', [bans.length_seconds(x) for x in ('1h', '1d', '7d', '30d', 'permanent', '90m')] ==
          [3600, 86400, 604800, 2592000, 0, 5400])
    again = bans.BanList(path)
    check('the file reads back the same entries', [x['id'] for x in again.entries] == [x['id'] for x in bl.entries])
    t1, m1 = bans.refusal(e, True, now)
    t2, m2 = bans.refusal(dict(e, until=0), False, now)
    t3, m3 = bans.refusal(k, False, now)
    check('the network refusal names the network, the reason, the end and the Discord appeal',
          t1 == 'BANNED FROM THE REBORN NETWORK' and 'Aimbot.' in m1 and 'Ends:' in m1 and 'Reborn Discord' in m1, m1)
    check('a server ban says it is that server\'s and how to appeal', t2 == 'BANNED FROM THIS SERVER' and
          'permanent' in m2 and "this server's admin" in m2, m2)
    check('a kick says when they can join again', t3 == 'KICKED FROM THIS SERVER' and '5 minutes' in m3, m3)
    check('no refusal text holds a quote, a brace or "reserved"',
          not any(c in t + mm for t, mm in ((t1, m1), (t2, m2), (t3, m3)) for c in '"{}\\') and
          'reserved' not in (m1 + m2 + m3).lower())
    bl.entries = [x for x in bl.entries]
    bl.save(now + 400)
    check('an ended kick is dropped when the list is saved', not any(x['kind'] == 'kick' for x in bl.entries))
    log = bans.ModerationLog(os.path.join(tmp, 'log.jsonl'))
    log.write(action='ban', at=int(time.time()) - 91 * 86400, names=['Old'])
    log.write(action='ban', names=['New'])
    log.trim()
    check('the moderation log keeps 90 days', [x.get('names') for x in log.read()] == [['New']], log.read())
    shutil.rmtree(tmp, ignore_errors=True)


def host_in_process():
    """The host's part (gmhost.py GameHost), with a stand-in socket: a ban made while a player plays takes them off
    (C_KICKED, then the link dropped and the address kept out), a mute stops their chat reaching anyone, and a client
    that never showed a ticket gets no packet through to the game server."""

    class Sent:
        def __init__(self):
            self.out = []

        def sendto(self, data, addr):
            self.out.append((data, addr))

        def is_closing(self):
            return False

        def close(self):
            pass

    tmp = tempfile.mkdtemp(prefix='bf-host-')
    try:
        host = gmhost.GameHost(('127.0.0.77', 1000), ('127.0.0.77', 9), 901, 7, 'Test', None)
        host.transport = Sent()
        host.server_number = 1
        host.bans = bans.BanList(os.path.join(tmp, 'bans.json'))
        host.modlog = bans.ModerationLog(os.path.join(tmp, 'log.jsonl'))

        def player(name, pid_hex, ip, port):
            peer = gmhost.Peer(host, (ip, port))
            peer.player = gmhost.Player(1, name, 0x0009000000000000 | int(pid_hex, 16), b'\0' * 36, 't')
            peer.state = 'joined'
            host.peers[peer.addr] = peer
            c = gmhost.CommsClient(len(host.comms.clients) + 1, (ip, port + 1), name)
            host.comms.clients[c.addr] = c
            return peer, c

        bad, bad_c = player('Griefer', 'C0000003', '10.0.0.3', 4000)
        good, good_c = player('Friendly', 'B0000002', '10.0.0.2', 5000)
        loud, loud_c = player('Loud', 'E0000005', '10.0.0.5', 6000)
        host.bans.add('ban', ids=['C0000003'], reason='Team killing', seconds=3600)
        host.bans.add('mute', ids=['E0000005'], reason='Shouting', seconds=3600)
        host.bans.reload(force=True)
        host.enforce()
        kicked = [d for d, a in host.transport.out if a == bad_c.addr and d[:9] == gmhost.COMMS + bytes([gmhost.C_KICKED])]
        check('a banned player on the server is sent C_KICKED with the title (a v0.4.8 game leaves and shows it)',
              len(kicked) == 3 and b'BANNED FROM THIS SERVER' in kicked[0], kicked[:1])
        check('and the message box too (an older game)', any(a == bad_c.addr and d[8] == gmhost.C_BROADCAST
                                                             for d, a in host.transport.out))
        check('the others are not', bad.addr in host.removing and good.addr not in host.removing and
              loud.addr not in host.removing)
        host.moderation_tick(time.monotonic() + 3)
        check('two seconds later its link is dropped', bad.addr not in host.peers)
        host.packet(b'\0\0\0\0\0\0', bad.addr)
        check('and its address gets no answer for a while', bad.addr not in host.peers and bad.addr in host.blocked)
        check('the moderation log says it', '"action":"removed"' in read(os.path.join(tmp, 'log.jsonl')))

        def chat(c, text):
            host.transport.out.clear()
            raw = text.encode()
            host.comms.handle(bytes([gmhost.C_CHAT]) + struct.pack('>HHBB', c.id, int(time.time() * 1000) & 0xFFFF,
                                                                   gmhost.CHAT_ALL, len(raw)) + raw, c.addr)
            return [a for d, a in host.transport.out if d[8] == gmhost.C_CHAT_RELAY]

        check('a muted player\'s chat reaches nobody', chat(loud_c, 'LOUD NOISES') == [])
        check('and they are told why (the message box)', any(a == loud_c.addr and d[8] == gmhost.C_BROADCAST and
                                                              b'muted' in d for d, a in host.transport.out))
        check('another player\'s chat still goes to everyone linked', set(chat(good_c, 'hello')) ==
              set(host.comms.clients))
        stranger = gmhost.Peer(host, ('10.0.0.9', 7000))
        host.peers[stranger.addr] = stranger
        before = host.unlet
        stranger.to_server(b'\x01\x02')
        check('a client that never showed a ticket gets no packet to the game server', host.unlet == before + 1 and
              stranger.server is None)
        host.network_out['B0000002'] = {'kind': 'network', 'title': 'BANNED FROM THE REBORN NETWORK', 'message': 'x',
                                        'until': time.time() + 600}
        host.enforce()
        check('the backend\'s "remove" (a network ban) takes its player off too', good.addr in host.removing)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    keep = '--keep' in sys.argv
    unit()
    host_in_process()
    base = tempfile.mkdtemp(prefix='bf-moderation-')
    data = os.path.join(base, 'data')
    os.makedirs(os.path.join(data, 'logs'))
    official_key = os.urandom(32).hex().encode()  # a TEST key, made here
    with open(os.path.join(data, 'official.key'), 'wb') as f:
        f.write(official_key)
    netbans_file = os.path.join(data, 'network_bans.json')
    community_bans = os.path.join(base, 'community_bans.json')
    community_log = os.path.join(base, 'community_log.jsonl')
    procs = []

    def start(args, out, env=None):
        p = subprocess.Popen(args, stdout=open(out, 'w'), stderr=subprocess.STDOUT, env=env or dict(os.environ),
                             cwd=ROOT, creationflags=0x08000000 if os.name == 'nt' else 0)
        procs.append(p)
        return p

    players = []
    try:
        backend_log = os.path.join(base, 'backend.log')
        start([PY, '-I', os.path.join(ROOT, 'server', 'plasma.py'), '--host', HOST, '--theater-host', HOST,
               '--port', str(FESL), '--theater-port', str(THEATER), '--registry-port', str(REG), '--login-delay', '0',
               '--no-create', '--log', backend_log, '--official-key-file', os.path.join(data, 'official.key'),
               '--network-bans', netbans_file], os.path.join(base, 'backend.out'))
        check('the test backend answers', wait_for(lambda: ask({'op': 'list'}, '127.0.0.30') is not None, 20))
        lan_log = os.path.join(base, 'lan.log')
        start([PY, '-I', os.path.join(ROOT, 'server', 'plasma.py'), '--host', LAN, '--theater-host', LAN, '--lan',
               '--port', str(LAN_FESL), '--theater-port', str(LAN_THEATER), '--registry-port', str(LAN_REG),
               '--login-delay', '0', '--no-create', '--log', lan_log], os.path.join(base, 'lan.out'))
        check('the LAN backend answers', wait_for(lambda: ask({'op': 'list'}, '127.0.0.30', LAN, LAN_REG), 20))

        def host(tag, port, backend=(HOST, REG), official=False, bans_file='', modlog=''):
            args = [PY, '-I', os.path.join(ROOT, 'server', 'gmhost.py'), '--backend', '%s:%d' % backend,
                    '--listen', '127.0.0.77:%d' % port, '--game-server', '127.0.0.77:9', '--name', 'Test %s' % tag,
                    '--log', os.path.join(base, 'host_%s.log' % tag), '--server-number', '1',
                    '--status-file', os.path.join(base, 'status_%s.json' % tag)]
            if official:
                args += ['--official-key-file', os.path.join(data, 'official.key')]
            if bans_file:
                args += ['--bans-file', bans_file, '--moderation-log', modlog]
            start(args, os.path.join(base, 'host_%s.out' % tag))

        cbans = bans.BanList(community_bans)
        community_banned = cbans.add('ban', ids=['C0000003'], names=['Griefer'], reason='Team killing', by='owner',
                                     seconds=86400)
        host('official', 18781, official=True)
        host('community', 18782, bans_file=community_bans, modlog=community_log)
        host('other', 18783)
        host('lan', 18784, backend=(LAN, LAN_REG))
        tags = ('official', 'community', 'other', 'lan')
        check('four hosts registered (official, community with its own ban list, another community, one on a LAN '
              'backend)', wait_for(lambda: all('registered with the backend' in read(os.path.join(base, 'host_%s.log' % t))
                                              for t in tags), 40),
              [t for t in tags if 'registered' not in read(os.path.join(base, 'host_%s.log' % t))])
        check('the community host told the backend its ban list (1 entry)',
              wait_for(lambda: 'its ban list has 1 entry' in read(backend_log), 10), read(backend_log)[-800:])
        s = {x['name']: x for x in ask({'op': 'list'}, '127.0.0.30')['servers']}
        g_off, g_com, g_oth = s['Test official']['gid'], s['Test community']['gid'], s['Test other']['gid']
        g_lan = ask({'op': 'list'}, '127.0.0.30', LAN, LAN_REG)['servers'][0]['gid']

        # ---- players: a network-banned one, a fine one, one banned on the community server only
        p_net = Player('Cheater', 'A0000001', '127.0.0.31')
        p_ok = Player('Friendly', 'B0000002', '127.0.0.32')
        p_com = Player('Griefer', 'C0000003', '127.0.0.33')
        players += [p_net, p_ok, p_com]
        check('three players logged in', all(p.lkey for p in players), [p.lkey for p in players])

        nbans = bans.BanList(netbans_file)
        net_ban = nbans.add('ban', ids=['A0000001'], ips=['127.0.0.31'], names=['Cheater'], reason='Aimbot',
                            by='TekRantGaming', seconds=7 * 86400)
        time.sleep(1.5)

        r = p_net.reserve(g_off)
        check('network ban: refused at reserve on an official server, with the pop-up\'s title and message',
              r.get('op') == 'banned' and r.get('title') == 'BANNED FROM THE REBORN NETWORK' and 'Aimbot' in
              r.get('message', '') and 'Reborn Discord' in r.get('message', '') and r.get('network') is True, r)
        r = p_net.reserve(g_com)
        check('network ban: refused at reserve on a registered community server', r.get('op') == 'banned' and
              r.get('network') is True, r)
        check('a refusal never holds the word "reserved" (games before the gate read it as a yes)',
              'reserved' not in json.dumps(r).lower())
        found = ask(dict(CLIENT, op='find', address='127.0.0.77', port=18782), '127.0.0.31')
        check('Direct connect to a registered server\'s address still finds it (the join is what is refused)',
              (found.get('server') or {}).get('gid') == g_com, found)
        check('network ban: no server address at EGAM (a game that skipped reserve)', p_net.egam(g_off) is None and
              p_net.egam(g_com) is None)
        check('the backend log says why', 'EGAM refused: Cheater (A0000001) is banned on the Reborn network' in
              read(backend_log))
        check('the community host\'s moderation log says a network-banned player was refused, and nothing more',
              wait_for(lambda: '"network":true' in read(community_log) and 'Aimbot' not in read(community_log), 5),
              read(community_log))
        lan_player = Player('Cheater', 'A0000001', '127.0.0.31', LAN, LAN_FESL, LAN_THEATER)
        players.append(lan_player)
        r = lan_player.reserve(g_lan, LAN, LAN_REG)
        check('network ban: a server on its own backend (LAN only, Direct connect) lets the player in',
              r.get('op') == 'reserved', r)
        check('and its EGAM gives the address', lan_player.egam(g_lan) is not None)
        check('another player joins the official server', p_ok.reserve(g_off).get('op') == 'reserved' and
              p_ok.egam(g_off) is not None)

        r = p_com.reserve(g_com)
        check('server ban: refused on that community server with its own title and reason',
              r.get('op') == 'banned' and r.get('title') == 'BANNED FROM THIS SERVER' and 'Team killing' in
              r.get('message', '') and "this server's admin" in r.get('message', '') and r.get('network') is False, r)
        check('server ban: no address at EGAM there', p_com.egam(g_com) is None)
        check('server ban: the same player joins another community server', p_com.reserve(g_oth).get('op') == 'reserved'
              and p_com.egam(g_oth) is not None)
        check('server ban: and the official server', p_com.reserve(g_off).get('op') == 'reserved')
        check('the community host logs the refusal with its ban', wait_for(
            lambda: community_banned['id'] in read(community_log) and '"action":"refused"' in read(community_log), 5),
            read(community_log))

        # ---- a kick on the community server: refused for its minutes, then the list is changed back
        cbans.add('kick', ids=['B0000002'], names=['Friendly'], reason='Language')
        check('the host sends its new list at once', wait_for(lambda: 'its ban list has 2 entries' in read(backend_log), 5))
        r = p_ok.reserve(g_com)
        check('a kicked player is refused for 5 minutes on that server', r.get('op') == 'banned' and
              r.get('title') == 'KICKED FROM THIS SERVER' and '5 minutes' in r.get('message', ''), r)
        cbans.remove(cbans.match('B0000002')['id'])
        check('an unban is sent too', wait_for(lambda: p_ok.reserve(g_com).get('op') == 'reserved', 5))

        # ---- a network ban made while the player plays: the host is told to remove them
        fake = socket.create_connection((HOST, REG), timeout=5)
        fake.sendall((json.dumps({'op': 'register', 'name': 'Test fake host', 'port': 18785, 'map': 'Wake Island',
                                  'max_players': 24, 'address': ['127.0.0.77', 18785]}) + '\n').encode())
        ff = fake.makefile()
        g_fake = json.loads(ff.readline())['gid']
        p_late = Player('Latecomer', 'D0000004', '127.0.0.34')
        players.append(p_late)
        check('a player joins the fake host', p_late.reserve(g_fake).get('op') == 'reserved' and
              p_late.egam(g_fake) is not None)
        fake.sendall((json.dumps({'op': 'status', 'players': 1, 'map': 'Wake Island', 'names': ['Latecomer']}) +
                      '\n').encode())
        time.sleep(0.5)
        nbans.add('ban', ids=['D0000004'], names=['Latecomer'], reason='Exploit', by='TekRantGaming', seconds=0)
        fake.settimeout(8)
        got = None
        try:
            while True:
                m = json.loads(ff.readline())
                if m.get('op') == 'remove':
                    got = m
                    break
        except (OSError, ValueError):
            pass
        check('a new network ban: the player\'s server is told to remove them (player id only, no address)',
              got and got.get('player_id') == 'D0000004' and 'BANNED FROM THE REBORN NETWORK' == got.get('title') and
              '127.0.0' not in json.dumps(got), got)
        fake.close()

        # ---- removing the network ban lets the player in again
        nbans.remove(net_ban['id'])
        time.sleep(1.5)
        check('an unbanned player joins again', p_net.reserve(g_off).get('op') == 'reserved')

        # ---- the signed lookup, only for the official manager
        r = ask({'op': 'admin', 'signed': 'bf1943-admin|1|0123456789abcdef|%d|players' % int(time.time()),
                 'sig': '00' * 64}, '127.0.0.30')
        check('an unsigned player lookup is refused', r.get('ok') is False, r)
        r = ask(gmhost.sign_admin(official_key, 'players'), '127.0.0.30')
        names = {x['name']: x for x in r.get('players', [])}
        check('the signed lookup lists who is online with player id and address',
              r.get('ok') and names.get('Friendly', {}).get('id') == 'B0000002' and
              names.get('Friendly', {}).get('ip') == '127.0.0.32', r)
        r = ask(gmhost.sign_admin(official_key, 'lookup:grief'), '127.0.0.30')
        check('lookup by part of a name', [x['name'] for x in r.get('players', [])] == ['Griefer'], r)

        # ---- the manager's commands (an official package's manager, its servers not started)
        mgr_root = os.path.join(base, 'mgr')
        shutil.copytree(os.path.join(ROOT, 'server'), os.path.join(mgr_root, 'server'),
                        ignore=shutil.ignore_patterns('__pycache__'))
        with open(os.path.join(mgr_root, 'package.json'), 'w') as f:
            json.dump({'official': True}, f)
        with open(os.path.join(mgr_root, 'version.txt'), 'w') as f:
            f.write('0.4.8-test\n')
        with open(os.path.join(data, 'server.json'), 'w') as f:
            json.dump({'web_port': 18799, 'servers': [{'name': 'Test 1', 'port': 18786, 'autostart': False}],
                       'backend': HOST}, f)
        menv = dict(os.environ, BF_SERVER_ROOT=mgr_root, BF_SERVER_DATA=data, BF_SERVER_BACKEND_BIND=HOST,
                    BF_SERVER_NO_BROWSER="1", BF_SERVER_LOOPBACK_BASE="80", BF_SERVER_REGISTRY_PORT=str(REG))
        # Its own backend would take the test backend's ports: the manager is started with its servers off, and its
        # network list is the test backend's file (the same data folder).
        start([PY, '-I', os.path.join(mgr_root, 'server', 'manager.py'), 'run', '--no-browser', '--no-start',
               '--port', '18799'], os.path.join(base, 'manager.out'), menv)

        def mgr(*args):
            r = subprocess.run([PY, '-I', os.path.join(mgr_root, 'server', 'manager.py')] + list(args), env=menv,
                               capture_output=True, text=True, timeout=60)
            return (r.stdout + r.stderr).strip()

        check('the manager runs', wait_for(lambda: 'manager' in mgr('status'), 30), mgr('status'))
        out = mgr('ban', 'E0000005', '--length', '1d', '--reason', 'Spawn camping', '--by', 'Tester')
        check('manager.py ban <player id> adds a server ban', out.startswith('ban b-') and 'Spawn camping' in out, out)
        out = mgr('bans')
        check('manager.py bans lists it', 'E0000005' in out and 'Spawn camping' in out, out)
        out = mgr('ban', 'Griefer', '--network', '--reason', 'Team killing everywhere', '--by', 'Tester')
        check('manager.py ban <name> --network finds the player on the network and bans id, account and address',
              out.startswith('ban b-') and 'Griefer' in out, out)
        check('the network ban took the player\'s id and address', any(
            'C0000003' in e['ids'] and '127.0.0.33' in e['ips'] for e in bans.BanList(netbans_file).entries))
        time.sleep(1.5)
        check('and the backend refuses them at once', p_com.reserve(g_oth).get('title') == 'BANNED FROM THE REBORN NETWORK')
        out = mgr('ban', 'E0000005', '--reason', 'say "hi"')
        check('a reason the game cannot show is refused', 'cannot be shown' in out, out)
        out = mgr('mute', 'E0000005', '--length', '1h', '--reason', 'Shouting')
        check('manager.py mute', out.startswith('mute b-'), out)
        ban_id = [e['id'] for e in bans.BanList(os.path.join(data, 'bans.json')).entries if e['kind'] == 'ban'][0]
        out = mgr('unban', ban_id, '--by', 'Tester')
        check('manager.py unban', out.startswith('removed ban ' + ban_id), out)
        out = mgr('modlog')
        check('the moderation log has the actions, newest first, and no addresses',
              out.splitlines()[0].count('action=unban') == 1 and 'action=ban' in out and '127.0.0' not in out, out)
        log_text = read(os.path.join(data, 'moderation_log.jsonl'))
        check('no address in the log file', '127.0.0' not in log_text, log_text[-400:])
        out = mgr('kick', 'E0000005', '--network')
        check('a kick is not a network action', 'unrecognized' in out or 'error' in out.lower(), out)
    finally:
        for p in players:
            try:
                p.close()
            except Exception:
                pass
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(10)
            except Exception:
                p.kill()
        if keep:
            print('kept: %s' % base)
        else:
            shutil.rmtree(base, ignore_errors=True)
    print('%d failed' % len(FAILS) if FAILS else 'all passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
