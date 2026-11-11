"""Exploratory EA Plasma backend for the Xbox 360 build of Battlefield 1943: FESL (login, accounts) on 18241 and
Theater (game lists, joining) on 18246. Our own code.

The 360 client talks plain TCP (no ProtoSSL: on the console the Xbox security gateway encrypted the link) to the
address the port gives XOnlineGetServiceInfo (bf/src/online_services.cpp), port 18241. Packets:

    4 bytes   type ("fsys", "acct", ...)
    4 bytes   big endian: transmission type in the top byte (0xC0 request, 0x80 response, 0xF0 and 0xB0 multi
              part, 0x00 ping), packet id in the low 24 bits
    4 bytes   big endian total length, header included
    payload   "key=value" lines joined by "\\n", values with spaces in double quotes, a zero byte at the end

Theater uses the same framing; the 4 letters are the command (CONN, USER, ...), requests carry 0x40000000 and
replies 0, the request's TID comes back in the reply.

Every packet in and out is logged. Unknown transactions are logged and answered with their TXN (FESL) or TID
(Theater) only, so the next step the client asks for shows up in the log.

    python -I server/plasma.py [--theater-host 127.0.0.1] [--log runs/plasma.log] [--seconds N]
"""
import argparse
import asyncio
import base64
import datetime
import itertools
import json
import os
import random
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder out
import gmhost  # noqa: E402

HEADER = 12
MULTI_LIMIT = 8064   # packets larger than this go as multi part (the client says fragmentSize=8096)
MULTI_CHUNK = 6000   # raw payload bytes per part
MULTI = (0xB0000000, 0xF0000000)  # multi part response, request
RESERVATIONS = {}  # player IP -> (GID, time): the server picked in our menu, for the next Quick Match
NAMES = {}  # a name, lower case -> (player id, held until: None while its player is online), see free_name
LEADERBOARD = {}  # a name, lower case -> {'name', 'score', 'seconds', 'captures', 'at'}: the players' own totals
MATCH_SCORES = {}  # a game's GID -> {name, lower case -> {'name', 'score', 'at'}}: the scoreboard, from the players' games
LEADERBOARD_FILE = None  # where it is kept (--leaderboard), loaded at the start


def leaderboard_put(m):
    """A player's totals from their game (our Stats & Awards): kept by name, the newest wins, at most 5,000 names."""
    name = ''.join(c for c in str(m.get('name', '')) if c.isalnum() or c in ' _')[:15].strip()
    if not name:
        return False
    try:
        entry = {'name': name, 'score': int(float(m.get('score', 0))), 'seconds': int(float(m.get('seconds', 0))),
                 'captures': int(float(m.get('captures', 0))), 'at': int(time.time())}
    except (TypeError, ValueError):
        return False
    if len(LEADERBOARD) >= 5000 and name.lower() not in LEADERBOARD:
        return False
    LEADERBOARD[name.lower()] = entry
    if LEADERBOARD_FILE:
        try:
            with open(LEADERBOARD_FILE + '.tmp', 'w', encoding='utf-8') as f:
                json.dump(list(LEADERBOARD.values()), f)
            os.replace(LEADERBOARD_FILE + '.tmp', LEADERBOARD_FILE)
        except OSError:
            pass
    return True


def leaderboard_top(count=50):
    rows = sorted(LEADERBOARD.values(), key=lambda e: (-e['score'], e['name'].lower()))
    return rows[:count], len(rows)


def forget_old():
    """What the tables keep for players gone a while: a server that runs for weeks keeps no list of every login, every
    browser pick and every name it has seen. Run at each login."""
    now = time.time()
    for name in [n for n, held in NAMES.items() if held[1] is not None and held[1] <= now]:
        del NAMES[name]
    for address in [a for a, (_, at) in RESERVATIONS.items() if now - at > 300]:
        del RESERVATIONS[address]
    # A login's key: kept 10 minutes after its FESL connection closed (Theater reads it when the game links).
    for lkey in [k for k, at in Client.closed.items() if now - at > 600]:
        for table in (Client.sessions, Client.user_ids, Client.xuids, Client.closed):
            table.pop(lkey, None)


def name_holder(name):
    """The player id holding a name now, or None."""
    held = NAMES.get(name.lower())
    if held and (held[1] is None or held[1] > time.time()):
        return held[0]
    NAMES.pop(name.lower(), None)
    return None


def free_name(name, pid):
    """The player's name, or it with the smallest number nobody else holds: TekRantGaming, TekRantGaming2, TekRantGaming3 (TekRantGaming, 7 Nov 2026:
    two players with the same name in a server). Names are 15 characters at most, as gamertags; a name differing only
    in case counts as the same one."""
    name = name[:15]
    holder = name_holder(name)
    if holder is None or holder == pid:
        return name
    for n in range(2, 1000):
        suffix = str(n)
        candidate = name[:15 - len(suffix)] + suffix
        holder = name_holder(candidate)
        if holder is None or holder == pid:
            return candidate
    return name[:9] + str(random.randint(100000, 999999))


GAMES = {}  # GID -> game fields: --game (a dedicated server started with --server), later Theater CGAM
# Game wide stats (owner type 11). global_kills unlocks Coral Sea at 1,000,000 (Arcadia's notes). pm_minplayers
# is read with it: 0.0 hangs the game's main thread right after the reply (run qm3), so it is left empty.
STAT_DEFAULTS = {'global_kills': os.environ.get('BF_GLOBAL_KILLS', '0.0'), 'pm_minplayers': os.environ.get('BF_PM_MINPLAYERS', '')}
REQUEST, RESPONSE = 0xC0000000, 0x80000000


def parse_payload(data):
    out = {}
    for line in data.rstrip(b'\0').decode('latin-1').split('\n'):
        if '=' in line:
            k, v = line.split('=', 1)
            if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
                v = v[1:-1]
            out[k] = v
    return out


