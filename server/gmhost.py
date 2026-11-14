"""GameManager host for a dedicated server: the part of EA's online layer a console host runs, written for our
own server package. Clients join a game the backend lists; their GameManager links to this host over EA's tunnel,
gets the roster and join messages, and then sends the game's own packets over that link. We pass those packets to
a dedicated server (bf1943.exe --server --cl=-dedicated, UDP 1003) and send its replies back the same way.

Layers, outermost first (see docs/02-dedicated-server-join.md):
  tunnel      u16 length, then u16 per sub packet (length << 4 | port index), then the sub packets. Port index 7
              is control (u8 0, u32 sender id, u8 bound), 1 is the GameManager link, 0 is voice (ignored here).
  link        u32 seq, u32 ack, then data. seq 1 INIT (ack field: the connection id), 2 CONN, 0x80 to 0xFF
              unreliable, 0x100 and up reliable. The seq's top nibble counts older payloads appended for
              redundancy, each followed by its size byte (payload for seq - k at index k).
  game link   data, then 10 sync bytes when the kind byte has 0x40, then the kind byte: 6 a stream message, 7 a
              datagram. SendData (sub_824A78A0) puts a flag byte first: 1 for GameManager's own messages.
  GameManager message type byte (0x80 + type), then fields. Integers are big endian with the sign bit flipped
              (value + 0x80, + 0x8000, + 0x80000000); strings are an i32 length and the bytes.
"""
import asyncio
import json
import os
import random
import socket
import struct
import sys
import time

CONTROL, LINK, VOICE = 7, 1, 0
INIT, CONN, UNREL, DATA = 1, 2, 0x80, 0x100
KIND_STREAM, KIND_DATAGRAM, KIND_SYNC = 6, 7, 0x40
RESEND_SECONDS = 0.3


PING, PROBE, PONG = b'BF43PING', b'BF43PRBE', b'BF43PONG'
COMMS = b'BF43COMM'  # our voice and text chat (Comms below), from the game's own comms socket, never a game packet


def dev_net_delay():
    """Tests only: BF_DEV_NET_DELAY=<ms>[,<jitter ms>] holds every game datagram between a client and the dedicated
    server that long each way, plus up to the jitter, in order: a remote server's latency on one PC (TekRantGaming's 0.1.26
    play test on the official server showed vehicle judder the local tests did not). Unset (the official servers),
    nothing waits."""
    try:
        parts = [float(x) / 1000.0 for x in os.environ.get('BF_DEV_NET_DELAY', '').split(',') if x.strip()]
    except ValueError:
        parts = []
    return (parts + [0.0, 0.0])[:2]


DEV_DELAY, DEV_JITTER = dev_net_delay()


class Held:
    """One direction's datagrams held DEV_DELAY plus up to DEV_JITTER, sent in the order they came."""

    def __init__(self):
        self.last = 0.0

    def send(self, fn, *args):
        if not DEV_DELAY and not DEV_JITTER:
            fn(*args)
            return
        loop = asyncio.get_running_loop()
        self.last = max(loop.time() + DEV_DELAY + random.random() * DEV_JITTER, self.last)
        loop.call_at(self.last, fn, *args)


def udp_socket(address):
    """A UDP socket bound to address that ignores Windows' unreachable reports. Windows answers an ICMP port
    unreachable for an earlier send (a player whose game closed) by failing the socket's next receive with
    WSAECONNRESET; asyncio's Proactor loop then calls error_received and never reads that socket again, so one player
    who quit uncleanly left the host deaf to everyone, rejoining included (TekRantGaming's list of 9 Nov 2026, item 6)."""
    sock = socket.socket(socket.AF_INET6 if ':' in address[0] else socket.AF_INET, socket.SOCK_DGRAM)
    if sys.platform == 'win32':
        import ctypes
        from ctypes import wintypes
        ioctl = ctypes.WinDLL('ws2_32').WSAIoctl
        ioctl.argtypes = [ctypes.c_size_t, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                          wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p, ctypes.c_void_p]
        off, got = wintypes.BOOL(False), wintypes.DWORD()
        for code in (0x9800000C, 0x9800000F):  # SIO_UDP_CONNRESET, SIO_UDP_NETRESET (TTL expired)
            ioctl(sock.fileno(), code, ctypes.byref(off), ctypes.sizeof(off), None, 0, ctypes.byref(got), None, None)
    sock.bind(address)
    return sock


def read_again(transport):
    """After a receive error asyncio's Proactor loop (Windows) stops reading that datagram socket for good; start it
    again. The socket above should never get one; this keeps a host joinable if some other error does."""
    if transport and not transport.is_closing() and getattr(transport, '_read_fut', 0) is None \
            and hasattr(transport, '_loop_reading'):
        transport._loop_reading()


