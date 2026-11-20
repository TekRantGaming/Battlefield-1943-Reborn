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
import math
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
# At most this many clients that have not joined at a time (the one heard from longest ago gives way to a new one),
# and at most this many log lines in 10 s.
MAX_LINKING = 64
NOTE_BUDGET = 200


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


def dev_net_delay_for():
    """Tests only: BF_DEV_NET_DELAY_FOR=<ip>=<ms>[;<ip>=<ms>...] holds one client's game datagrams (by its address)
    that long each way instead of BF_DEV_NET_DELAY's: players at different distances in one test (the scoreboards'
    ping, 20 Nov 2026)."""
    out = {}
    for part in os.environ.get('BF_DEV_NET_DELAY_FOR', '').split(';'):
        ip, _, ms = part.partition('=')
        try:
            out[ip.strip()] = (float(ms) / 1000.0, 0.0)
        except ValueError:
            pass
    return out


DEV_DELAY_FOR = dev_net_delay_for()


def dev_delay(ip):
    """The test delay each way (and jitter) for a client at that address: (0, 0) unless a test sets one."""
    return DEV_DELAY_FOR.get(ip, (DEV_DELAY, DEV_JITTER))


class Held:
    """One direction's datagrams held a test's delay plus up to its jitter (dev_delay), sent in the order they came."""

    def __init__(self, delay=None):
        self.last = 0.0
        self.delay, self.jitter = delay if delay is not None else (DEV_DELAY, DEV_JITTER)

    def send(self, fn, *args):
        if not self.delay and not self.jitter:
            fn(*args)
            return
        loop = asyncio.get_running_loop()
        self.last = max(loop.time() + self.delay + random.random() * self.jitter, self.last)
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


# ---- The build check (16 Nov 2026: a community server listed through our backend ran a modified build with bots; the
# backend now lists a community server only when the package it runs is an official build, else it is hidden from the
# list and Quick Match and joined by Direct connect only). The server package's program files are hashed here and the
# backend compares their digest with the official one for that version (tools/build_manifest.py writes it at a
# release). Only our own files: never the player's game files. Official servers prove themselves with official.key.

BUILD_FOLDERS = ('bin', 'server', 'python')
# Files an older official package had and a later one dropped: an update in place leaves them, so they are not counted.
RETIRED_FILES = ('bin/amd_fidelityfx_vk.dll',)


def build_files(root):
    """{path relative to the package: SHA-256} of every program file in the package (bin, server, python), without
    Python's own caches (made while it runs)."""
    import hashlib
    files = {}
    for sub in BUILD_FOLDERS:
        for base, dirs, names in os.walk(os.path.join(root, sub)):
            dirs[:] = sorted(d for d in dirs if d != '__pycache__')
            for n in sorted(names):
                # Python's caches; an update's files moved aside while in use (manager.py, removed at a later start)
                if n.endswith(('.pyc', '.pyo', '.old')):
                    continue
                path = os.path.join(base, n)
                if os.path.relpath(path, root).replace(os.sep, '/') in RETIRED_FILES:
                    continue
                h = hashlib.sha256()
                with open(path, 'rb') as f:
                    for block in iter(lambda: f.read(1 << 20), b''):
                        h.update(block)
                files[os.path.relpath(path, root).replace(os.sep, '/')] = h.hexdigest()
    return files


def build_digest(files):
    """One SHA-256 over every file's path and hash, in path order: the same from the package and from its manifest."""
    import hashlib
    h = hashlib.sha256()
    for path in sorted(files):
        h.update(('%s\0%s\n' % (path, files[path])).encode('utf-8'))
    return h.hexdigest()