def build(kind, transmission, packet_id, fields):
    lines = ['%s=%s' % (k, '"%s"' % v if ' ' in str(v) else v) for k, v in fields.items()]
    body = ('\n'.join(lines) + '\0').encode('latin-1')
    return kind.encode() + struct.pack('>II', (transmission & 0xFF000000) | (packet_id & 0xFFFFFF), HEADER + len(body)) + body


class Client:
    """One FESL connection."""
    ids = itertools.count(11)  # user ids; distinct from PIDs and LIDs, so logs show which is which
    sessions = {}  # lkey -> persona name, for Theater's USER
    user_ids = {}  # lkey -> user id
    xuids = {}  # lkey -> the console's XUID (decimal, from NuXBL360Login)
    closed = {}  # lkey -> when its FESL connection closed (forget_old drops its entries 10 minutes later)
    online = set()  # the personas of the FESL connections open now (a name is free again when its player leaves)
    name_grace = 60  # seconds a player's name stays theirs after their connection closes (a reconnect keeps it)
    pnow_ids = itertools.count(1)

    def __init__(self, reader, writer, args, log):
        self.reader, self.writer, self.args, self.log = reader, writer, args, log
        self.peer = '%s:%d' % writer.get_extra_info('peername')[:2]
        self.user_id = next(Client.ids)
        self.persona = None
        self.holds_name = False
        self.lkey = None
        self.multi_id, self.multi_data, self.multi_b64 = None, b'', 0

    def note(self, text):
        line = '%s %s %s' % (time.strftime('%H:%M:%S'), self.peer, text)
        print(line, flush=True)
        self.log.write(line + '\n')
        self.log.flush()

    async def send(self, kind, transmission, packet_id, fields):
        self.note('<- %s %08X id %d %s' % (kind, transmission, packet_id, fields))
        packet = build(kind, transmission, packet_id, fields)
        if len(packet) <= MULTI_LIMIT:
            self.writer.write(packet)
        else:
            # Multi part: each part carries a base64 chunk of the payload ("=" written as %3d), the total base64
            # size and the decoded payload size.
            payload = packet[HEADER:]
            chunks = [base64.b64encode(payload[i:i + MULTI_CHUNK]).decode().replace('=', '%3d')
                      for i in range(0, len(payload), MULTI_CHUNK)]
            total = sum(len(c) for c in chunks)
            for c in chunks:
                self.writer.write(build(kind, MULTI[0], packet_id, {'data': c, 'size': total,
                                                                    'decodedSize': len(payload)}))
            self.note('   (sent as %d parts)' % len(chunks))
        await self.writer.drain()

    # Keepalive: the client sends none of its own and drops a connection that stays silent for 4 minutes (TekRantGaming, kicked
    # 4:00 after joining every time: "BF GM conn err -206", "TheaterConn broken", then a goodbye; run kick2). EA's
    # servers pinged it: FESL with an fsys Ping request, Theater with a PING.
    KEEPALIVE_SECONDS = 30

    async def keepalive(self):
        await self.send('fsys', REQUEST, 0, {'TXN': 'Ping'})

    async def keep_alive(self):
        try:
            while True:
                await asyncio.sleep(self.KEEPALIVE_SECONDS)
                await self.keepalive()
                await self.writer.drain()
        except (ConnectionError, asyncio.CancelledError):
            pass

    async def run(self):
        self.note('connected')
        pinger = asyncio.get_running_loop().create_task(self.keep_alive())
        try:
            while True:
                head = await self.reader.readexactly(HEADER)
                kind = head[:4].decode('latin-1')
                word, length = struct.unpack('>II', head[4:])
                body = await self.reader.readexactly(length - HEADER) if length > HEADER else b''
                transmission, packet_id = word & 0xFF000000, word & 0xFFFFFF
                fields = parse_payload(body)
                if transmission in MULTI:
                    part = fields.get('data', '').replace('%3d', '=')
                    if self.multi_id != packet_id:
                        self.multi_id, self.multi_data, self.multi_b64 = packet_id, b'', 0
                    self.multi_data += base64.b64decode(part)
                    self.multi_b64 += len(part)
                    if self.multi_b64 < int(fields.get('size', '0')):
                        continue
                    self.multi_id = None
                    fields = parse_payload(self.multi_data)
                self.note('-> %s %08X id %d %s' % (kind, transmission, packet_id, fields))
                await self.handle(kind, transmission, packet_id, fields)
        except (asyncio.IncompleteReadError, ConnectionError) as e:
            self.note('closed (%s)' % type(e).__name__)
        finally:
            pinger.cancel()
            self.writer.close()
            if self.lkey:
                Client.closed[self.lkey] = time.time()
            if self.holds_name:
                Client.online.discard(self.persona)
                held = NAMES.get(self.persona.lower())
                if held and held[1] is None:
                    NAMES[self.persona.lower()] = (held[0], time.time() + Client.name_grace)

    async def handle(self, kind, transmission, packet_id, f):
        txn = f.get('TXN', '')
        key = '%s/%s' % (kind, txn)
        if key == 'fsys/Hello':
            now = datetime.datetime.now(datetime.timezone.utc).strftime('%b-%d-%Y %H:%M:%S UTC')
            await self.send('fsys', RESPONSE, packet_id, {
                'domainPartition.domain': f.get('sku', 'xenon'),
                'messengerIp': self.args.theater_host,
                'messengerPort': 0,
                'domainPartition.subDomain': f.get('clientString', 'beach').split('-')[0].upper(),
                'TXN': 'Hello',
                'activityTimeoutSecs': 0,
                'curTime': now,
                'theaterIp': self.args.theater_host,
                'theaterPort': self.args.theater_port,
            })
            await self.send('fsys', REQUEST, 0, {'TXN': 'MemCheck', 'memcheck.[]': 0, 'type': 0,
                                                 'salt': random.randint(100000000, 999999999)})
        elif key in ('fsys/MemCheck', 'fsys/Ping'):
            pass
        elif key == 'fsys/Goodbye':
            await self.send(kind, RESPONSE, packet_id, {'TXN': txn})
            self.writer.close()
        elif key == 'acct/NuXBL360Login':
            # The menu opens its "Connecting" dialog through a queued command but hears "connected" directly; an
            # instant login reply closes the dialog before it opens and it then stays up. Real EA servers took
            # seconds, so wait a little (docs/01-online-flow.md).
            await asyncio.sleep(self.args.login_delay)
            self.persona = f.get('gamertag') or f.get('name') or 'Player%d' % self.user_id
            # Personas must differ: a host finds a joiner's user by name (sub_82507450), and two consoles with the
            # same gamertag (both "User" in our runs) made the host link the joiner under its own id (run duo10).
            # Only the players online now count: a player who left (or whose game closed) gets their name back.
            # A renamed player can't join a dedicated server (their game finds itself in the roster by its own
            # gamertag: an access violation at stage 5, TekRantGaming 6 Nov, run hetzner-rename2), so each game now has its
            # own name (bf_player_name, bf\src\live_layers.cpp) and this is the last resort.
            # The player id: the XUID's low word (the game's player_id, bf\src\game_menu.cpp). Our games ask the
            # registry for a free name before they log in ({"op": "name"}), so this rename is the last resort.
            try:
                pid = int(f.get('xuid', '0')) & 0xFFFFFFFF
            except ValueError:
                pid = 0
            if not pid:
                pid = 0x80000000 | self.user_id
            if name_holder(self.persona) not in (None, pid):
                renamed = free_name(self.persona, pid)
                if renamed == self.persona:
                    renamed = '%s%d' % (self.persona[:12], self.user_id)
                self.note('the name %s is in use: this player is renamed %s' % (self.persona, renamed))
                self.persona = renamed
            NAMES[self.persona.lower()] = (pid, None)
            Client.online.add(self.persona)
            self.holds_name = True
            lkey = 'BF1943LKEY%08d' % self.user_id
            forget_old()
            self.lkey = lkey
            Client.sessions[lkey] = self.persona
            Client.user_ids[lkey] = self.user_id
            Client.xuids[lkey] = f.get('xuid', '0')
            await self.send(kind, RESPONSE, packet_id, {
                'TXN': txn, 'lkey': lkey, 'userId': self.user_id, 'personaName': self.persona,
            })
        elif key == 'fsys/GetPingSites':
            await self.send(kind, RESPONSE, packet_id, {'TXN': txn, 'pingSite.[]': 0, 'minPingSitesToPing': 0})
        elif key == 'asso/GetAssociations':
            await self.send(kind, RESPONSE, packet_id, {
                'TXN': txn, 'domainPartition.domain': f.get('domainPartition.domain', 'xenon'),
                'domainPartition.subDomain': f.get('domainPartition.subDomain', 'BEACH'),
                'owner.id': self.user_id, 'owner.type': 1, 'type': f.get('type', ''),
                'owner.name': self.persona or 'Player', 'maxListSize': 100, 'members.[]': 0,
            })
        elif key == 'acct/GetTelemetryToken':
            # "host,port,locale," then an opaque part; nothing listens there, telemetry stays off.
            token = base64.b64encode(b'127.0.0.1,9946,enUS,bf1943-no-telemetry').decode()
            await self.send(kind, RESPONSE, packet_id, {'TXN': txn, 'filters': '', 'enabled': '', 'disabled': '',
                                                        'telemetryToken': token})
        elif key == 'rank/GetStats':
            count = int(f.get('keys.[]', '0') or 0)
            out = {'TXN': txn, 'stats.[]': count}
            for i in range(count):
                stat = f.get('keys.%d' % i, '')
                out['stats.%d.key' % i] = stat
                out['stats.%d.value' % i] = STAT_DEFAULTS.get(stat, '0.0')
            await self.send(kind, RESPONSE, packet_id, out)
        elif key == 'pnow/Start':
            # Play now (Quick Match): the session id first, then the result. resultType is one of NOMATCH,
            # NOSERVER, JOIN, LIST, CREATE, CREATE_PROXY (the client's strings at 0x8202CE4C). The client asks with
            # sessionType findServer, and after NOSERVER again with resetServer: then CREATE makes it host a
            # public game itself (Theater CGAM), which the next player's findServer joins.
            session_id = next(Client.pnow_ids)
            partition = f.get('partition.partition', '/xenon/BEACH')
            session = f.get('players.0.props.{sessionType}', 'findServer')
            await self.send(kind, RESPONSE, packet_id, {'TXN': 'Start', 'id.id': session_id, 'id.partition': partition})
            status = {'TXN': 'Status', 'id.id': session_id, 'id.partition': partition, 'sessionState': 'COMPLETE'}
            # An unlisted server (a private match, Direct connect only) never takes a Quick Match the player did not
            # pick in our menu (its reservation below still joins it).
            games = [g for g in GAMES.values()
                     if (g.get('TYPE') != 'P' or self.args.list_private) and g.get('listed', True)]
            # A server picked in our menu's server browser (Registry "reserve", by the player's address).
            reserved = RESERVATIONS.get(self.peer.split(':')[0])
            if reserved and time.time() - reserved[1] < 300 and reserved[0] in GAMES:
                games = [GAMES[reserved[0]]]
            if games:
                status.update({'props.{}': 3, 'props.{resultType}': 'JOIN', 'props.{avgFit}': '1.0',
                               'props.{games}.[]': len(games)})
                for i, g in enumerate(games):
                    status['props.{games}.%d.gid' % i] = g['GID']
                    status['props.{games}.%d.lid' % i] = g['LID']
            elif session == 'resetServer' and not self.args.no_create:
                # The game's attributes (sub_824F3F78 reads props.{attrib}.N.name/value: "version", then U-*
                # attributes) from what the player asked for: filter-X values and pref-X values with their
                # prefToGame-X names.
                pre = 'players.0.props.{'
                attrib = [('version', f.get(pre + 'filter-version}', ''))]
                for k, v in f.items():
                    if k.startswith(pre + 'filterToGame-') and v.startswith('U-'):
                        attrib.append((v, f.get(pre + 'filter-' + k[len(pre + 'filterToGame-'):], '')))
                    elif k.startswith(pre + 'prefToGame-'):
                        attrib.append((v, f.get(pre + 'pref-' + k[len(pre + 'prefToGame-'):], '')))
                status.update({'props.{}': 4, 'props.{resultType}': 'CREATE',
                               'props.{maxGameSize}': f.get(pre + 'poolMaxPlayers}', '24'),
                               'props.{resetTag}': 'bf1943-%d' % session_id, 'props.{attrib}.[]': len(attrib)})
                for i, (name, value) in enumerate(attrib):
                    status['props.{attrib}.%d.name' % i] = name
                    status['props.{attrib}.%d.value' % i] = value
            else:
                status.update({'props.{}': 1, 'props.{resultType}': 'NOSERVER'})
            await self.send(kind, RESPONSE, packet_id, status)
        else:
            self.note('unknown transaction %s, answering with its TXN only' % key)
            await self.send(kind, RESPONSE, packet_id, {'TXN': txn})