def official_code(key, name, stamp=None, nonce=None):
    """The proof an official server sends when it registers: HMAC SHA256 of the time, a random value and its name
    under the official key (official.key, TekRantGaming's own; never in git or a download). The backend takes a code once and
    only within two minutes of its time."""
    import hashlib
    import hmac
    stamp = int(time.time()) if stamp is None else int(stamp)
    nonce = os.urandom(8).hex() if nonce is None else nonce
    mac = hmac.new(key, ('%d:%s:%s' % (stamp, nonce, name)).encode('utf-8'), hashlib.sha256).hexdigest()
    return {'stamp': stamp, 'nonce': nonce, 'mac': mac}


def read_key(path):
    with open(path, 'rb') as f:
        key = f.read().strip()
    if len(key) < 32:
        raise ValueError('the official key is too short: %s' % path)
    return key


def i8(v):
    return bytes([(v + 0x80) & 0xFF])


def i16(v):
    return struct.pack('>H', (v + 0x8000) & 0xFFFF)


def i32(v):
    return struct.pack('>I', (v + 0x80000000) & 0xFFFFFFFF)


def text(s):
    b = s.encode('latin-1')
    return i32(len(b)) + b


def tunnel_ident(user_id):
    """The id a tunnel goes by: a hash of the user id's decimal text (the module's own id, +0x10)."""
    h = 0
    for c in str(user_id).encode():
        h = ((h >> 27) ^ (h << 5) ^ c) & 0xFFFFFFFF
    return h


def xnaddr(ip, enet=b'\xcc' * 6):
    """An XNADDR: ina, inaOnline, wPortOnline, abEnet, abOnline (20 bytes)."""
    return bytes(int(x) for x in ip.split('.')) + b'\0' * 6 + enet + b'\0' * 20


def player_blob(pid, slot, name, xuid, address, port, stamp):
    """A roster player as a console host writes it (cap1: the host's own entry and the joiner's)."""
    return (i32(pid) + i32(slot) + i32(slot) + text(name) + i8(0) + struct.pack('>Q', stamp) +
            struct.pack('>Q', xuid) + i8(0) + i8(2) + address + struct.pack('>H', port) +
            b'\x00\x80' + b'\0' * 7)


class Player:
    def __init__(self, pid, name, xuid, address, ticket):
        self.pid, self.name, self.xuid, self.address, self.ticket = pid, name, xuid, address, ticket


# ---- voice and text chat (bf/src/comms.cpp is the other end; docs/COMMS-PLAN.md)
# Every packet: COMMS, a type byte, then big endian fields. A client's comms socket is its own (not the game's
# tunnel), so its address differs from the game's; it is let in only as a player joined to this server, from the
# same IP. Client to host: HELLO, STATE (every half second: team, squad, in the world, where), CHAT, VOICE, BYE.
# Host to client: WELCOME (its id), REFUSED, CHAT_RELAY, ROSTER (every 5 s), VOICE_RELAY (a VOICE's body as sent).
C_HELLO, C_WELCOME, C_STATE, C_CHAT_RELAY, C_CHAT, C_ROSTER, C_VOICE, C_VOICE_RELAY, C_BYE, C_REFUSED = range(1, 11)
# A server broadcast (TekRantGaming, 13 Nov 2026: "any server broadcast should be shown in a box at the top center of
# the screen ... remain on screen for 10s and play a sound ... to announce things like game updates and server restarts
# also the server admins will have an option to send custom text messages"): the host to every player on the server,
# id (2 bytes), kind (BROADCAST_KINDS), seconds on screen, the text. A game older than v0.4.0 ignores the kind.
C_BROADCAST = 11
BROADCAST_KINDS = {'custom': 0, 'update': 1, 'restart': 2}
COMMS_PROTOCOL = 1
CHAT_ALL, CHAT_TEAM, CHAT_SQUAD = 0, 1, 2
# metres: a listener further from the speaker gets nothing (the client fades out sooner). BF_DEV_VOICE_RANGE (tests
# only): another range, so two test players far apart on the map still hear each other.
VOICE_RANGE = float(os.environ.get('BF_DEV_VOICE_RANGE') or 80.0)
CHAT_MAX = 200          # bytes of text
VOICE_MAX = 400         # bytes of voice in one packet


class CommsClient:
    def __init__(self, cid, addr, name):
        self.id, self.addr, self.name = cid, addr, name
        self.team = self.squad = self.flags = 0
        self.pos = None
        self.last_heard = time.monotonic()
        self.chat_times = []
        self.chat_seen = []  # the last message numbers: each message comes twice (UDP may lose one)
        self.voice_second, self.voice_count = 0, 0