def package_root():
    return os.path.abspath(os.environ.get('BF_SERVER_ROOT') or os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def build_report(root=None):
    """What a registration says of its build: the package's version and platform and the digest of its files."""
    root = root or package_root()
    try:
        with open(os.path.join(root, 'version.txt'), encoding='utf-8') as f:
            version = (f.read().strip().splitlines() or [''])[0].strip()
    except OSError:
        version = ''
    files = build_files(root)
    return {'version': version, 'platform': 'windows' if os.name == 'nt' else 'linux', 'digest': build_digest(files),
            'files': len(files)}


def host_key_seed(path):
    """This host's own key (32 random bytes, made once in the data folder by the manager): the backend knows a host
    by it whatever its address. Never sent; the backend gets the public key and a signature."""
    with open(path, 'rb') as f:
        seed = bytes.fromhex(f.read().decode('ascii').strip())
    if len(seed) != 32:
        raise ValueError('the host key is not 32 bytes: %s' % path)
    return seed


def host_proof(seed, name, digest):
    """The host key's signature over this registration's name and build digest, with a random value."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:  # python -I leaves the script's own folder out
        sys.path.insert(0, here)
    import ed25519
    nonce = os.urandom(8).hex()
    text = 'bf1943-register|%s|%s|%s' % (nonce, digest, name)
    return {'key': ed25519.public_key(seed).hex(), 'text': text, 'sig': ed25519.sign(seed, text.encode('utf-8')).hex()}


def read_key(path):
    with open(path, 'rb') as f:
        key = f.read().strip()
    if len(key) < 32:
        raise ValueError('the official key is too short: %s' % path)
    return key


# ---- Network broadcasts (TekRantGaming, 16 Nov 2026: "send the broadcast messages that are sent to the official servers
# to all community servers ... More than likely, it will only be sending messages through the official servers, but
# there can be times where I might need to broadcast to the whole player base"). The official manager signs a message
# with a key made from official.key (broadcast_seed); the main backend checks it and passes it to every server
# registered with it, official ones only (the default) or everyone, over each server's registry connection; each host
# checks it again before its players see it. A host that cannot check it (no key known, a wrong signature, too old,
# seen before, too many) drops it. Hosts from before network broadcasts ignore the message (an op they do not know).
#
#   signed: "bf1943-broadcast|1|<id>|<issued>|<expires>|<scope>|<kind>|<seconds>|<text>"   (Ed25519, hex "sig")
#   scope "official" (official servers only) or "everyone"; kind as BROADCAST_KINDS; seconds on screen 3 to 30;
#   issued and expires in UTC seconds, expires at most 5 minutes after issued; the id 16 hex digits, used once.
#
# The public key community hosts check against: printed by "manager.py broadcast-key" on the official machine (it
# reads official.key there). Until it is written here, community servers show no network broadcast at all; official
# servers make it themselves from their own official.key. Tests only: BF_DEV_BROADCAST_KEY=<public key hex>.
OFFICIAL_BROADCAST_KEYS = ('6884b20490087ce70141d7c944d7ded67ef10718bd5aaff3ad96b8bbf0811822',)
NETWORK_SCOPES = ('official', 'everyone')
NETWORK_TEXT_MAX = 200        # characters, as every broadcast
NETWORK_LIFE = 60             # seconds a signed message is good for after it is made (the manager's)
NETWORK_LIFE_MAX = 300        # the longest a host takes
NETWORK_CLOCK_SLACK = 60      # seconds a host's clock may differ from the official machine's
NETWORK_PER_MINUTE = 6        # at most this many in any minute (the backend and each host)


def broadcast_seed(official_key):
    """The network broadcast signing key (an Ed25519 seed), made from official.key: only the official machine has it."""
    import hashlib
    import hmac
    return hmac.new(official_key, b'bf1943 network broadcast signing key 1', hashlib.sha256).digest()


def _ed25519():
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:  # python -I leaves the script's own folder out
        sys.path.insert(0, here)
    import ed25519
    return ed25519


def broadcast_public(official_key):
    return _ed25519().public_key(broadcast_seed(official_key)).hex()


def clean_broadcast_text(value):
    """A broadcast's text as every host sends it: printable, single spaces, at most NETWORK_TEXT_MAX characters."""
    return ' '.join(''.join(c if c.isprintable() else ' ' for c in str(value)).split())[:NETWORK_TEXT_MAX]


def sign_network(official_key, scope, kind, text_, seconds=10, now=None, life=NETWORK_LIFE, op='network_broadcast'):
    """The signed network broadcast the official manager hands to the main backend."""
    now = int(time.time() if now is None else now)
    signed = 'bf1943-broadcast|1|%s|%d|%d|%s|%s|%d|%s' % (os.urandom(8).hex(), now, now + int(life), scope, kind,
                                                         int(seconds), clean_broadcast_text(text_))
    return {'op': op, 'signed': signed, 'sig': _ed25519().sign(broadcast_seed(official_key), signed.encode('utf-8')).hex()}


def sign_admin(official_key, what, now=None):
    """A question only the official manager may ask the main backend (the community servers' host keys)."""
    now = int(time.time() if now is None else now)
    signed = 'bf1943-admin|1|%s|%d|%s' % (os.urandom(8).hex(), now, what)
    return {'op': 'admin', 'signed': signed, 'sig': _ed25519().sign(broadcast_seed(official_key), signed.encode('utf-8')).hex()}


def trusted_broadcast_keys(official_key=None):
    """The public keys a network broadcast may be signed with, on this host."""
    keys = [k.lower() for k in OFFICIAL_BROADCAST_KEYS]
    keys += [k.strip().lower() for k in os.environ.get('BF_DEV_BROADCAST_KEY', '').split(',') if k.strip()]
    if official_key:
        keys.append(broadcast_public(official_key))
    return [k for k in keys if len(k) == 64]


class NetworkCheck:
    """Checks signed network broadcasts and admin questions: the signature against the trusted keys, the time, an id
    used once, and how many came in the last minute."""

    def __init__(self, keys, per_minute=NETWORK_PER_MINUTE):
        self.keys = []
        for k in keys:
            try:
                self.keys.append(bytes.fromhex(k))
            except ValueError:
                pass
        self.per_minute = per_minute
        self.seen = {}     # id -> time it can be forgotten
        self.times = []    # when the last ones were taken

    def _signature(self, m, prefix):
        signed, sig = m.get('signed'), m.get('sig')
        if not isinstance(signed, str) or not isinstance(sig, str) or len(signed) > 1024 or not signed.startswith(prefix):
            return 'not a signed message'
        if not self.keys:
            return 'no official broadcast key is known to this host'
        try:
            raw = bytes.fromhex(sig)
        except ValueError:
            return 'a signature that is not hex'
        ed = _ed25519()
        if len(raw) != 64 or not any(ed.verify(k, signed.encode('utf-8'), raw) for k in self.keys):
            return 'the signature does not check (not from the official machine)'
        return ''

    def _fresh(self, ident, issued, expires, now):
        if len(ident) != 16 or any(c not in '0123456789abcdef' for c in ident):
            return 'a bad id'
        if expires < issued or expires - issued > NETWORK_LIFE_MAX:
            return 'a bad lifetime'
        if now < issued - NETWORK_CLOCK_SLACK:
            return 'made %d s in the future (is this machine\'s clock right?)' % (issued - now)
        if now > expires + NETWORK_CLOCK_SLACK:
            return 'too old (made %d s ago; a replay, or this machine\'s clock is wrong)' % (now - issued)
        self.seen = {k: t for k, t in self.seen.items() if t > now}
        if ident in self.seen:
            return 'seen before (a replay)'
        self.times = [t for t in self.times if now - t < 60]
        if len(self.times) >= self.per_minute:
            return 'too many in a minute (at most %d)' % self.per_minute
        return ''

    def _take(self, ident, expires, now):
        self.seen[ident] = max(expires, now) + NETWORK_CLOCK_SLACK + NETWORK_LIFE_MAX
        self.times.append(now)

    def broadcast(self, m, now=None):
        """(why it is refused, or ''; the fields {"id", "scope", "kind", "seconds", "text"})."""
        now = time.time() if now is None else now
        why = self._signature(m, 'bf1943-broadcast|1|')
        if why:
            return why, None
        parts = m['signed'].split('|', 8)
        if len(parts) != 9:
            return 'a message with missing parts', None
        _, _, ident, issued, expires, scope, kind, seconds, text_ = parts
        try:
            issued, expires, seconds = int(issued), int(expires), int(seconds)
        except ValueError:
            return 'a bad number', None
        if scope not in NETWORK_SCOPES:
            return 'an unknown scope', None
        if kind not in BROADCAST_KINDS:
            return 'an unknown kind', None
        if not text_ or len(text_) > NETWORK_TEXT_MAX or text_ != clean_broadcast_text(text_):
            return 'a text that is empty, too long or not plain', None
        if not 3 <= seconds <= 30:
            return 'a bad time on screen', None
        why = self._fresh(ident, issued, expires, now)
        if why:
            return why, None
        self._take(ident, expires, now)
        return '', {'id': ident, 'scope': scope, 'kind': kind, 'seconds': seconds, 'text': text_}

    def admin(self, m, now=None):
        """(why it is refused, or ''; what is asked)."""
        now = time.time() if now is None else now
        why = self._signature(m, 'bf1943-admin|1|')
        if why:
            return why, None
        parts = m['signed'].split('|', 4)
        if len(parts) != 5:
            return 'a message with missing parts', None
        try:
            issued = int(parts[3])
        except ValueError:
            return 'a bad number', None
        why = self._fresh(parts[2], issued, issued + NETWORK_LIFE, now)
        if why:
            return why, None
        self._take(parts[2], issued + NETWORK_LIFE, now)
        return '', parts[4]


def i8(v):
    return bytes([(v + 0x80) & 0xFF])


def i16(v):
    return struct.pack('>H', (v + 0x8000) & 0xFFFF)


def i32(v):
    return struct.pack('>I', (v + 0x80000000) & 0xFFFFFFFF)


def text(s):
    # A soldier name's trademark sign (since v0.4.7.4) as UTF-8, the bytes the player's own game holds; every other
    # character Latin-1 has, as before.
    try:
        b = s.encode('latin-1')
    except UnicodeEncodeError:
        b = b''.join(c.encode('latin-1') if ord(c) < 256 else c.encode('utf-8') for c in s)
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
# id (2 bytes), kind (BROADCAST_KINDS), seconds on screen, the text. A game older than v0.4.0 ignores the kind; one
# older than v0.4.7 shows maintenance (3) as a message.
C_BROADCAST = 11
BROADCAST_KINDS = {'custom': 0, 'update': 1, 'restart': 2, 'maintenance': 3}
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
        # Team balance (bf/src/team_balance.cpp): the dedicated server writes its moves, refusals and warnings to
        # --balance-file, one JSON object a line; each new line becomes a server chat line, and a message box for the
        # player it is about.
        self.balance_file = ''
        self.balance_pos = None     # bytes of the file read (None: not looked at yet, so an old file is not told)
        self.balance_seq = 0
        self.balance_looked = 0.0
        self.server_msg = 0         # the number of the host's own chat lines (each client keeps the first of each)

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

    def server_chat(self, text_, team=0):
        """A chat line from the server itself to every player (team 0) or one team's (1, 2): every game since the
        chat's first version shows it, as from "Server"."""
        self.server_msg = self.server_msg % 0xFFFF + 1
        name = b'Server'
        raw = text_.encode('utf-8')[:CHAT_MAX]
        relay = struct.pack('>HHBBB', 0, self.server_msg, CHAT_TEAM if team else CHAT_ALL, team, len(name)) + name + \
            bytes([len(raw)]) + raw
        sent = 0
        for c in self.clients.values():
            if team and c.team != team:
                continue
            sent += 1
            for _ in range(2):
                self.send(C_CHAT_RELAY, relay, c.addr)
        self.host.note('comms: server chat to %s (%d players): %s' % ('team %d' % team if team else 'all', sent, text_))

    def tell(self, name, text_, seconds=8):
        """A message box for one player (the server broadcast's box and chime, shown by every game since v0.4.0)."""
        self.broadcast_id = (self.broadcast_id % 0xFFFF) + 1
        body = struct.pack('>HBB', self.broadcast_id, 0, max(3, min(30, int(seconds)))) + short_text(text_)
        told = 0
        for c in self.clients.values():
            if c.name == name:
                told += 1
                for _ in range(3):
                    self.send(C_BROADCAST, body, c.addr)
        self.host.note('comms: told %s%s: %s' % (name, '' if told else ' (not linked: chat line only)', text_))

    def read_balance_file(self, now):
        if not self.balance_file or now - self.balance_looked < 0.5:
            return
        self.balance_looked = now
        try:
            size = os.path.getsize(self.balance_file)
        except OSError:
            if self.balance_pos is None:
                self.balance_pos = 0  # no file yet: everything written from now on is new
            return
        if self.balance_pos is None:
            self.balance_pos = size  # as the host starts: what is there was for an earlier run
            return
        if size < self.balance_pos:
            self.balance_pos, self.balance_seq = 0, 0  # the server started again and began a new file
        if size == self.balance_pos:
            return
        with open(self.balance_file, 'rb') as f:
            f.seek(self.balance_pos)
            data = f.read(size - self.balance_pos)
        end = data.rfind(b'\n') + 1  # whole lines only; the rest next time
        self.balance_pos += end
        for line in data[:end].splitlines():
            try:
                m = json.loads(line)
                seq = int(m.get('seq', 0))
            except (ValueError, TypeError, AttributeError):
                continue
            if seq and seq <= self.balance_seq:
                continue
            self.balance_seq = seq or self.balance_seq
            self.balance_event(m)

    def balance_event(self, m):
        teams = {1: 'US', 2: 'Japan'}
        kind, name = str(m.get('kind', '')), str(m.get('name', ''))[:64]
        team, to = int(m.get('team') or 0), int(m.get('to') or 0)
        if kind == 'moved' and to in teams:
            self.server_chat('%s was moved to %s to even the teams' % (name, teams[to]))
            self.tell(name, 'You were moved to %s to even the teams. No points lost.' % teams[to])
        elif kind == 'refused' and to in teams:
            self.tell(name, '%s has more players. Change Team was refused to keep the teams even.' % teams[to])
        elif kind == 'warn' and team in teams and to in teams:
            self.server_chat('Teams are uneven: the next %s player to go down may be moved to %s' % (teams[team], teams[to]),
                             team)

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
            self.read_balance_file(now)
        except Exception as e:  # never the host's own loop for a message
            if not getattr(self, 'balance_error', None):
                self.balance_error = True
                self.host.note('comms: the balance file could not be read (%s: %s)' % (type(e).__name__, e))
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
        self.first = None  # the first bytes of its first packet (said when it is dropped without ever linking)

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
        transport = self.host.transport
        if transport is None or transport.is_closing():
            return  # a host being replaced (GameHost.close): its server sockets may still deliver a packet
        if not self.client_bound:
            subs = [self.control()] + subs
        transport.sendto(self.datagram(subs), self.addr)

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
        if self.first is None:
            self.first = data[:16]
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
        self.up, self.down = Held(dev_delay(peer.addr[0])), Held(dev_delay(peer.addr[0]))  # BF_DEV_NET_DELAY (tests)

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


class World:
    """The match's world: its clock and weather, asked for by every player's game (bf/src/world_sync.cpp), which then
    draws the same hour and the same sky; an admin's game may change them while the server runs. 14 Nov 2026: "cloud
    coverage, rate, weather percentage, day/night adjustment and time scale settings ... for servers ... giving
    player IDs admin panel access in-game". The same as gm_host.cpp's World (a match a player's game hosts).

    Asked with BF43WRLD: version 1, flags (1: a change follows), the player's name and a key (a length byte and text
    each); for a change: day and night (a byte) and eight floats: hour, a day's minutes, cloud cover, cloud rate,
    weather, distance fog, ground fog, wind. Answered with BF43WRLR: version, flags (1 day and night, 2 the asker is
    an admin), the same eight floats
    as they are now, and the clouds' two clocks as doubles (drift seconds, forming). Big endian. Last in both
    (15 Nov 2026; a game from before it sends and reads none): the cloud base's height, metres, a float."""

    def __init__(self, host):
        self.host = host
        self.day_night, self.hour, self.day_minutes = False, 16.0, 0.0
        self.cover, self.rate, self.weather = 0.35, 0.33, 0.0
        self.fog = self.ground_fog = 0.0
        self.wind = 0.3
        self.cloud_height = 450.0
        self.seconds = self.evolve = 0.0
        self.at = time.monotonic()
        self.admins, self.key = set(), ''
        self.night_ops = False
        self.round, self.progress, self.stage = None, 0.0, -1  # Night Ops: the round's build (match)
        self.ramp_from = time.monotonic()
        self.pings = {}  # a joined player's name -> (their ping in ms, when they said it): the scoreboards' ping

    # Night Ops (16 Nov 2026, TekRantGaming: the night maps gone, "we would just start the match to always be at a set
    # time the full time so could have night ops maps"): a Night Ops server's world is the day and night cycle held at
    # this hour for the whole match. 20 Nov 2026, his presets: "time of day should be 1900h and is locked at that time";
    # the weather follows the tickets.
    NIGHT_OPS_HOUR = 19.0
    # The storm's build over a round (TekRantGaming, 20 Nov 2026: "a nice clear day at the start ... we still want to see
    # the stars, and then the cloud coverage slowly builds up ... the weather gets stronger up until the point at the end
    # of the game. Now it's all based on tickets ... I do want a good chunk of the first half of the game to be rain
    # free"; 100% with 80% of the match gone). The progress is the share of its tickets the team with fewer left has
    # lost (match.txt, match_rules.cpp). Each row: progress, cloud cover, weather, fog (distance and ground); between two
    # rows the values go in a straight line, and players' games ease each change over about 25 s. Since pull request 62
    # the weather is a hurricane that forms at sea from 25%, rains where it is past 75% and covers the whole map at
    # 100% (85% left the carrier dry, run night048g). The progress never goes back within a round; a new round starts
    # clear. On a Night Ops server these four are the build's: an admin's F9 keeps the rest (cloud rate, wind, height).
    NIGHT_OPS_STAGES = (
        (0.00, 0.15, 0.00, 0.00),  # a clear night: stars and the moon
        (0.40, 0.15, 0.00, 0.00),  # still clear at 40%
        (0.55, 0.60, 0.30, 0.00),  # the clouds gather, the storm forms out at sea: far lightning, no rain on the map
        (0.80, 0.90, 1.00, 0.50),  # it rolls in: the rain reaches the map about two thirds in, the fog rises with it
        (1.00, 0.90, 1.00, 0.50),  # the full storm to the end
    )
    NIGHT_OPS_STAGE_NAMES = ('a clear night', 'the clouds gather', 'the storm rolls in', 'the full storm')

    @classmethod
    def night_ops_values(cls, progress):
        """Cloud cover, weather and fog at a round's progress (0 to 1), and the stage's index (NIGHT_OPS_STAGE_NAMES:
        the clear night up to the second row, then one stage for each row after it, the last row's from its start)."""
        rows = cls.NIGHT_OPS_STAGES
        p = max(0.0, min(1.0, progress))
        i = 0
        while i < len(rows) - 2 and p > rows[i + 1][0]:
            i += 1
        a, b = rows[i], rows[i + 1]
        t = 0.0 if b[0] <= a[0] else max(0.0, min(1.0, (p - a[0]) / (b[0] - a[0])))
        cover, weather, fog = (a[k] + (b[k] - a[k]) * t for k in (1, 2, 3))
        stage = 0 if p <= rows[1][0] else min(len(cls.NIGHT_OPS_STAGE_NAMES) - 1, i if p < rows[-2][0] else i + 1)
        return cover, weather, fog, stage

    def configure(self, args):
        clamp = lambda v, lo, hi: max(lo, min(hi, float(v)))
        self.night_ops = bool(getattr(args, 'night', False))
        self.day_night = bool(args.day_night)
        self.hour = clamp(args.time, 0, 24) % 24
        self.day_minutes = clamp(args.day_minutes, 0, 1440)
        if self.night_ops:
            self.day_night, self.hour, self.day_minutes = True, self.NIGHT_OPS_HOUR, 0.0
        self.cover, self.rate = clamp(args.cloud_cover, 0, 1), clamp(args.cloud_rate, 0, 1)
        self.weather = clamp(args.weather, 0, 1)
        self.fog, self.ground_fog = clamp(args.fog, 0, 1), clamp(args.ground_fog, 0, 1)
        self.wind = clamp(args.wind, 0, 1)
        self.cloud_height = clamp(args.cloud_height, 100, 1500)
        self.round, self.progress, self.stage = None, 0.0, -1
        self.match_file, self.match_read = getattr(args, 'match_file', '') or '', 0.0
        if self.night_ops:
            self.set_night_ops(0.0)
        # Player IDs as the game shows them (eight hex digits, the low half of the XUID).
        self.admins = {a.strip().upper().rjust(8, '0') for a in args.admins.replace(';', ',').replace(' ', ',').split(',')
                       if a.strip()}
        self.key = args.admin_key
        self.host.note('world: %s' % self.said())

    def said(self):
        return ('day and night %s, %.1f h, a day in %.0f min, clouds %.0f%% at rate %.0f%%, weather %.0f%%, fog %.0f%%, '
                'ground fog %.0f%%, wind %.0f%%, cloud height %.0f m') % (
            'on' if self.day_night else 'off', self.hour, self.day_minutes, self.cover * 100, self.rate * 100,
            self.weather * 100, self.fog * 100, self.ground_fog * 100, self.wind * 100, self.cloud_height)

    def set_night_ops(self, progress):
        self.progress = progress
        self.cover, self.weather, fog, stage = self.night_ops_values(progress)
        self.fog = self.ground_fog = fog
        if stage != self.stage:
            self.stage = stage
            self.host.note('world: Night Ops at %.0f%% of the round: %s, %s' % (
                progress * 100, self.NIGHT_OPS_STAGE_NAMES[stage], self.said()))

    def match(self, round_number, left):
        """Night Ops: the round's progress from the server (round number, the share of tickets the team with fewer still
        has): the clouds, weather and fog of NIGHT_OPS_STAGES. A new round starts clear."""
        if not self.night_ops:
            return
        progress = max(0.0, min(1.0, 1.0 - left))
        test = os.environ.get('BF_DEV_NIGHT_RAMP', '')  # tests: the whole build in this many seconds, not the tickets
        if test:
            try:
                progress = min(1.0, (time.monotonic() - self.ramp_from) / max(1.0, float(test)))
            except ValueError:
                test = ''
        if round_number != self.round:
            if self.round is not None:
                self.host.note('world: Night Ops round %s starts clear' % round_number)
            self.round, self.stage = round_number, -1
            self.ramp_from = time.monotonic()
            self.set_night_ops(0.0 if not test else progress)
            return
        if progress > self.progress + 1e-6:
            self.set_night_ops(progress)

    def read_match(self, every=0.5):
        """Night Ops: the round and its tickets from the server's match.txt (match_rules.cpp writes it each second it
        changes), read at most every `every` seconds: on players' asks too, not only the 5 s heartbeat, so a new round's
        clear night reaches their games within a second of its load (TekRantGaming, 20 Nov 2026: the last round's storm
        was seen and heard at the next one's start)."""
        now = time.monotonic()
        if not self.night_ops or not self.match_file or now - self.match_read < every:
            return
        self.match_read = now
        try:
            with open(self.match_file, encoding='utf-8') as f:
                parts = f.read().split()
            if len(parts) >= 2:
                self.match(int(parts[0]), float(parts[1]))
        except (OSError, ValueError):
            pass

    def advance(self):
        now = time.monotonic()
        dt, self.at = now - self.at, now
        if self.day_night and self.day_minutes > 0:
            self.hour = (self.hour + dt / (self.day_minutes * 60) * 24) % 24
        self.seconds += dt
        self.evolve += dt * self.rate

    def admin(self, name, key, addr):
        if self.key and key == self.key:
            return True
        return any(p.state == 'joined' and p.player and p.player.name == name and p.addr[0] == addr[0] and
                   '%08X' % (p.player.xuid & 0xFFFFFFFF) in self.admins for p in self.host.peers.values())

    def ping_list(self):
        """BF43PNGL: version 1, a count, then each joined player's ping (u16 ms) and name (a length byte and text),
        for the players' scoreboards (bf/src/world_sync.cpp). Players heard from in the last 10 s, at most 64, and the
        datagram kept under 1200 bytes."""
        now = time.monotonic()
        for name in [n for n, (_, at) in self.pings.items() if now - at > 10]:
            del self.pings[name]
        body, count = b'', 0
        for name, (ms, _) in list(self.pings.items())[:64]:
            raw = name.encode('utf-8')[:64]
            entry = struct.pack('>HB', min(ms, 0xFFFE), len(raw)) + raw
            if len(body) + len(entry) > 1180:
                break
            body += entry
            count += 1
        return b'BF43PNGL' + bytes([1, count]) + body

    def receive(self, data, addr):
        try:
            version, flags = data[0], data[1]
            n = data[2]
            name = data[3:3 + n].decode('utf-8', 'replace')
            at = 3 + n
            k = data[at]
            key = data[at + 1:at + 1 + k].decode('utf-8', 'replace')
            at += 1 + k
            if version != 1 or at > len(data):
                return
            self.advance()
            self.read_match()  # Night Ops: a new round's world at once
            admin = self.admin(name, key, addr)
            if flags & 1 and admin and len(data) >= at + 33:
                day_night = data[at] != 0
                values = [v if math.isfinite(v) else 0.0 for v in struct.unpack_from('>8f', data, at + 1)]
                before = self.said()
                if not self.night_ops:  # a Night Ops server's clock stays at its night (configure)
                    self.day_night = day_night
                    self.hour = max(0.0, values[0]) % 24
                    self.day_minutes = max(0.0, min(1440.0, values[1]))
                (cover, self.rate, weather, fog, ground_fog,
                 self.wind) = (max(0.0, min(1.0, v)) for v in values[2:8])
                if not self.night_ops:  # a Night Ops server's clouds, weather and fogs follow its tickets (match)
                    self.cover, self.weather, self.fog, self.ground_fog = cover, weather, fog, ground_fog
                if len(data) >= at + 37:
                    height = struct.unpack_from('>f', data, at + 33)[0]
                    if math.isfinite(height):
                        self.cloud_height = max(100.0, min(1500.0, height))
                if self.said() != before:
                    self.host.note('world: %s set %s' % (name or 'an admin', self.said()))
            # The asker's ping (flag 4, an ask without a change; a game from before 20 Nov 2026 sends none): kept for
            # a joined player, by the name it plays under.
            if flags & 4 and not flags & 1 and len(data) >= at + 2:
                ms = struct.unpack_from('>H', data, at)[0]
                if self.host.comms.joined_player(name, addr):
                    if ms != 0xFFFF:
                        if name not in self.pings:
                            self.host.note('ping: %s at %d ms' % (name, ms))
                        self.pings[name] = (ms, time.monotonic())
                    else:
                        self.pings.pop(name, None)
            reply = b'BF43WRLR' + bytes([1, (1 if self.day_night else 0) | (2 if admin else 0)]) + struct.pack(
                '>8f2d', self.hour, self.day_minutes, self.cover, self.rate, self.weather, self.fog, self.ground_fog,
                self.wind, self.seconds, self.evolve) + struct.pack('>f', self.cloud_height)
            self.host.transport.sendto(reply, addr)
            if flags & 4:
                self.host.transport.sendto(self.ping_list(), addr)
        except (IndexError, struct.error):
            pass


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
        self.world = World(self)
        self.transport = None
        self.tick_task = None
        self.error_noted = 0  # monotonic time of the last socket error written to the log
        self.note_window, self.notes_in_window, self.notes_dropped = 0.0, 0, 0
        self.bad_packets, self.refused_peers, self.quiet_noted = 0, 0, 0.0

    def close(self):
        """This host is replaced by another (a registration that gave other ids): its players' links, their sockets to
        the dedicated server, its tick and its socket end with it."""
        for addr in list(self.peers):
            self.drop(addr)
        if self.tick_task:
            self.tick_task.cancel()
        if self.transport:
            self.transport.close()

    def note(self, text_):
        # A budget of lines each 10 s; what was left out is counted.
        now = time.monotonic()
        if now - self.note_window >= 10:
            dropped = self.notes_dropped
            self.note_window, self.notes_in_window, self.notes_dropped = now, 0, 0
            if dropped:
                self.note('%d log lines left out in the last 10 s (too many)' % dropped)
        self.notes_in_window += 1
        if self.notes_in_window > NOTE_BUDGET:
            self.notes_dropped += 1
            return
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
        for ip, (delay, _) in DEV_DELAY_FOR.items():
            self.note('test delay: %s\'s game datagrams held %.0f ms each way' % (ip, delay * 1000))
        self.tick_task = asyncio.get_running_loop().create_task(self.tick())

    def datagram_received(self, data, addr):
        # A packet that cannot be read is passed over and counted.
        try:
            self.packet(data, addr)
        except Exception as e:
            self.bad_packets += 1
            self.quiet_note('packets that could not be read passed over (the last from %s:%d: %s)' % (
                addr[0], addr[1], type(e).__name__))

    def quiet_note(self, what):
        """At most one line each 10 s about passed over packets and refused clients."""
        now = time.monotonic()
        if now - self.quiet_noted >= 10:
            self.quiet_noted = now
            self.note('%d %s; %d clients that had not joined let go for new ones (%d at most)' % (
                self.bad_packets, what, self.refused_peers, MAX_LINKING))
            self.bad_packets = self.refused_peers = 0

    def packet(self, data, addr):
        # The server browser's ping and the backend's reachability probe (SERVERS-PLAN.md sections 5 and 6): answered
        # at once, never a game packet (those start with a flag byte, 0 or 1).
        if data[:8] in (PING, PROBE):
            delay, jitter = dev_delay(addr[0])
            if delay or jitter:
                # Tests (BF_DEV_NET_DELAY): the answer waits as a game datagram's round trip does (held each way), so
                # the browser's ping and the players' pings show the far server the test pretends to be.
                asyncio.get_running_loop().call_later(2 * delay + random.random() * jitter,
                                                      self.transport.sendto, PONG + data[8:40], addr)
            else:
                self.transport.sendto(PONG + data[8:40], addr)
            return
        # The match's clock and weather (World): asked for from a socket of the game's own.
        if data[:8] == b'BF43WRLD' and len(data) > 12:
            self.world.receive(data[8:], addr)
            return
        # Voice and text chat: from a client's comms socket, never the game's tunnel address.
        if data[:8] == COMMS and addr not in self.peers and len(data) > 8:
            self.comms.receive(data[8:], addr)
            return
        peer = self.peers.get(addr)
        if not peer:
            waiting = [(p.last_heard, a) for a, p in self.peers.items() if p.state != 'joined']
            if len(waiting) >= MAX_LINKING:
                # The one heard from longest ago gives way (a real player's new link is never kept out for long).
                self.drop(min(waiting)[1])
                self.refused_peers += 1
                self.quiet_note('packets passed over')
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
                    # 17 Nov 2026: on US Central most of these were addresses that sent one packet and never linked
                    # (336 of 414), so what they sent is said.
                    peer.note('silent for 30 s, dropped' + ('' if peer.connected else
                                                            ' (it never linked; its first packet %s)' % (peer.first or b'').hex()))
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
    # The build check's report, once (hashing the package takes about a second); the host key when the manager gave one.
    try:
        build = build_report()
        print('%s build %s (%s, %d files): %s' % (time.strftime('%H:%M:%S'), build['version'] or '?', build['platform'],
                                                  build['files'], build['digest']), flush=True)
    except OSError as e:
        build = None
        print('%s build check: the package could not be read (%s)' % (time.strftime('%H:%M:%S'), e), flush=True)
    official_key = read_key(args.official_key_file) if args.official_key_file else None
    network = None  # NetworkCheck, made at the first registration
    host = None  # the GameHost, kept through a lost backend link (its players stay)
    host_seed = None
    if args.host_key_file:
        try:
            host_seed = host_key_seed(args.host_key_file)
        except (OSError, ValueError) as e:
            print('%s host key: %s' % (time.strftime('%H:%M:%S'), e), flush=True)
    while True:
        try:
            reader, writer = await asyncio.open_connection(*backend)
        except OSError as e:
            print('%s backend %s:%d not reachable (%s), retrying' % (time.strftime('%H:%M:%S'), *backend, e),
                  flush=True)
            await asyncio.sleep(5)
            continue
        # A link that stops answering (no acknowledgement for 30 s) ends with an error instead of after the system's
        # own retransmission time (about 15 minutes on Linux), so the host registers again while its players play on.
        link = writer.get_extra_info('socket')
        if link is not None and hasattr(socket, 'TCP_USER_TIMEOUT'):
            try:
                link.setsockopt(socket.IPPROTO_TCP, socket.TCP_USER_TIMEOUT, 30000)
            except OSError:
                pass
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
        if build is not None:
            reg['build'] = build
            if host_seed:
                try:
                    reg['host'] = host_proof(host_seed, args.name, build['digest'])
                except Exception as e:  # never stop a server over it: the backend then lists it not
                    print('%s host key: no signature (%r)' % (time.strftime('%H:%M:%S'), e), flush=True)
        # A host whose link to the backend dropped (the backend restarted, or the link timed out) keeps its players
        # (17 Nov 2026: each restart of the main backend dropped every player of US Central, which registers with it
        # from across the ocean, and on 16 Nov 08:59 a link that timed out ended its host and the whole server with
        # it). It asks to keep its user id (its tunnel's id) and player id, and names its players' ids so a backend
        # that restarted gives them to nobody else (plasma.py Registry.resume). A backend that keeps them leaves the
        # players' links as they are; one that cannot (or one from before this) gives other ids, and the host is
        # replaced as before.
        if host is not None:
            reg['resume'] = {'uid': host.user_id, 'pid': host.pid,
                             'pids': sorted({p.player.pid for p in host.peers.values() if p.player})[:64]}
        # Listening before the backend is told: its reachability probe comes at once (a kept host listens already).
        sock = None
        try:
            if host is None:
                sock = udp_socket(listen)
            send(reg)
            await writer.drain()
            reply = json.loads(await reader.readline())
        except (OSError, ValueError) as e:  # the backend went away again or answered nothing, or the port is taken
            print('%s registration with the backend %s:%d failed (%s: %s), retrying' % (
                time.strftime('%H:%M:%S'), *backend, type(e).__name__, e), flush=True)
            if sock:
                sock.close()
            writer.close()
            await asyncio.sleep(5)
            continue
        if reply.get('op') == 'refused':
            print('%s backend refused the server: %s' % (time.strftime('%H:%M:%S'), reply.get('why')), flush=True)
            write_status(args, {'state': 'refused', 'why': reply.get('why', '')})
            if sock:
                sock.close()
            writer.close()
            await asyncio.sleep(30)
            continue
        if host is not None and reply.get('uid') == host.user_id and reply.get('pid') == host.pid:
            host.note('the backend kept this server\'s ids: its %d players stay' % host.player_count())
        else:
            if host is not None:
                host.note('the backend gave this server other ids: its %d players are dropped' % host.player_count())
                host.close()
                for _ in range(60):  # the old socket closes on one of the loop's next turns
                    await asyncio.sleep(0.05)
                    try:
                        sock = udp_socket(listen)
                        break
                    except OSError:
                        sock = None
                if sock is None:
                    print('%s the server\'s port is still taken, registering again' % time.strftime('%H:%M:%S'),
                          flush=True)
                    host = None
                    writer.close()
                    await asyncio.sleep(5)
                    continue
            host = await start(listen, parse_address(args.game_server), reply['uid'], reply['pid'], args.name, log,
                               public, sock)
            host.comms.broadcast_file = args.broadcast_file
            if network is None:  # once: the ids it has seen stay through a new registration
                network = NetworkCheck(trusted_broadcast_keys(official_key))
                if not network.keys:
                    host.note('network broadcasts: no official broadcast key known, none will be shown')
            host.comms.balance_file = args.balance_file
            host.world.configure(args)
        host.note('registered with the backend as game %s%s%s' % (
            reply['gid'], ' (official)' if reply.get('official') else '', '' if reply.get('listed', True) else
            ' (not listed: Direct connect only)'))
        if reply.get('hidden'):
            host.note('not listed by the backend: %s. Only an official server package is listed; update the server '
                      'package (its manager updates itself) or download it again.' % reply['hidden'])
        state = {'state': 'registered', 'gid': reply['gid'], 'official': bool(reply.get('official')),
                 'listed': reply.get('listed', True), 'address': reply.get('address', ''), 'reachable': None,
                 'hidden': str(reply.get('hidden', ''))}
        write_status(args, state)

        reported = [args.map]

        async def heartbeat():
            try:
                await beat_loop()
            except Exception as e:  # a dead heartbeat left the list's players and map stale
                host.note('heartbeat failed: %r' % e)

        stats_at = [None]  # how far into --stats-file has been sent (None: not looked yet)

        def stats_lines():
            """Each player's stats from the game (server_stats.cpp, --bf_stats_file): the lines written since the last
            look, each a "player_stats" message for the backend. What is there as the host starts is from an earlier
            run; a file that shrank started again."""
            if not args.stats_file:
                return []
            try:
                size = os.path.getsize(args.stats_file)
            except OSError:
                if stats_at[0] is None:
                    stats_at[0] = 0
                return []
            if stats_at[0] is None:
                stats_at[0] = size
                return []
            if size < stats_at[0]:
                stats_at[0] = 0
            if size == stats_at[0]:
                return []
            with open(args.stats_file, 'rb') as f:
                f.seek(stats_at[0])
                data = f.read(min(size - stats_at[0], 1 << 20))
            end = data.rfind(b'\n') + 1
            stats_at[0] += end
            out = []
            for line in data[:end].splitlines():
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                if isinstance(m, dict) and m.get('op') == 'player_stats' and isinstance(m.get('rows'), list):
                    out.append({'op': 'player_stats', 'rows': m['rows'][:64]})
            return out

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
                for m in stats_lines():  # each player's stats since the last beat (player_stats.py)
                    send(m)
                host.world.read_match(every=0)  # the round's progress (match_rules.cpp): Night Ops' storm builds with it
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
                    try:
                        host.expect(m['ticket'], m['pid'], m['name'], m['xuid'], m['xnaddr'])
                    except (KeyError, TypeError, ValueError) as e:  # never the host's end: the message is passed over
                        host.note('backend: a join notice that cannot be read (%s)' % type(e).__name__)
                        continue
                    host.note('backend: %s joins (PID %s)' % (m['name'], m['pid']))
                elif m.get('op') == 'network_broadcast':
                    # A broadcast from the official machine, through the main backend (sign_network): checked here
                    # again, whoever passed it on.
                    why, b = network.broadcast(m)
                    if not why and b['scope'] == 'official' and not state['official']:
                        why = 'meant for the official servers only'
                    if why:
                        host.note('network broadcast refused: %s' % why)
                    else:
                        host.note('network broadcast %s (to %s)' % (b['id'], b['scope']))
                        host.comms.broadcast(b['text'], b['kind'], b['seconds'])
        except (OSError, ValueError) as e:  # OSError: ConnectionError and also TimeoutError (errno 110, 16 Nov)
            host.note('backend link lost (%s)' % type(e).__name__)
        else:
            host.note('backend link lost (closed by the backend)')
        beat.cancel()
        writer.close()
        await asyncio.sleep(2)  # the host keeps its players; the next registration asks to keep its ids


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
    # Night Ops (TekRantGaming, 11 Nov 2026): the maps at night, shown in the server list. Since 16 Nov 2026 the night
    # is the server's world: the day and night cycle held at one hour for the whole match (World.NIGHT_OPS_HOUR, 19:00
    # since 20 Nov 2026), drawn by players' games with the levels' lamps lit (NativeXRenderer D175); --day-night, --time
    # and --day-minutes are then not used. Its storm builds with each round's tickets (World.match, --match-file).
    ap.add_argument('--night', action='store_true',
                    help='a night server (Night Ops): its world held at 19:00, the storm building with the tickets')
    # Where the server is (TekRantGaming, 13 Nov 2026: the browser shows the region with its flag): one of the game's
    # region codes (bf/src/regions.h, the manager's drop down). And its tick rate, for the browser: the game's own
    # 30 Hz today.
    ap.add_argument('--broadcast-file', default='',
                    help='a JSON file the server manager writes to send a message to every player on the server: '
                         '{"seq": a new number each time, "kind": "custom", "update" or "restart", "text", "seconds"}')
    ap.add_argument('--stats-file', default='',
                    help="each player's stats from the game (its --bf_stats_file): every new line goes to the backend")
    ap.add_argument('--balance-file', default='',
                    help="the dedicated server's team balance events (its --bf_balance_events): players are told of "
                         'moves and refused team changes')
    # The server's world (World): its clock and weather, the same for every player; admins change it in game (F9).
    ap.add_argument('--day-night', action='store_true', help='day and night: the light follows the server\'s clock')
    ap.add_argument('--time', type=float, default=16.0, help='the time of day the server starts at, 0 to 24')
    ap.add_argument('--day-minutes', type=float, default=0.0,
                    help='minutes a whole day and night take; 0: the time stands still')
    ap.add_argument('--cloud-cover', type=float, default=0.35, help='the cloud cover, 0 to 1')
    ap.add_argument('--cloud-rate', type=float, default=0.33, help='how fast the clouds form and change, 0 to 1')
    ap.add_argument('--weather', type=float, default=0.0,
                    help='the weather, 0 fair to 1 a storm (overcast, mist, far rain shafts, lightning)')
    ap.add_argument('--fog', type=float, default=0.0, help='the distance fog, 0 to 1')
    ap.add_argument('--ground-fog', type=float, default=0.0, help='the ground fog, 0 to 1')
    ap.add_argument('--wind', type=float, default=0.3, help='the wind, 0 calm to 1 a gale: how far the rain leans')
    ap.add_argument('--cloud-height', type=float, default=450.0,
                    help='the cloud base over the sea, metres (100 to 1500): low, the planes fly through the clouds')
    ap.add_argument('--admins', default='',
                    help="player IDs (as the game shows them, separated by commas) who may change the world in game")
    ap.add_argument('--admin-key', default='', help='a key that makes the game showing it an admin; empty: none')
    ap.add_argument('--region', default='', help="the server's region code, e.g. gb, de, us-east (shown in the browser)")
    ap.add_argument('--tick', type=int, default=60, help="the server's tick rate in Hz (shown in the browser; 60 by default since 14 Nov 2026)")
    ap.add_argument('--map-file', default='', help="the server's current_map.txt (its map rotation)")
    ap.add_argument('--match-file', default='',
                    help="the server's match.txt (match_rules.cpp): the round and its tickets left, for Night Ops' storm")
    ap.add_argument('--maps', default='', help='the rotation, map titles separated by commas (for the next map)')
    ap.add_argument('--description', default='', help='shown in the server browser (200 characters)')
    ap.add_argument('--unlisted', action='store_true', help='registered but not listed: Direct connect only')
    ap.add_argument('--official-key-file', default='', help='official.key: the server shows as official')
    ap.add_argument('--host-key-file', default='', help="this host's key (the manager makes it in the data folder)")
    ap.add_argument('--status-file', default='', help="this server's state for the package's window (JSON)")
    ap.add_argument('--log', default='')
    asyncio.run(serve(ap.parse_args()))


if __name__ == '__main__':
    main()