class TheaterClient(Client):
    """One Theater connection.

    Games: --game servers (dedicated, not connected to Theater) and games a client hosts (Create Private Match:
    CGAM with RESERVE-HOST=1, then the host enters its own game with EGAM). Entering a hosted game: Theater asks
    the host (EGRQ), the host answers (EGRS, ALLOWED), and the joiner gets the host's address and Xbox secure
    address (EGEG: XNADDR, XNKID session id, XNKEY key). The field names are the ones the client reads (its tables
    at 0x8202543C to 0x8202589C).
    """
    gids = itertools.count(1)
    pids = itertools.count(7)

    async def keepalive(self):
        await self.send('PING', 0, 0, {'TID': '0'})

    def __init__(self, *a):
        super().__init__(*a)
        self.name, self.uid, self.xuid, self.xnaddr = 'Player', 0, '0', ''
        self.joins = {}  # PID -> (joiner, game) waiting for this host's EGRS

    async def handle(self, kind, transmission, packet_id, f):
        tid = f.get('TID', '')
        if kind == 'CONN':
            await self.send('CONN', 0, 0, {'TIME': int(time.time()), 'TID': tid, 'activityTimeoutSecs': 0,
                                           'PROT': f.get('PROT', '2')})
        elif kind == 'USER':
            lkey = f.get('LKEY', '')
            self.name, self.uid = Client.sessions.get(lkey, 'Player'), Client.user_ids.get(lkey, 0)
            self.xuid = Client.xuids.get(lkey, '0')
            await self.send('USER', 0, 0, {'NAME': self.name, 'TID': tid})
        elif kind == 'PING':
            pass
        elif kind == 'CGAM':
            gid = str(next(TheaterClient.gids) + 100)
            g = {'GID': gid, 'LID': '5', 'I': self.peer.split(':')[0], 'P': int(f.get('PORT', '1000')),
                 'INT-IP': f.get('INT-IP', ''), 'INT-PORT': f.get('INT-PORT', f.get('PORT', '1000')),
                 'HU': self.uid, 'HN': self.name, 'N': f.get('NAME', self.name), 'TYPE': f.get('TYPE', 'P'),
                 'JOIN': f.get('JOIN', 'O'), 'MAX-PLAYERS': f.get('MAX-PLAYERS', '16'), 'AP': 0,
                 'UGID': f.get('UGID') or 'NOGUID', 'SECRET': f.get('SECRET') or 'NOSECRET',
                 'B-version': f.get('B-version', ''), 'host': self,
                 'XNKID': base64.b64encode(os.urandom(8)).decode(), 'XNKEY': base64.b64encode(os.urandom(16)).decode()}
            for k, v in f.items():
                if k.startswith('B-') and k not in g:
                    g[k] = v
            GAMES[gid] = g
            await self.send('CGAM', 0, 0, {'TID': tid, 'LID': g['LID'], 'GID': gid, 'MAX-PLAYERS': g['MAX-PLAYERS'],
                                           'EKEY': self.args.ekey, 'UGID': g['UGID'], 'JOIN': g['JOIN'],
                                           'SECRET': g['SECRET'], 'J': g['JOIN']})
        elif kind == 'GDAT':
            g = GAMES.get(f.get('GID', ''))
            await self.send('GDAT', 0, 0, dict(public(g), TID=tid) if g else {'TID': tid})
        elif kind == 'EGAM':
            g = GAMES.get(f.get('GID', ''))
            await self.send('EGAM', 0, 0, {'TID': tid, 'LID': f.get('LID', ''), 'GID': f.get('GID', '')})
            if not g:
                return
            self.xnaddr = f.get('R-XNADDR', '')
            pid = next(TheaterClient.pids)
            # One ticket per join: the joiner shows the one from EGEG to the host, which compares it with the one
            # from EGRQ and drops the player when they differ (sub_824BDDF0).
            ticket = '0x%x' % random.getrandbits(31)
            host = g.get('host')
            if host is self:
                g['XNADDR'] = self.xnaddr
            if host:
                # Ask the host, also when the host enters its own game (it adds players to its list from EGRQ;
                # without that its PENT reply finds no player). EGEG goes to the joiner when the host allows it.
                host.joins[str(pid)] = (self, g, ticket)
                await host.send('EGRQ', 0, 0, {
                    # The host reads NAME, UID, PID, TICKET, PORT, R-XNADDR, XUID, R-U-*, R-XUID, R-UID,
                    # R-USER, GROUP.[], R-GROUPID, PTYPE (sub_824D8580).
                    'XUID': self.xuid, 'R-XUID': self.xuid, 'R-USER': self.name,
                    'R-NAME': self.name, 'R-UID': self.uid, 'R-U-accid': self.uid, 'R-XNADDR': self.xnaddr,
                    'R-INT-IP': f.get('R-INT-IP', self.peer.split(':')[0]), 'R-INT-PORT': f.get('PORT', '1000'),
                    'NAME': self.name, 'UID': self.uid, 'PID': pid, 'TICKET': ticket,
                    'IP': self.peer.split(':')[0], 'PORT': f.get('PORT', '1000'), 'PTYPE': f.get('PTYPE', 'P'),
                    'XNADDR': self.xnaddr, 'LID': g['LID'], 'GID': g['GID']})
            else:
                # A --game or --dedicated server: not connected to Theater, the address goes out at once. Our
                # GameManager host for a --dedicated server learns the player first; the client's hello names
                # the ticket.
                if g.get('gmhost'):
                    g['gmhost'].expect(ticket, pid, self.name, self.xuid, self.xnaddr)
                await self.send_egeg(g, pid, ticket)
        elif kind in ('PENT', 'PLVT'):
            # Player entered (or left) the host's game; the host looks the player up by the PID in the reply
            # (without it, sub_824B6CD0 writes through a null player).
            await self.send(kind, 0, 0, {'TID': tid, 'PID': f.get('PID', ''), 'LID': f.get('LID', ''),
                                         'GID': f.get('GID', '')})
        elif kind == 'EGRS':
            # The host's EGRS is a request: without the reply (sub_824BDD70 handles it) the host never starts its
            # link to the joiner, and the joiner's tunnel packets find no tunnel there.
            await self.send('EGRS', 0, 0, {'TID': tid, 'LID': f.get('LID', ''), 'GID': f.get('GID', ''),
                                           'PID': f.get('PID', '')})
            joiner, g, ticket = self.joins.pop(f.get('PID', ''), (None, None, None))
            if joiner and f.get('ALLOWED', '1') == '1':
                await joiner.send_egeg(g, f.get('PID', '0'), ticket)
            elif joiner:
                self.note('host refused PID %s: %s' % (f.get('PID'), f.get('REASON', '')))
        elif transmission == 0:
            pass  # not a request (UGAM, the host's game updates): no reply, or the client logs an unknown txn
        else:
            self.note('unknown Theater command %s, answering with its TID only' % kind)
            await self.send(kind, 0, 0, {'TID': tid})

    async def send_egeg(self, g, pid, ticket):
        out = {'PL': 'xenon', 'TICKET': ticket, 'PID': pid, 'HUID': g['HU'],
               'I': g['I'], 'P': g['P'], 'INT-IP': g.get('INT-IP') or g['I'], 'INT-PORT': g.get('INT-PORT', g['P']),
               'EKEY': self.args.ekey, 'UGID': g['UGID'], 'LID': g['LID'], 'GID': g['GID']}
        for k in ('XNADDR', 'XNKID', 'XNKEY'):
            if g.get(k):
                out[k] = g[k]
        await self.send('EGEG', 0, 0, out)