class Comms:
    """The relay for one server's voice and text chat."""

    def __init__(self, host):
        self.host = host
        self.clients = {}    # addr -> CommsClient
        self.ids = {}        # name -> id, kept for the server's life so a rejoin keeps its id
        self.next_id = 1
        self.roster_at = 0.0
        # Broadcasts: asked for through --broadcast-file, a JSON file the server manager writes ({"seq", "kind",
        # "text", "seconds"}); each new seq is sent to everyone, three times a third of a second apart (it is UDP;
        # the games show an id once).
        self.broadcast_file = ''
        self.broadcast_seq = None   # the file's seq last seen (None: not looked at yet, so an old file is not sent)
        self.broadcast_id = 0
        self.broadcast_due = []     # (monotonic time, body)
        self.broadcast_looked = 0.0
        self.broadcast_last = (0.0, b'')  # (monotonic time it leaves the screens, body): a player who links before
                                          # then gets it too (handle, C_HELLO)

    def send(self, kind, body, addr):
        self.host.transport.sendto(COMMS + bytes([kind]) + body, addr)

    def broadcast(self, text_, kind='custom', seconds=10):
        text_ = ' '.join(''.join(c if c.isprintable() else ' ' for c in str(text_)).split())[:200]
        if not text_:
            return
        self.broadcast_id = (self.broadcast_id % 0xFFFF) + 1
        body = struct.pack('>HBB', self.broadcast_id, BROADCAST_KINDS.get(kind, 0),
                           max(3, min(30, int(seconds)))) + short_text(text_)
        now = time.monotonic()
        self.broadcast_due += [(now, body), (now + 0.35, body), (now + 0.7, body)]
        self.broadcast_last = (now + body[3], body)
        self.host.note('comms: broadcast %d (%s) to %d players: %s' % (self.broadcast_id, kind, len(self.clients), text_))

    def read_broadcast_file(self, now):
        if not self.broadcast_file or now - self.broadcast_looked < 0.5:
            return
        self.broadcast_looked = now
        try:
            with open(self.broadcast_file, encoding='utf-8') as f:
                m = json.load(f)
            seq = int(m.get('seq', 0))
        except (OSError, ValueError, TypeError, AttributeError):
            if self.broadcast_seq is None:
                self.broadcast_seq = 0  # no file yet: the first one written is new
            return
        if self.broadcast_seq is None:
            self.broadcast_seq = seq  # as the host starts: whatever is there was for an earlier run
            return
        if seq == self.broadcast_seq:
            return
        self.broadcast_seq = seq
        try:
            seconds = int(m.get('seconds', 10))
        except (TypeError, ValueError):
            seconds = 10
        self.broadcast(m.get('text', ''), str(m.get('kind', 'custom')), seconds)

    def joined_player(self, name, addr):
        return any(p.state == 'joined' and p.player and p.player.name == name and p.addr[0] == addr[0]
                   for p in self.host.peers.values())

    def receive(self, data, addr):
        try:
            self.handle(data, addr)
        except (struct.error, IndexError, UnicodeDecodeError, ValueError):
            pass  # a malformed packet: dropped

    def handle(self, data, addr):
        kind, body = data[0], data[1:]
        now = time.monotonic()
        client = self.clients.get(addr)
        if kind == C_HELLO:
            proto, team, squad, n = struct.unpack_from('>BBBB', body)
            name = body[4:4 + n].decode('utf-8')
            if proto != COMMS_PROTOCOL:
                self.send(C_REFUSED, bytes([2]) + short_text('this server speaks comms version %d' % COMMS_PROTOCOL),
                          addr)
                return
            if not self.joined_player(name, addr):
                self.send(C_REFUSED, bytes([1]) + short_text('not in the game on this server yet'), addr)
                return
            fresh = not client
            if not client:
                for other_addr, other in list(self.clients.items()):
                    if other.name == name:
                        del self.clients[other_addr]  # the same player from a new socket
                cid = self.ids.get(name)
                if cid is None:
                    cid = self.ids[name] = self.next_id
                    self.next_id = self.next_id % 65535 + 1
                client = self.clients[addr] = CommsClient(cid, addr, name)
                self.host.note('comms: %s in (id %d)' % (name, cid))
                self.roster_at = 0  # everyone gets the new roster at the next tick
            client.team, client.squad, client.last_heard = team, squad, now
            self.send(C_WELCOME, struct.pack('>HB', client.id, COMMS_PROTOCOL), addr)
            if fresh and now < self.broadcast_last[0]:
                self.send(C_BROADCAST, self.broadcast_last[1], addr)  # the message still on the others' screens
            return
        if not client:
            self.send(C_REFUSED, bytes([3]) + short_text('say hello first'), addr)
            return
        client.last_heard = now
        if kind == C_STATE:
            cid, client.team, client.squad, client.flags, x, y, z = struct.unpack_from('>HBBBfff', body)
            client.pos = (x, y, z) if client.flags & 1 and all(abs(v) < 1e6 for v in (x, y, z)) else None
        elif kind == C_CHAT:
            cid, msg, channel, n = struct.unpack_from('>HHBB', body)
            text_ = body[6:6 + min(n, CHAT_MAX)].decode('utf-8', 'replace').strip()
            if msg in client.chat_seen:
                return  # the second copy: sent on already
            client.chat_seen = client.chat_seen[-31:] + [msg]
            client.chat_times = [t for t in client.chat_times if now - t < 5] + [now]
            if not text_ or len(client.chat_times) > 5 or channel not in (CHAT_ALL, CHAT_TEAM, CHAT_SQUAD):
                return  # empty, more than 5 in 5 s, or no such channel
            if (channel == CHAT_TEAM and not client.team) or (channel == CHAT_SQUAD and not client.squad):
                return  # its team or squad not known (the client says so itself): never to the other team
            name = client.name.encode('utf-8')[:64]
            raw = text_.encode('utf-8')[:CHAT_MAX]
            relay = struct.pack('>HHBBB', client.id, msg, channel, client.team, len(name)) + name + bytes([len(raw)]) + raw
            for other in self.clients.values():
                if channel == CHAT_TEAM and other.team != client.team:
                    continue
                if channel == CHAT_SQUAD and (other.team != client.team or other.squad != client.squad or
                                              not client.squad):
                    continue
                for _ in range(2):  # twice: a lost datagram still leaves one (the client keeps the first)
                    self.send(C_CHAT_RELAY, relay, other.addr)
            self.host.note('comms chat %s %s: %s' % (('all', 'team', 'squad')[channel], client.name, text_[:80]))
        elif kind == C_VOICE:
            cid, seq, x, y, z, codec, n = struct.unpack_from('>HHfffBH', body)
            if n > VOICE_MAX or len(body) < 19 + n:
                return
            second = int(now)
            if second != client.voice_second:
                client.voice_second, client.voice_count = second, 0
            client.voice_count += 1
            if client.voice_count > 60:  # 50 a second is a full 20 ms stream
                return
            relay = struct.pack('>H', client.id) + body[2:19 + n]
            for other in self.clients.values():
                if other is client:
                    continue
                if other.pos is not None:
                    d2 = (other.pos[0] - x) ** 2 + (other.pos[1] - y) ** 2 + (other.pos[2] - z) ** 2
                    if d2 > VOICE_RANGE * VOICE_RANGE:
                        continue
                self.send(C_VOICE_RELAY, relay, other.addr)
        elif kind == C_BYE:
            self.host.note('comms: %s out' % client.name)
            del self.clients[addr]
            self.roster_at = 0

    def tick(self, now):
        try:
            self.read_broadcast_file(now)
        except Exception as e:  # never the host's own loop for a message (run bcast1: an error here stopped it)
            if not getattr(self, 'broadcast_error', None):
                self.broadcast_error = True
                self.host.note('comms: the broadcast file could not be read (%s: %s)' % (type(e).__name__, e))
        if self.broadcast_due:
            for at, body in [d for d in self.broadcast_due if d[0] <= now]:
                for c in self.clients.values():
                    self.send(C_BROADCAST, body, c.addr)
            self.broadcast_due = [d for d in self.broadcast_due if d[0] > now]
        for addr, client in list(self.clients.items()):
            if now - client.last_heard > 10 or not self.joined_player(client.name, addr):
                self.host.note('comms: %s out (%s)' % (client.name, 'silent' if now - client.last_heard > 10
                                                        else 'not in the game'))
                del self.clients[addr]
                self.roster_at = 0
        if self.clients and now - self.roster_at > 5:
            self.roster_at = now
            entries = b''
            for c in list(self.clients.values())[:64]:
                name = c.name.encode('utf-8')[:64]
                entries += struct.pack('>HBBB', c.id, c.team, c.squad, len(name)) + name
            body = bytes([min(len(self.clients), 64)]) + entries
            for c in self.clients.values():
                self.send(C_ROSTER, body, c.addr)


def short_text(s):
    b = s.encode('utf-8')[:255]
    return bytes([len(b)]) + b


class Rejoin(Exception):
    """A connected client's GameManager linking again with a new connection id (it left the game and joins again from
    the same address and port): its old session is dropped and a fresh one takes the datagram."""


class Peer:
    """One client's link to us."""

    def __init__(self, host, addr):
        self.host, self.addr = host, addr
        self.connident = 0
        self.connected = False
        self.peer_bound = False       # we have the client's control id
        self.client_bound = False     # the client says it has ours (its control's bound byte)
        self.next_seq = DATA          # our next reliable seq
        self.peer_ack = DATA - 1      # the last reliable seq we have from the client, in order
        self.unrel = 0
        self.unacked = []             # (seq, payload, last send time)
        self.player = None
        self.state = 'link'
        self.server = None            # our socket to the dedicated server for this client
        self.clock0 = time.monotonic()
        self.last_heard = time.monotonic()

    def note(self, text_):
        self.host.note('%s:%d %s' % (self.addr[0], self.addr[1], text_))

    # ---- sending

    def datagram(self, subs):
        heads = b''.join(struct.pack('>H', (len(d) << 4) | port) for port, d in subs)
        body = heads + b''.join(d for _, d in subs)
        return struct.pack('>H', len(body)) + body

    def control(self):
        return CONTROL, b'\0' + struct.pack('>I', self.host.ident) + bytes([1 if self.peer_bound else 0])

    def send_subs(self, subs):
        if not self.client_bound:
            subs = [self.control()] + subs
        self.host.transport.sendto(self.datagram(subs), self.addr)

    def link_packet(self, seq, body):
        return LINK, struct.pack('>II', seq, self.peer_ack if seq >= UNREL else self.connident) + body

    def sync(self):
        now = int((time.monotonic() - self.clock0) * 1000) & 0xFFFFFFFF
        return struct.pack('>IIH', now, now, 0)

    def send_reliable(self, data, kind=KIND_STREAM, with_sync=False):
        payload = data + (self.sync() if with_sync else b'') + bytes([kind | (KIND_SYNC if with_sync else 0)])
        seq = self.next_seq
        self.next_seq += 1
        self.unacked.append([seq, payload, time.monotonic()])
        self.send_subs([self.link_packet(seq, payload)])

    def send_unreliable(self, data, kind=KIND_DATAGRAM):
        seq = UNREL + (self.unrel & 0x7F)
        self.unrel += 1
        self.send_subs([self.link_packet(seq, data + bytes([kind]))])

    def send_ack(self):
        self.send_subs([self.link_packet(self.next_seq, b'')])

    def gm(self, body):
        """A GameManager message (SendData flag 1)."""
        self.send_reliable(b'\x01' + body)

    def resend(self):
        now = time.monotonic()
        for item in self.unacked:
            if now - item[2] >= RESEND_SECONDS:
                item[2] = now
                self.send_subs([self.link_packet(item[0], item[1])])

    # ---- receiving

    def receive(self, data):
        self.last_heard = time.monotonic()
        if len(data) < 4:
            return
        heads, at = [], 2
        while at + 2 <= len(data):
            h = struct.unpack_from('>H', data, at)[0]
            heads.append((h >> 4, h & 15))
            at += 2
            if at + sum(n for n, _ in heads) >= len(data):
                break
        for n, port in heads:
            sub = data[at:at + n]
            at += n
            if port == CONTROL and len(sub) >= 6:
                if not self.peer_bound:
                    self.peer_bound = True
                    self.note('tunnel: client id %08X' % struct.unpack_from('>I', sub, 1)[0])
                if sub[5] and not self.client_bound:
                    self.client_bound = True
                    self.note('tunnel bound both ways')
            elif port == LINK and len(sub) >= 8:
                self.link(sub)
            # port 0: voice chat (COd connect, PNG ping, DSC): no voice server here

    def link(self, sub):
        seq_word, ack = struct.unpack_from('>II', sub, 0)
        seq = seq_word & 0x0FFFFFFF
        body = sub[8:]
        parts = []
        for _ in range(seq_word >> 28):
            if not body:
                break
            n = body[-1]
            parts.insert(0, body[-1 - n:-1])
            body = body[:-1 - n]
        parts.insert(0, body)
        if seq == INIT:
            if self.connected and ack != self.connident:
                # A new link from a client we had: the old session's sequence numbers and its socket to the server
                # are stale (TekRantGaming could not rejoin after leaving a game: every INIT was answered, every packet after
                # it dropped as out of order).
                raise Rejoin()
            self.connident = ack
            self.note('link INIT %08X' % ack)
            self.send_subs([(LINK, struct.pack('>II', CONN, self.connident))])
            if not self.connected:
                self.connected = True
                self.send_reliable(b'', kind=0, with_sync=True)  # the host's first data packet: a sync (cap1)
            return
        if seq == CONN:
            self.connected = True
            return
        if seq >= DATA:
            self.unacked = [u for u in self.unacked if u[0] > ack]
            got = False
            for k in range(len(parts) - 1, -1, -1):
                if seq - k == self.peer_ack + 1 and parts[k]:
                    self.peer_ack += 1
                    got = True
                    self.payload(parts[k])
            if got:
                self.send_ack()
        elif seq >= UNREL:
            self.unacked = [u for u in self.unacked if u[0] > ack]
            if parts[0]:
                self.payload(parts[0])

    def payload(self, p):
        kind = p[-1]
        data = p[:-1]
        if kind & KIND_SYNC:
            data = data[:-10]
        kind &= 0x3F
        if not data:
            return
        flag, body = data[0], data[1:]
        if kind == KIND_STREAM and flag == 1:
            self.message(body)
        elif kind == KIND_DATAGRAM:
            self.to_server(body)
        else:
            self.note('game link kind %d flag %d: %s' % (kind, flag, body.hex().upper()[:80]))

    # ---- GameManager

    def message(self, m):
        t = m[0] - 0x80
        if t == 0:  # hello: u16 version, the ticket as text
            n = struct.unpack_from('>I', m, 3)[0] - 0x80000000
            ticket = m[7:7 + n].decode('latin-1')
            self.player = self.host.players.pop(ticket, None)
            if not self.player:
                self.note('hello with unknown ticket %s' % ticket)
                return
            self.note('hello from %s (PID %d)' % (self.player.name, self.player.pid))
            self.send_roster()
        elif t == 4:  # roster ack
            self.note('roster ack')
            self.send_join()
        elif t == 1:  # the player leaves the game
            self.note('left the game')
            self.host.drop(self.addr)
        else:
            self.note('GameManager message %d: %s' % (t, m.hex().upper()[:80]))

    def send_roster(self):
        h, p = self.host, self.player
        host_blob = player_blob(h.pid, 0, h.name, h.xuid, xnaddr(h.public[0]), 6001, 0xB86)
        game = (i8(2) + i16(0x14) + text(h.name) + i16(0) + i8(1) + i8(1) + i8(1) + b'\0\0' + b'\0\0\x01\0' +
                i8(0) + b'\x01' + b'\x00\x10\x01\x00\x00\x00' + b'\0' * 8 + struct.pack('>H', 2) + b'\x01' +
                host_blob)
        self.gm(game)
        self.gm(i8(3) + self.joiner_blob(0xD83))
        self.state = 'roster'

    def joiner_blob(self, stamp):
        p = self.player
        return player_blob(p.pid, 1, p.name, p.xuid, p.address, 1000, stamp)

    def send_join(self):
        pid = self.player.pid
        self.gm(i8(5) + i32(pid) + self.joiner_blob(0xD85))
        self.gm(i8(8) + i32(pid))
        self.gm(i8(9) + i32(pid) + bytes.fromhex('80000000800000000000000000000400800000000000000000'))
        self.gm(i8(0x15) + b'\x01' + i32(self.host.pid) + b'\x00')
        self.state = 'joined'
        self.note('joined: game packets now go to the server')

    # ---- game packets

    def to_server(self, body):
        if not self.server:
            loop = asyncio.get_running_loop()
            self.server = ServerSide(self)
            loop.create_task(loop.create_datagram_endpoint(lambda: self.server,
                                                           sock=udp_socket((self.host.server[0], 0))))
            self.server.pending.append(body)
            return
        self.server.send(body)

    def from_server(self, body):
        self.send_unreliable(b'\x00' + body)