class RemoteHost:
    """A registered server's GameManager host, reached over its registry connection."""

    def __init__(self, writer):
        self.writer = writer

    def expect(self, ticket, pid, name, xuid, xnaddr_b64):
        self.writer.write((json.dumps({'op': 'join', 'ticket': ticket, 'pid': int(pid), 'name': name,
                                       'xuid': str(xuid), 'xnaddr': xnaddr_b64}) + '\n').encode())


class Registry:
    """Dedicated servers register here (server/gmhost.py run by the server package) and stay listed while their
    connection lasts. One JSON object per line:
      server:  {"op": "register", "name", "port", "map", "max_players", "mode", "description", "maps", "listed",
                "official"?, "address"?}
               mode: "classic" or "modern", the movement and aiming the server plays (default classic)
               the address: the one the registration came from (a loopback one: this backend's --theater-host);
               "address" [host, port] only from older packages and tests (SERVERS-PLAN.md section 6)
               listed false: not in the list, joined by Direct connect ("find") only
               official: {"stamp", "nonce", "mac"} from the official key (gmhost.official_code); checked against
               --official-key-file, else refused
      backend: {"op": "registered", "gid", "uid", "pid", "official", "listed", "address"}
      backend: {"op": "reachable", "ok", "address", "local"} after a UDP probe of the server's port; for a server on
               this backend's own machine ("local"), ok is null when it answers: a probe from here never passes
               the machine's incoming firewall, so whether players outside reach it cannot be told from here
      backend: {"op": "refused", "why"}                     an official code that does not check
      server:  {"op": "status", "players", "map", "next_map"}   every few seconds
      backend: {"op": "join", "ticket", "pid", "name", "xuid", "xnaddr"}   before the joiner gets EGEG
      anyone:  {"op": "list"} -> {"op": "servers", "servers": [...]}   for a server browser (listed servers only)
      anyone:  {"op": "find", "address", "port"} -> {"op": "found", "server"|null}   Direct connect
      player:  {"op": "reserve", "gid"} -> {"op": "reserved"}  the player's next Quick Match joins that game
      player:  {"op": "name", "name", "id"} -> {"op": "name", "name"}  before logging in: the player's name, or it
               with a number when another player has it (free_name); held for that player id for 10 minutes, and
               while they are online
    """
    ids = itertools.count(1)

    def __init__(self, log, args=None):
        self.log = log
        self.public = getattr(args, 'theater_host', '127.0.0.1') if args else '127.0.0.1'
        self.key = None
        if args and getattr(args, 'official_key_file', ''):
            self.key = gmhost.read_key(args.official_key_file)
        self.used = {}  # official codes taken (nonce -> time)

    def official(self, code, name):
        """An official code that checks: made with the official key, within two minutes, never seen before."""
        import hmac
        if not self.key or not isinstance(code, dict):
            return False
        try:
            stamp, nonce = int(code['stamp']), str(code['nonce'])[:32]
        except (KeyError, ValueError, TypeError):
            return False
        now = time.time()
        self.used = {n: t for n, t in self.used.items() if now - t < 300}
        if abs(now - stamp) > 120 or nonce in self.used:
            return False
        want = gmhost.official_code(self.key, name, stamp, nonce)['mac']
        if not hmac.compare_digest(want, str(code.get('mac', ''))):
            return False
        self.used[nonce] = now
        return True

    async def probe(self, writer, host, port, local=False):
        """Whether players can reach a registered server: a UDP probe its host answers (gmhost PROBE, PONG). For a
        server on this machine (local) an answer only says it runs: the probe stays inside the machine and skips its
        incoming firewall (Hetzner, 8 Nov 2026: all five official servers read reachable while the firewall blocked
        four), so the answer is then "not known" (ok null), and no answer still means it cannot be reached."""
        loop = asyncio.get_running_loop()
        got = loop.create_future()

        class Probe(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                if data.startswith(gmhost.PONG) and not got.done():
                    got.set_result(True)

        ok = False
        try:
            # A socket that ignores Windows' port unreachable reports (gmhost.udp_socket): a probe that came before the
            # host had bound its port left asyncio's Proactor loop reading nothing more, so the answers to the next
            # two were never read (rejoin tests of 9 Nov 2026).
            sock = gmhost.udp_socket(('::' if ':' in host else '0.0.0.0', 0))
            transport, _ = await loop.create_datagram_endpoint(Probe, sock=sock)
            try:
                for _ in range(3):
                    transport.sendto(gmhost.PROBE + os.urandom(8), (host, port))
                    try:
                        ok = await asyncio.wait_for(asyncio.shield(got), 1.0)
                        break
                    except asyncio.TimeoutError:
                        pass
            finally:
                transport.close()
        except OSError:
            ok = False
        verdict = None if (ok and local) else bool(ok)
        try:
            writer.write((json.dumps({'op': 'reachable', 'ok': verdict, 'local': bool(local),
                                      'address': '%s:%d' % (host, port)}) + '\n').encode())
            await writer.drain()
        except ConnectionError:
            pass
        self.note('%s:%d %s' % (host, port, 'NOT reachable by UDP (closed port?)' if not ok else
                                'answers on this machine; from outside not checked (a probe from here skips the '
                                'firewall): try the game\'s server browser from another network' if local else
                                'reachable'))

    def note(self, text):
        line = '%s REGISTRY %s' % (time.strftime('%H:%M:%S'), text)
        print(line, flush=True)
        self.log.write(line + '\n')
        self.log.flush()

    async def client(self, reader, writer):
        gid = None
        peer = '%s:%d' % writer.get_extra_info('peername')[:2]
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                m = json.loads(line)
                op = m.get('op')
                if op == 'register' and not gid:
                    name = ''.join(c for c in str(m.get('name', 'Server')) if c.isprintable())[:48] or 'Server'
                    official = 'official' in m and self.official(m['official'], m.get('name', 'Server'))
                    if 'official' in m and not official:
                        writer.write((json.dumps({'op': 'refused', 'why': 'the official code does not check'}) +
                                      '\n').encode())
                        self.note('%s refused: an official code that does not check' % peer)
                        await writer.drain()
                        break
                    n = next(Registry.ids)
                    gid, uid, pid = str(200 + n), 900 + n, next(TheaterClient.pids)
                    local = False
                    if 'address' in m:
                        host, port = m['address']
                    else:
                        host, port = writer.get_extra_info('peername')[0], int(m.get('port', 18300))
                        if host.startswith('127.') or host == '::1':
                            host, local = self.public, True  # a server on this backend's own machine
                    GAMES[gid] = {'GID': gid, 'LID': '1', 'I': host, 'P': int(port), 'HU': uid,
                                  'HN': name, 'N': name, 'TYPE': 'G',
                                  'JOIN': 'O', 'MAX-PLAYERS': int(m.get('max_players', 24)), 'AP': 0,
                                  'UGID': 'NOGUID', 'B-version': 'RETAIL429432', 'B-U-type': 'RANKED',
                                  'gmhost': RemoteHost(writer), 'map': m.get('map', ''),
                                  'mode': 'modern' if m.get('mode') == 'modern' else 'classic',
                                  'description': ''.join(c for c in str(m.get('description', ''))
                                                         if c.isprintable())[:200],
                                  'maps': [str(x)[:32] for x in m.get('maps', []) if isinstance(x, str)][:8],
                                  'next_map': '', 'official': official, 'listed': bool(m.get('listed', True)),
                                  'XNADDR': base64.b64encode(gmhost.xnaddr(host)).decode(),
                                  'XNKID': base64.b64encode(os.urandom(8)).decode(),
                                  'XNKEY': base64.b64encode(os.urandom(16)).decode()}
                    writer.write((json.dumps({'op': 'registered', 'gid': gid, 'uid': uid, 'pid': pid,
                                              'official': official, 'listed': GAMES[gid]['listed'],
                                              'address': '%s:%d' % (host, int(port))}) + '\n').encode())
                    self.note('%s registered "%s" at %s:%s as game %s%s%s' % (
                        peer, name, host, port, gid, ' (official)' if official else '',
                        '' if GAMES[gid]['listed'] else ' (not listed)'))
                    asyncio.get_running_loop().create_task(self.probe(writer, host, int(port), local))
                elif op == 'status' and gid in GAMES:
                    GAMES[gid]['AP'] = int(m.get('players', 0))
                    GAMES[gid]['map'] = m.get('map', GAMES[gid]['map'])
                    GAMES[gid]['next_map'] = str(m.get('next_map', ''))[:32]
                    names = m.get('names', [])
                    GAMES[gid]['names'] = [str(n)[:15] for n in names][:64] if isinstance(names, list) else []
                elif op == 'players':
                    # Join Match With Friends (our main menu): the players signed in now and the listed server each
                    # plays on (no address), the servers' own names left out. A player in a private match (a server
                    # not listed) shows as there, with no way in from this list: its host gives friends the address
                    # for Direct connect.
                    hosts = {g.get('HN') for g in GAMES.values()}
                    where = {}
                    for g in GAMES.values():
                        if g.get('TYPE') == 'G':
                            for n in g.get('names', []):
                                where[n.lower()] = g
                    rows = []
                    for persona in sorted(Client.online):
                        if persona in hosts:
                            continue
                        g = where.get(persona.lower())
                        if g and not g.get('listed', True):
                            rows.append({'name': persona, 'gid': '', 'server': 'Private match', 'map': ''})
                            continue
                        rows.append({'name': persona, 'gid': g['GID'] if g else '', 'server': g.get('N', '') if g else '',
                                     'map': g.get('map', '') if g else ''})
                    writer.write((json.dumps({'op': 'players', 'rows': rows[:200]}) + '\n').encode())
                elif op == 'match_score' and str(m.get('gid', '')) in GAMES:
                    # The pause's Scoreboard: each player's game sends its score in this match.
                    name = ''.join(c for c in str(m.get('name', '')) if c.isalnum() or c in ' _')[:15].strip()
                    try:
                        score = int(float(m.get('score', 0)))
                    except (TypeError, ValueError):
                        score = None
                    if name and score is not None:
                        table = MATCH_SCORES.setdefault(str(m['gid']), {})
                        if len(table) < 64 or name.lower() in table:
                            table[name.lower()] = {'name': name, 'score': score, 'at': int(time.time())}
                    writer.write((json.dumps({'op': 'match_score', 'ok': bool(name)}) + '\n').encode())
                elif op == 'scoreboard':
                    g = GAMES.get(str(m.get('gid', '')))
                    joined = {n.lower() for n in g.get('names', [])} if g else set()
                    table = MATCH_SCORES.get(str(m.get('gid', '')), {})
                    rows = [e for k, e in table.items() if not joined or k in joined]
                    for n in (g.get('names', []) if g else []):  # players with no score sent yet
                        if n.lower() not in table:
                            rows.append({'name': n, 'score': 0, 'at': 0})
                    rows.sort(key=lambda e: (-e['score'], e['name'].lower()))
                    writer.write((json.dumps({'op': 'scoreboard', 'rows': rows[:64]}) + '\n').encode())
                elif op == 'stats_put':
                    ok = leaderboard_put(m)
                    writer.write((json.dumps({'op': 'stats_put', 'ok': ok}) + '\n').encode())
                elif op == 'leaderboard':
                    rows, total = leaderboard_top()
                    writer.write((json.dumps({'op': 'leaderboard', 'rows': rows, 'total': total}) + '\n').encode())
                elif op == 'reserve' and m.get('gid') in GAMES:
                    RESERVATIONS[peer.split(':')[0]] = (m['gid'], time.time())
                    writer.write((json.dumps({'op': 'reserved', 'gid': m['gid']}) + '\n').encode())
                    self.note('%s picked game %s' % (peer, m['gid']))
                elif op == 'name':
                    asked = ''.join(c for c in str(m.get('name', '')) if c.isalnum() or c in ' _')[:15].strip()
                    try:
                        pid = int(str(m.get('id', '0')), 16) & 0xFFFFFFFF
                    except ValueError:
                        pid = 0
                    if not asked or not pid:
                        writer.write((json.dumps({'op': 'name', 'name': asked}) + '\n').encode())
                    else:
                        name = free_name(asked, pid)
                        if name_holder(name) != pid or NAMES[name.lower()][1] is not None:
                            NAMES[name.lower()] = (pid, time.time() + 600)
                        writer.write((json.dumps({'op': 'name', 'name': name}) + '\n').encode())
                        self.note('%s asked for the name %s: %s' % (peer, asked, name))
                elif op == 'find':
                    want = (str(m.get('address', '')), int(m.get('port', 18300) or 18300))
                    hit = [g for g in GAMES.values() if g.get('TYPE') == 'G' and (g['I'], g['P']) == want]
                    writer.write((json.dumps({'op': 'found', 'server': listing(hit[0]) if hit else None}) +
                                  '\n').encode())
                elif op == 'list':
                    # The address goes to the game (its ping and its join need it); the browser never shows it.
                    servers = [listing(g) for g in GAMES.values() if g.get('TYPE') == 'G' and g.get('listed', True)]
                    # The players signed in now, for the launcher's Home (PLAYERS ONLINE): every open FESL persona but
                    # the servers' own.
                    hosts = {g.get('HN') for g in GAMES.values()}
                    online = len([p for p in Client.online if p not in hosts])
                    writer.write((json.dumps({'op': 'servers', 'servers': servers, 'players_online': online}) +
                                  '\n').encode())
                await writer.drain()
        except (ConnectionError, ValueError, KeyError) as e:
            self.note('%s %s' % (peer, type(e).__name__))
        finally:
            if gid:
                GAMES.pop(gid, None)
                MATCH_SCORES.pop(gid, None)
                self.note('game %s gone (server disconnected)' % gid)
            writer.close()


def listing(g):
    """A game as the server browser gets it."""
    return {'gid': g['GID'], 'name': g['N'], 'address': [g['I'], g['P']], 'map': g.get('map', ''),
            'players': g['AP'], 'max_players': g['MAX-PLAYERS'], 'mode': g.get('mode', 'classic'),
            'description': g.get('description', ''), 'official': bool(g.get('official')),
            'next_map': g.get('next_map', ''), 'maps': g.get('maps', [])}


def public(g):
    """A game's fields without our own bookkeeping."""
    return {k: v for k, v in g.items() if k not in ('host', 'gmhost', 'XNKID', 'XNKEY', 'XNADDR', 'map', 'mode')}


async def http_client(reader, writer, log):
    """The game's HTTP requests (EA's editorial news, easo.ea.com, reached at the second LIVE service's address)."""
    peer = '%s:%d' % writer.get_extra_info('peername')[:2]
    try:
        head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 10)
        text = head.decode('latin-1').strip()
        line = '%s %s HTTP %s' % (time.strftime('%H:%M:%S'), peer, text.replace('\r\n', ' | '))
        log.write(line + '\n')
        log.flush()
        path = text.split(' ')[1] if ' ' in text else ''
        if path.endswith('/ticker-xenon.xml'):
            # The client checks for a UTF-8 byte order mark and "<?xml" in the first 8 bytes (else it retries in
            # 60 s), then reads <ticker><lang id="..."><item>text</item>...</lang></ticker> for its language.
            status, kind = b'200 OK', b'text/xml'
            body = (b'\xef\xbb\xbf<?xml version="1.0" encoding="UTF-8"?>\n<ticker>\n  <lang id="en-US">\n'
                    b'    <item>Local test server</item>\n  </lang>\n</ticker>\n')
        else:
            status, kind, body = b'404 Not Found', b'text/plain', b'Not found'
        style = os.environ.get('BF_HTTP_STYLE', '11')  # experiments with the reply format
        if style == '10':  # HTTP/1.0, no Connection header
            head_out = b'HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n' % (status, kind, len(body))
        elif style == 'nolen':  # HTTP/1.0, the close ends the body
            head_out = b'HTTP/1.0 %s\r\nContent-Type: %s\r\n\r\n' % (status, kind)
        else:
            head_out = (b'HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n'
                        % (status, kind, len(body)))
        writer.write(head_out + body)
        await writer.drain()
    except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, asyncio.LimitOverrunError):
        pass
    finally:
        writer.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=18241, help='FESL port')
    ap.add_argument('--theater-host', default='127.0.0.1')
    ap.add_argument('--theater-port', type=int, default=18246)
    ap.add_argument('--login-delay', type=float, default=2.0, help='seconds before the login reply')
    # Off by default: with our HTTP reply the client never finishes the ticker request (docs/01-online-flow.md).
    ap.add_argument('--http-port', type=int, default=0, help='HTTP port for the news ticker (0: off)')
    ap.add_argument('--game', action='append', default=[], metavar='HOST:PORT',
                    help='list a dedicated server (bf1943.exe --server --cl=-dedicated) for Quick Match')
    # The client's own local EGEG (sub_824B03F8) sends these two: no tunnel key, no game UID.
    ap.add_argument('--ekey', default='NOENCYRPTIONKEY', help="EGEG's tunnel key (the client's spelling)")
    ap.add_argument('--list-private', action='store_true',
                    help='Quick Match also offers private games a client hosts (join tests)')
    ap.add_argument('--no-create', action='store_true',
                    help='Quick Match never asks a client to host (only --game servers and hosted games)')
    ap.add_argument('--dedicated', action='append', default=[], metavar='GWHOST:GWPORT=SERVERHOST:SERVERPORT',
                    help='list a dedicated server behind our GameManager host (server/gmhost.py): joiners link to '
                         'GWHOST:GWPORT, game packets go on to the server at SERVERHOST:SERVERPORT (UDP 1003)')
    ap.add_argument('--registry-host', default='', help='where dedicated servers register (default: --host)')
    ap.add_argument('--registry-port', type=int, default=18250, help='server registry port (0: off)')
    ap.add_argument('--official-key-file', default='',
                    help='official.key: servers registering with a code from it are official (TekRantGaming only)')
    ap.add_argument('--log', default='runs/plasma.log')
    ap.add_argument('--leaderboard', default='', help="a file keeping the players' totals (our Leaderboards)")
    ap.add_argument('--seconds', type=int, default=0, help='stop after this many seconds (0: run until stopped)')
    args = ap.parse_args()
    global LEADERBOARD_FILE
    LEADERBOARD_FILE = args.leaderboard or None
    if LEADERBOARD_FILE and os.path.exists(LEADERBOARD_FILE):
        try:
            for e in json.load(open(LEADERBOARD_FILE, encoding='utf-8')):
                LEADERBOARD[str(e['name']).lower()] = e
        except (OSError, ValueError, KeyError, TypeError):
            pass
    for n, address in enumerate(args.game, 1):
        host, port = address.rsplit(':', 1)
        GAMES[str(n)] = {'GID': str(n), 'LID': '1', 'I': host, 'P': int(port), 'HU': n, 'HN': 'bf1943-server-%d' % n,
                         'N': 'Local server %d' % n, 'TYPE': 'G', 'JOIN': 'O', 'MAX-PLAYERS': 24, 'AP': 0,
                         'UGID': 'NOGUID', 'B-version': 'RETAIL429432', 'B-U-type': 'RANKED'}
    with open(args.log, 'a') as log:
        for n, spec in enumerate(args.dedicated, len(args.game) + 1):
            gw, srv = spec.split('=')
            gw_host, gw_port = gw.rsplit(':', 1)
            srv_host, srv_port = srv.rsplit(':', 1)
            uid, name = 900 + n, 'Dedicated server %d' % n
            host = await gmhost.start((gw_host, int(gw_port)), (srv_host, int(srv_port)), uid,
                                      next(TheaterClient.pids), name, log)
            GAMES[str(n)] = {'GID': str(n), 'LID': '1', 'I': gw_host, 'P': int(gw_port), 'HU': uid, 'HN': name,
                             'N': name, 'TYPE': 'G', 'JOIN': 'O', 'MAX-PLAYERS': 24, 'AP': 0, 'UGID': 'NOGUID',
                             'B-version': 'RETAIL429432', 'B-U-type': 'RANKED', 'gmhost': host,
                             'XNADDR': base64.b64encode(gmhost.xnaddr(gw_host)).decode(),
                             'XNKID': base64.b64encode(os.urandom(8)).decode(),
                             'XNKEY': base64.b64encode(os.urandom(16)).decode()}
        fesl = await asyncio.start_server(lambda r, w: Client(r, w, args, log).run(), args.host, args.port)
        theater = await asyncio.start_server(lambda r, w: TheaterClient(r, w, args, log).run(), args.host,
                                             args.theater_port)
        log.write('%s FESL on %s:%d, Theater on %s:%d\n' % (time.strftime('%H:%M:%S'), args.host, args.port,
                                                          args.host, args.theater_port))
        log.flush()
        servers = [fesl, theater]
        if args.registry_port:
            registry = Registry(log, args)
            servers.append(await asyncio.start_server(registry.client, args.registry_host or args.host,
                                                      args.registry_port))
            log.write('%s server registry on %s:%d\n' % (time.strftime('%H:%M:%S'), args.registry_host or args.host,
                                                          args.registry_port))
        if args.http_port:
            servers.append(await asyncio.start_server(lambda r, w: http_client(r, w, log), args.host, args.http_port))
            log.write('%s HTTP on %s:%d\n' % (time.strftime('%H:%M:%S'), args.host, args.http_port))
        if args.seconds:
            await asyncio.sleep(args.seconds)
        else:
            await asyncio.gather(*(srv.serve_forever() for srv in servers))


if __name__ == '__main__':
    asyncio.run(main())