class ServerSide(asyncio.DatagramProtocol):
    """One client's socket to the dedicated server: its game socket takes VDP datagrams (u16 length first)."""

    def __init__(self, peer):
        self.peer, self.transport, self.pending = peer, None, []
        self.up, self.down = Held(), Held()  # BF_DEV_NET_DELAY (tests)

    def connection_made(self, transport):
        self.transport = transport
        for b in self.pending:
            self.send(b)
        self.pending = []

    def send(self, body):
        self.up.send(self.send_now, body)

    def send_now(self, body):
        if self.transport:
            self.transport.sendto(struct.pack('>H', len(body)) + body, self.peer.host.server)
        else:
            self.pending.append(body)

    def datagram_received(self, data, addr):
        if len(data) >= 2:
            self.down.send(self.peer.from_server, data[2:2 + struct.unpack_from('>H', data)[0]])

    def error_received(self, exc):
        asyncio.get_running_loop().call_soon(read_again, self.transport)


class GameHost(asyncio.DatagramProtocol):
    """The host side of one listed game: listens where the backend's EGEG points joiners (I, P)."""

    def __init__(self, address, server, user_id, pid, name, log, public=None):
        self.address, self.server = address, server
        self.public = public or address  # where clients reach us (EGEG I and P, the host's XNADDR)
        self.user_id, self.pid, self.name = user_id, pid, name
        self.ident = tunnel_ident(user_id)
        self.xuid = 0x0009000000000000 + user_id
        self.log = log
        self.players = {}  # ticket -> Player, from the backend's EGAM
        self.peers = {}
        self.comms = Comms(self)
        self.transport = None
        self.error_noted = 0  # monotonic time of the last socket error written to the log

    def note(self, text_):
        line = '%s GMHOST %s' % (time.strftime('%H:%M:%S'), text_)
        print(line, flush=True)
        if self.log:
            self.log.write(line + '\n')
            self.log.flush()

    def expect(self, ticket, pid, name, xuid, address_b64):
        import base64
        try:
            address = base64.b64decode(address_b64)[:36]
        except ValueError:
            address = b''
        if len(address) != 36:
            address = xnaddr('0.0.0.0')
        self.players[ticket] = Player(int(pid), name, int(xuid or 0), address, ticket)
        self.players[ticket].expected_at = time.monotonic()

    def connection_made(self, transport):
        self.transport = transport
        self.note('listening on %s:%d for game server %s:%d' % (self.address + self.server))
        if DEV_DELAY or DEV_JITTER:
            self.note('test delay: game datagrams held %.0f ms each way, plus up to %.0f ms' % (
                DEV_DELAY * 1000, DEV_JITTER * 1000))
        asyncio.get_running_loop().create_task(self.tick())

    def datagram_received(self, data, addr):
        # The server browser's ping and the backend's reachability probe (SERVERS-PLAN.md sections 5 and 6): answered
        # at once, never a game packet (those start with a flag byte, 0 or 1).
        if data[:8] in (PING, PROBE):
            self.transport.sendto(PONG + data[8:40], addr)
            return
        # Voice and text chat: from a client's comms socket, never the game's tunnel address.
        if data[:8] == COMMS and addr not in self.peers and len(data) > 8:
            self.comms.receive(data[8:], addr)
            return
        peer = self.peers.get(addr)
        if not peer:
            peer = self.peers[addr] = Peer(self, addr)
            peer.note('new client')
        try:
            peer.receive(data)
        except Rejoin:
            peer.note('linking again with a new connection: its old session is dropped')
            self.drop(addr)
            peer = self.peers[addr] = Peer(self, addr)
            peer.note('new client (rejoin)')
            peer.receive(data)

    def error_received(self, exc):
        if time.monotonic() - self.error_noted > 10:
            self.error_noted = time.monotonic()
            self.note('socket error (%s), still listening' % exc)
        asyncio.get_running_loop().call_soon(read_again, self.transport)

    def drop(self, addr):
        peer = self.peers.pop(addr, None)
        if peer and peer.server and peer.server.transport:
            peer.server.transport.close()

    def player_count(self):
        return sum(1 for p in self.peers.values() if p.state == 'joined')

    def player_names(self):
        """The joined players' names, for the backend's Join Match With Friends (our main menu)."""
        return [p.player.name for p in self.peers.values() if p.state == 'joined' and p.player][:64]

    async def tick(self):
        pruned_at = time.monotonic()
        while True:
            await asyncio.sleep(0.1)
            now = time.monotonic()
            for addr, peer in list(self.peers.items()):
                peer.resend()
                if now - peer.last_heard > 30:
                    peer.note('silent for 30 s, dropped')
                    self.drop(addr)
            self.comms.tick(now)
            # A join the backend announced whose client never linked (its game failed to connect): forgotten after
            # 5 minutes, so a server that runs for weeks keeps no list of them.
            if now - pruned_at > 10:
                pruned_at = now
                for ticket in [t for t, p in self.players.items() if now - getattr(p, 'expected_at', now) > 300]:
                    del self.players[ticket]


async def start(address, server, user_id, pid, name, log, public=None, sock=None):
    host = GameHost(address, server, user_id, pid, name, log, public)
    await asyncio.get_running_loop().create_datagram_endpoint(lambda: host, sock=sock or udp_socket(address))
    return host


# ---- the server package's side: register with the backend and take its join notices

async def serve(args):
    """Run one game: our GameManager host in front of a dedicated server, listed by the backend's server
    registry (one JSON object per line over TCP, see plasma.py's Registry)."""
    import json
    log = open(args.log, 'a') if args.log else None
    listen = parse_address(args.listen)
    public = parse_address(args.public) if args.public else listen
    backend = parse_address(args.backend)
    while True:
        try:
            reader, writer = await asyncio.open_connection(*backend)
        except OSError as e:
            print('%s backend %s:%d not reachable (%s), retrying' % (time.strftime('%H:%M:%S'), *backend, e),
                  flush=True)
            await asyncio.sleep(5)
            continue
        send = lambda obj: writer.write((json.dumps(obj) + '\n').encode())
        # The address comes from the connection (the backend sees it; nothing to type, SERVERS-PLAN.md section 6);
        # --public only when given (older packages and tests).
        reg = {'op': 'register', 'name': args.name, 'port': public[1], 'map': args.map,
               'max_players': args.max_players, 'mode': 'classic', 'description': args.description[:200],
               'maps': [m for m in args.maps.split(',') if m], 'listed': not args.unlisted, 'night': args.night,
               'region': args.region, 'tick': args.tick}
        # A host on one address of its own (tests: 127.0.0.2) says where; one on every address (the packages: 0.0.0.0)
        # is found by the backend.
        if args.public or listen[0] not in ('0.0.0.0', '::'):
            reg['address'] = list(public)
        if args.official_key_file:
            reg['official'] = official_code(read_key(args.official_key_file), args.name)
        # Listening before the backend is told: its reachability probe comes at once.
        sock = udp_socket(listen)
        send(reg)
        await writer.drain()
        reply = json.loads(await reader.readline())
        if reply.get('op') == 'refused':
            print('%s backend refused the server: %s' % (time.strftime('%H:%M:%S'), reply.get('why')), flush=True)
            write_status(args, {'state': 'refused', 'why': reply.get('why', '')})
            sock.close()
            writer.close()
            await asyncio.sleep(30)
            continue
        host = await start(listen, parse_address(args.game_server), reply['uid'], reply['pid'], args.name, log,
                           public, sock)
        host.comms.broadcast_file = args.broadcast_file
        host.note('registered with the backend as game %s%s%s' % (
            reply['gid'], ' (official)' if reply.get('official') else '', '' if reply.get('listed', True) else
            ' (not listed: Direct connect only)'))
        state = {'state': 'registered', 'gid': reply['gid'], 'official': bool(reply.get('official')),
                 'listed': reply.get('listed', True), 'address': reply.get('address', ''), 'reachable': None}
        write_status(args, state)

        reported = [args.map]

        async def heartbeat():
            try:
                await beat_loop()
            except Exception as e:  # a dead heartbeat left the list's players and map stale
                host.note('heartbeat failed: %r' % e)

        async def beat_loop():
            while True:
                # The map now running: the server's rotation writes it (server_rotation.cpp), else --map.
                current = args.map
                if args.map_file:
                    try:
                        with open(args.map_file, encoding='utf-8') as f:
                            current = f.read().strip() or args.map
                    except OSError:
                        pass
                if current != reported[0]:
                    host.note('map now %s' % current)
                    reported[0] = current
                rotation = [m for m in args.maps.split(',') if m]
                upcoming = rotation[(rotation.index(current) + 1) % len(rotation)] if current in rotation else ''
                send({'op': 'status', 'players': host.player_count(), 'map': current, 'next_map': upcoming,
                      'names': host.player_names()})
                write_status(args, dict(state, players=host.player_count(), map=current, next_map=upcoming,
                                        names=host.player_names()))
                await writer.drain()
                await asyncio.sleep(5)

        beat = asyncio.get_running_loop().create_task(heartbeat())
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                m = json.loads(line)
                if m.get('op') == 'reachable':
                    # null: it answers, but it runs on the main backend's own machine, so whether players outside
                    # reach it is not known (the backend's probe skips the machine's firewall)
                    state['reachable'] = None if m.get('ok') is None else bool(m.get('ok'))
                    state['reachable_local'] = bool(m.get('local'))
                    write_status(args, state)
                    host.note('backend: %s at %s' % (
                        'players can reach this server' if m.get('ok')
                        else 'this server answers on the backend\'s own machine; whether players outside reach it '
                             'is not checked from there' if m.get('ok') is None
                        else 'players cannot reach this server', m.get('address', '?')))
                elif m.get('op') == 'join':
                    host.expect(m['ticket'], m['pid'], m['name'], m['xuid'], m['xnaddr'])
                    host.note('backend: %s joins (PID %s)' % (m['name'], m['pid']))
        except (ConnectionError, ValueError) as e:
            host.note('backend link lost (%s)' % type(e).__name__)
        beat.cancel()
        host.transport.close()
        await asyncio.sleep(2)


def write_status(args, state):
    """The server package's window reads each server's state from here (--status-file)."""
    if not args.status_file:
        return
    import json
    try:
        with open(args.status_file + '.tmp', 'w', encoding='utf-8') as f:
            json.dump(dict(state, at=int(time.time())), f)
        os.replace(args.status_file + '.tmp', args.status_file)
    except OSError:
        pass


def parse_address(text):
    h, p = text.rsplit(':', 1)
    return h, int(p)


def main():
    import argparse
    ap = argparse.ArgumentParser(description='GameManager host for one BF1943 dedicated server')
    ap.add_argument('--backend', default='127.0.0.1:18250', help="the backend's server registry")
    ap.add_argument('--listen', default='0.0.0.0:1000', help='where joining clients link to us (UDP)')
    ap.add_argument('--public', default='', help='the address clients use to reach --listen (default: --listen)')
    ap.add_argument('--game-server', default='127.0.0.1:1003', help="the dedicated server's game socket (UDP)")
    ap.add_argument('--name', default='BF1943 server')
    ap.add_argument('--map', default='Wake Island')
    ap.add_argument('--max-players', type=int, default=24)
    # Movement and aiming: every server plays the game's own since v0.4.0 (Modern was retired; TekRantGaming, 13 Nov
    # 2026). The option is still accepted, so an older start script does no harm, and is not used: the server is
    # registered as classic, which is what the game plays.
    ap.add_argument('--mode', choices=['classic', 'modern'], default='classic',
                    help='not used since v0.4.0: every server plays the original movement and aiming')
    # Night Ops (TekRantGaming, 11 Nov 2026): the maps at night, shown in the server list; players' games draw it so
    # (NativeXRenderer D162) while they play there.
    ap.add_argument('--night', action='store_true', help='a night server: players see its maps at night')
    # Where the server is (TekRantGaming, 13 Nov 2026: the browser shows the region with its flag): one of the game's
    # region codes (bf/src/regions.h, the manager's drop down). And its tick rate, for the browser: the game's own
    # 30 Hz today.
    ap.add_argument('--broadcast-file', default='',
                    help='a JSON file the server manager writes to send a message to every player on the server: '
                         '{"seq": a new number each time, "kind": "custom", "update" or "restart", "text", "seconds"}')
    ap.add_argument('--region', default='', help="the server's region code, e.g. gb, de, us-east (shown in the browser)")
    ap.add_argument('--tick', type=int, default=60, help="the server's tick rate in Hz (shown in the browser; 60 by default since 14 Nov 2026)")
    ap.add_argument('--map-file', default='', help="the server's current_map.txt (its map rotation)")
    ap.add_argument('--maps', default='', help='the rotation, map titles separated by commas (for the next map)')
    ap.add_argument('--description', default='', help='shown in the server browser (200 characters)')
    ap.add_argument('--unlisted', action='store_true', help='registered but not listed: Direct connect only')
    ap.add_argument('--official-key-file', default='', help='official.key: the server shows as official')
    ap.add_argument('--status-file', default='', help="this server's state for the package's window (JSON)")
    ap.add_argument('--log', default='')
    asyncio.run(serve(ap.parse_args()))


if __name__ == '__main__':
    main()
