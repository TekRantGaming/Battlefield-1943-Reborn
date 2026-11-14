#!/usr/bin/env python3
"""Battlefield 1943 Reborn server manager (TekRantGaming, 13 Nov 2026: "a whole new UI that's user friendly and has all
the options you would expect to see in a tool like this").

One program for Windows and Linux, the Python standard library only. It keeps the servers of server.json running
(each one a dedicated game process and its host, gmhost.py), and serves the manager's page to a browser on this
machine: the servers and who plays on them, their settings, the region, LAN only mode, messages to the players,
updates, the game's files, the logs.

    manager.py [run] [--no-browser] [--port N]   the manager: the page, and the servers kept running
    manager.py open                              the page in a browser; starts the manager in the background first
                                                 when it is not running (what "BF1943 Server.bat" does)
    manager.py status                            what runs (asks the running manager)
    manager.py start [n] | stop [n] | restart [n]
    manager.py broadcast --kind restart --minutes 5 [--server n] [--no-restart]
    manager.py broadcast --kind custom --text "Teams are balanced after this round"
    manager.py update                            look for a new release now, and install it
    manager.py install <package or folder>       the game's files, from the host's own copy of the game
    manager.py quit [--keep-servers]             stops the manager (and, without the option, its servers)

Where things are: the program (bin, server, python on Windows) in the package's folder; server.json, logs and game in
the same folder when it can be written, else (a Flatpak, an AppImage) in the user's data folder. BF_SERVER_DATA names
another place.

Each server: its own dedicated game process on a loopback address of its own (127.0.0.11, .12 and on: nothing to
open for it), and its host on the server's UDP port (18300 and on), which players reach. In the usual mode the hosts
register with the main backend, and the server is in every player's server browser. LAN only (server.json lan_only):
this machine runs its own backend (plasma.py --lan) and nothing is told to the main one; players join with Direct
connect, by this machine's address on the network, or from outside by its public address once the ports are open.
"""
import argparse
import base64
import contextlib
import hashlib
import hmac
import http.server
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tarfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

WINDOWS = os.name == 'nt'
HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.join(HERE, 'manager_ui')

# The regions a server can say it is in: the same list, in the same order, as the game's (bf/src/regions.h kRegions;
# tools/test_regions.py checks the two against each other). The server browser shows the name and the country's flag.
REGIONS = [
    ('us-east', 'US East', 'us'), ('us-central', 'US Central', 'us'), ('us-west', 'US West', 'us'),
    ('ca', 'Canada', 'ca'), ('mx', 'Mexico', 'mx'), ('br', 'Brazil', 'br'),
    ('ar', 'Argentina', 'ar'), ('cl', 'Chile', 'cl'), ('eu-central', 'EU Central', 'eu'),
    ('gb', 'United Kingdom', 'gb'),
    ('ie', 'Ireland', 'ie'), ('fr', 'France', 'fr'), ('de', 'Germany', 'de'),
    ('nl', 'Netherlands', 'nl'), ('be', 'Belgium', 'be'), ('es', 'Spain', 'es'),
    ('pt', 'Portugal', 'pt'), ('it', 'Italy', 'it'), ('ch', 'Switzerland', 'ch'),
    ('at', 'Austria', 'at'), ('pl', 'Poland', 'pl'), ('cz', 'Czechia', 'cz'),
    ('se', 'Sweden', 'se'), ('no', 'Norway', 'no'), ('fi', 'Finland', 'fi'),
    ('dk', 'Denmark', 'dk'), ('ua', 'Ukraine', 'ua'), ('tr', 'Turkey', 'tr'),
    ('il', 'Israel', 'il'), ('ae', 'UAE', 'ae'), ('za', 'South Africa', 'za'),
    ('in', 'India', 'in'), ('sg', 'Singapore', 'sg'), ('jp', 'Japan', 'jp'),
    ('kr', 'South Korea', 'kr'), ('hk', 'Hong Kong', 'hk'), ('tw', 'Taiwan', 'tw'),
    ('au', 'Australia', 'au'), ('nz', 'New Zealand', 'nz'),
]
MAPS = [('wake', 'Wake Island', 'Levels/Wake_island_s'), ('guadalcanal', 'Guadalcanal', 'Levels/Guadal_Canal'),
        ('iwo', 'Iwo Jima', 'Levels/Iwo_Jima_s'), ('coral', 'Coral Sea', 'Levels/Coral_sea')]
MAP_TITLE = {k: t for k, t, _ in MAPS}
MAP_LEVEL = {k: l for k, _, l in MAPS}
# 60 Hz by default since 14 Nov 2026 (TekRantGaming: "all servers are going to be 60 Hertz", and dedicated servers "30,60
# and above ... recommend 60hz"); 30 the original game's, 75 and 100 experimental.
BASE_PORT, MAX_SERVERS, WEB_PORT, TICK_HZ = 18300, 16, 18280, 60
# The dedicated game's own socket: UDP 1003 on its loopback address; Linux moves ports under 1024 up by 30000
# (bf/src/socket_fixes.cpp HostBindPort).
GAME_PORT = 1003 if WINDOWS else 31003
BACKEND_PORTS = (18241, 18246, 18250)  # a backend of our own (LAN only, official): login, Theater, the registry (TCP)
# Two test aids, for a second set of servers on a machine that runs one already: the first server's loopback address
# is 127.0.0.<base + 1> (BF_SERVER_LOOPBACK_BASE, 10), and our own backend listens on one address only
# (BF_SERVER_BACKEND_BIND; every address without it).
LOOPBACK_BASE = int(os.environ.get('BF_SERVER_LOOPBACK_BASE') or 10)
BACKEND_BIND = os.environ.get('BF_SERVER_BACKEND_BIND') or ''
MAIN_BACKEND = '127.0.0.1'
FEED = 'https://api.github.com/repos/TekRantGaming/Battlefield-1943-Reborn/releases/latest'
# The messages an admin can send from a list (TekRantGaming, 13 Nov 2026), {time} the time left as words.
PRESETS = {
    'update': 'Game has updated to a new version server restarting in {time}',
    'restart': 'This server will restart in {time}',
}
ANNOUNCE_AT = (3600, 1800, 900, 600, 300, 120, 60, 30, 10)  # a countdown is said again with this many seconds left


# ---------------------------------------------------------------------------------------------------------- places

def writable(folder):
    try:
        probe = os.path.join(folder, '.bf-write-test-%d' % os.getpid())
        with open(probe, 'w'):
            pass
        os.remove(probe)
        return True
    except OSError:
        return False


def program_root():
    return os.path.abspath(os.environ.get('BF_SERVER_ROOT') or os.path.dirname(HERE))


def data_root(root):
    if os.environ.get('BF_SERVER_DATA'):
        return os.path.abspath(os.environ['BF_SERVER_DATA'])
    if os.environ.get('FLATPAK_ID'):
        return os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.var/app/%s/data' % os.environ['FLATPAK_ID'])
    if os.environ.get('APPIMAGE'):
        return os.path.join(os.path.dirname(os.environ['APPIMAGE']), 'bf1943-server')
    if writable(root):
        return root
    base = os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share')
    return os.path.join(base, 'bf1943-reborn-server')


ROOT = program_root()
DATA = data_root(ROOT)


def update_target():
    """Where an update's files go: over the program itself (the plain package), or, for a package that is an image
    or cannot be written (an AppImage, whose running copy is only a cache; a Flatpak), into "program" in the data
    folder, which every later start hands over to while it is the newer one (newer_program)."""
    own = os.path.join(DATA, 'program')
    if os.path.abspath(ROOT) == os.path.abspath(own):
        return ROOT
    image = bool(os.environ.get('APPIMAGE') or os.environ.get('FLATPAK_ID'))
    return ROOT if writable(ROOT) and not image else own
LOGS = os.path.join(DATA, 'logs')
CONFIG = os.path.join(DATA, 'server.json')
STATE = os.path.join(LOGS, 'manager.json')  # the running manager: its pid, its port, the token of this machine's tools


def read_text(path, default=''):
    try:
        with open(path, encoding='utf-8-sig') as f:
            return f.read()
    except OSError:
        return default


def write_atomic(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = '%s.tmp%d' % (path, os.getpid())
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.replace(tmp, path)


def package_version(root=None):
    return (read_text(os.path.join(root or ROOT, 'version.txt')).strip().splitlines() or ['dev'])[0].strip() or 'dev'


def package_flags():
    try:
        flags = json.loads(read_text(os.path.join(ROOT, 'package.json'), '{}') or '{}')
    except ValueError:
        flags = {}
    # "main_backend": false (TekRantGaming, 14 Nov 2026: official servers on a second machine, a VPS in the US): the
    # servers are official (the key's code at registering) but the package runs no backend of its own; they register
    # with the main one (server.json "backend", MAIN_BACKEND by default).
    return {'official': bool(flags.get('official')), 'night_ops': bool(flags.get('night_ops')),
            'main_backend': bool(flags.get('official')) and flags.get('main_backend', True) is not False}


def version_key(text):
    """A version as numbers for comparing ("0.4.0" < "0.10.1"); a word in it (a test build's) sorts before the
    release of the same numbers. A hotfix is a fourth number (14 Nov 2026): 0.4.5 < 0.4.5.1 < 0.4.5.10 < 0.4.6;
    missing numbers count as 0 (0.4.5 is 0.4.5.0)."""
    m = re.match(r'v?(\d+(?:\.\d+)*)(.*)$', str(text).strip())
    if not m:
        return ((-1,), 0)
    numbers = [int(x) for x in m.group(1).split('.')]
    while len(numbers) < 4:
        numbers.append(0)
    return (tuple(numbers), 0 if m.group(2) else 1)


def game_exe():
    return os.path.join(ROOT, 'bin', 'bf1943.exe' if WINDOWS else 'bf1943')


def game_installed():
    return os.path.exists(os.path.join(DATA, 'game', 'default.xex'))


# ---------------------------------------------------------------------------------------------------------- settings

def new_server(name, port):
    return {'name': name, 'description': '', 'maps': [k for k, _, _ in MAPS], 'round_end_delay': 45, 'listed': True,
            'night': False, 'port': port, 'max_players': 24, 'autostart': True, 'tick': 0}


def clean_text(value, limit):
    return ' '.join(''.join(c if c.isprintable() else ' ' for c in str(value or '')).split())[:limit]


def clean_config(raw):
    """server.json as this version keeps it (version 3), from any earlier one: every field present and in range.
    Version 1 was one server (name, maps); version 2 a list without ports (they followed the list: 18300 and on)."""
    raw = raw if isinstance(raw, dict) else {}
    out = {'version': 3}
    listed = raw.get('servers')
    if not isinstance(listed, list) or not listed:
        listed = [{'name': raw.get('name') or 'Battlefield 1943 Reborn', 'maps': raw.get('maps'),
                   'round_end_delay': raw.get('round_end_delay')}]
    servers, used = [], set()
    for i, s in enumerate(listed[:MAX_SERVERS]):
        s = s if isinstance(s, dict) else {}
        entry = new_server(clean_text(s.get('name'), 48) or 'Battlefield 1943 Reborn', BASE_PORT + i)
        entry['description'] = clean_text(s.get('description'), 200)
        maps = [m for m in (s.get('maps') or []) if m in MAP_TITLE]
        entry['maps'] = list(dict.fromkeys(maps)) or entry['maps']
        with contextlib.suppress(TypeError, ValueError):
            entry['round_end_delay'] = max(15, min(300, int(s.get('round_end_delay') or 45)))
        with contextlib.suppress(TypeError, ValueError):
            entry['max_players'] = max(2, min(24, int(s.get('max_players') or 24)))
        entry['listed'] = s.get('listed') is not False
        entry['night'] = s.get('night') is True
        entry['autostart'] = s.get('autostart') is not False
        # A rate of its own (13 Nov 2026, TekRantGaming: one 60 Hz test server beside the official 30 Hz ones); 0 takes
        # the manager's rate (Network's Tick rate).
        with contextlib.suppress(TypeError, ValueError):
            own = int(s.get('tick') or 0)
            entry['tick'] = own if own in (30, 60, 75, 100) else 0
        port = BASE_PORT + i
        with contextlib.suppress(TypeError, ValueError):
            port = int(s.get('port') or port)
        if not 1024 <= port <= 65535 or port in used or port in BACKEND_PORTS:
            port = next(p for p in range(BASE_PORT, BASE_PORT + 200) if p not in used)
        used.add(port)
        entry['port'] = port
        servers.append(entry)
    out['servers'] = servers
    region = str(raw.get('region') or '').lower()
    out['region'] = region if region in {c for c, _, _ in REGIONS} else ''
    # The servers' simulation rate (13 Nov 2026, the game's v0.4.1): 30, the original, or 60. Every player in a match
    # must run the server's rate, and a game only lists and joins servers of its own (bf_tick_rate in the game).
    tick = TICK_HZ
    with contextlib.suppress(TypeError, ValueError):
        tick = int(raw.get('tick') or TICK_HZ)
    out['tick'] = tick if tick in (30, 60, 75, 100) else TICK_HZ  # 75 and 100: experimental
    out['lan_only'] = raw.get('lan_only') is True
    out['public_address'] = clean_text(raw.get('public_address'), 64)
    out['upnp'] = raw.get('upnp') is not False
    out['backend'] = clean_text(raw.get('backend'), 64) or MAIN_BACKEND
    out['auto_update'] = raw.get('auto_update') is not False
    warn = 10
    with contextlib.suppress(TypeError, ValueError):
        warn = max(0, min(120, int(raw.get('update_warning_minutes', 10))))
    out['update_warning_minutes'] = warn
    port = WEB_PORT
    with contextlib.suppress(TypeError, ValueError):
        port = int(raw.get('web_port') or WEB_PORT)
    out['web_port'] = port if 1024 <= port <= 65535 else WEB_PORT
    out['web_remote'] = raw.get('web_remote') is True
    password = raw.get('web_password')
    out['web_password'] = password if isinstance(password, dict) and password.get('hash') and password.get('salt') else None
    out['start_servers'] = raw.get('start_servers') is not False
    out['version_gate'] = raw.get('version_gate') is not False
    out['required_version'] = clean_text(raw.get('required_version'), 24)
    return out


def load_config():
    default = os.path.join(ROOT, 'server.default.json')
    if not os.path.exists(CONFIG) and os.path.exists(default):
        os.makedirs(DATA, exist_ok=True)
        shutil.copy(default, CONFIG)
    try:
        raw = json.loads(read_text(CONFIG, '{}') or '{}')
    except ValueError:
        raw = {}
    return clean_config(raw)


def save_config(cfg):
    write_atomic(CONFIG, json.dumps(cfg, indent=2, ensure_ascii=False) + '\n')


def password_record(password):
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac('sha256', password.encode('utf-8'), salt, 200000)
    return {'salt': base64.b64encode(salt).decode(), 'hash': base64.b64encode(digest).decode()}


def password_ok(record, password):
    if not record:
        return False
    try:
        salt, want = base64.b64decode(record['salt']), base64.b64decode(record['hash'])
    except (KeyError, ValueError, TypeError):
        return False
    return hmac.compare_digest(hashlib.pbkdf2_hmac('sha256', password.encode('utf-8'), salt, 200000), want)


# ---------------------------------------------------------------------------------------------------------- processes

if WINDOWS:
    import ctypes
    _k32 = ctypes.windll.kernel32
    _k32.OpenProcess.restype = ctypes.c_void_p

    def pid_image(pid):
        """The program file of a running process, or '' when there is none of that number."""
        handle = _k32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ''
        handle = ctypes.c_void_p(handle)
        buf, size, code = ctypes.create_unicode_buffer(1024), ctypes.c_ulong(1024), ctypes.c_ulong(0)
        ok = _k32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size))
        _k32.GetExitCodeProcess(handle, ctypes.byref(code))
        _k32.CloseHandle(handle)
        return buf.value if ok and code.value == 259 else ''  # STILL_ACTIVE

    def kill_pid(pid):
        subprocess.run(['taskkill', '/PID', str(int(pid)), '/T', '/F'], capture_output=True,
                       creationflags=0x08000000)
else:
    import signal

    def pid_image(pid):
        try:
            with open('/proc/%d/cmdline' % int(pid), 'rb') as f:
                return f.read().replace(b'\0', b' ').decode('utf-8', 'replace').strip()
        except OSError:
            try:
                os.kill(int(pid), 0)
                return '?'
            except OSError:
                return ''

    def kill_pid(pid):
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.kill(int(pid), sig)
            except OSError:
                return
            for _ in range(20):
                time.sleep(0.1)
                if not pid_image(pid):
                    return


def spawn(args, env=None, cwd=None, output=None):
    """A child with no window, its own session (a Ctrl+C in the manager's terminal is the manager's), its output in
    `output` (a file) or nowhere."""
    out = open(output, 'ab') if output else subprocess.DEVNULL
    kwargs = {'env': env, 'cwd': cwd, 'stdin': subprocess.DEVNULL, 'stdout': out, 'stderr': subprocess.STDOUT}
    if WINDOWS:
        kwargs['creationflags'] = 0x08000000  # CREATE_NO_WINDOW
    else:
        kwargs['start_new_session'] = True
    try:
        return subprocess.Popen(args, **kwargs)
    finally:
        if output:
            out.close()


def stop_process(proc, grace=3.0):
    if not proc or proc.poll() is not None:
        return
    try:
        proc.terminate()
        proc.wait(grace)
    except (OSError, subprocess.TimeoutExpired):
        with contextlib.suppress(OSError):
            proc.kill()
        with contextlib.suppress(OSError, subprocess.TimeoutExpired):
            proc.wait(5)


# ---------------------------------------------------------------------------------------------------------- network

def lan_addresses():
    """This machine's addresses on its networks, the one the default route uses first."""
    found = []
    with contextlib.suppress(OSError):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(('192.0.2.1', 9))  # no packet is sent: the address the system would send from
            found.append(s.getsockname()[0])
    with contextlib.suppress(OSError):
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if ip not in found:
                found.append(ip)
    return [ip for ip in found if not ip.startswith('127.') and ip != '0.0.0.0']


class Upnp:
    """The router's port forwarding (UPnP IGD), with the standard library: found once by SSDP, then asked to map each
    port to this machine and for the router's public address."""

    def __init__(self):
        self.control, self.service, self.local, self.looked = None, None, None, 0.0
        self.external, self.error = '', ''

    def find(self):
        if self.control or time.time() - self.looked < 300:
            return bool(self.control)
        self.looked = time.time()
        message = ('M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 2\r\n'
                   'ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n').encode()
        locations = []
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.settimeout(2.5)
                s.sendto(message, ('239.255.255.250', 1900))
                end = time.time() + 2.5
                while time.time() < end:
                    try:
                        data, _ = s.recvfrom(4096)
                    except OSError:
                        break
                    m = re.search(rb'(?im)^location:\s*(\S+)', data)
                    if m and m.group(1).decode('latin-1') not in locations:
                        locations.append(m.group(1).decode('latin-1'))
        except OSError as e:
            self.error = 'no answer from a router (%s)' % e
            return False
        for location in locations:
            try:
                with urllib.request.urlopen(location, timeout=4) as r:
                    xml = r.read().decode('utf-8', 'replace')
            except (OSError, ValueError):
                continue
            for service in ('WANIPConnection:2', 'WANIPConnection:1', 'WANPPPConnection:1'):
                m = re.search(r'<serviceType>(urn:schemas-upnp-org:service:%s)</serviceType>.*?<controlURL>([^<]+)</controlURL>'
                              % re.escape(service), xml, re.S)
                if m:
                    self.service, self.control = m.group(1), urllib.parse.urljoin(location, m.group(2).strip())
                    host = urllib.parse.urlparse(location).hostname
                    with contextlib.suppress(OSError):
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                            s.connect((host, 9))
                            self.local = s.getsockname()[0]
                    return True
        self.error = 'no router answered with port forwarding (UPnP)'
        return False

    def call(self, action, fields):
        body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:%s xmlns:u="%s">%s</u:%s>'
                '</s:Body></s:Envelope>') % (action, self.service,
                                             ''.join('<%s>%s</%s>' % (k, v, k) for k, v in fields), action)
        request = urllib.request.Request(self.control, body.encode(), {
            'Content-Type': 'text/xml; charset="utf-8"', 'SOAPAction': '"%s#%s"' % (self.service, action)})
        with urllib.request.urlopen(request, timeout=5) as r:
            return r.read().decode('utf-8', 'replace')

    def open(self, ports):
        """Asks the router for each (port, 'UDP' or 'TCP'); the ones it would not open."""
        if not self.find() or not self.local:
            return ['%s %d' % (proto, port) for port, proto in ports]
        failed = []
        for port, proto in ports:
            try:
                self.call('AddPortMapping', [('NewRemoteHost', ''), ('NewExternalPort', port), ('NewProtocol', proto),
                                             ('NewInternalPort', port), ('NewInternalClient', self.local),
                                             ('NewEnabled', 1),
                                             ('NewPortMappingDescription', 'Battlefield 1943 Reborn %d' % port),
                                             ('NewLeaseDuration', 0)])
            except (OSError, ValueError):
                failed.append('%s %d' % (proto, port))
        with contextlib.suppress(OSError, ValueError):
            m = re.search(r'<NewExternalIPAddress>([^<]*)<', self.call('GetExternalIPAddress', []))
            if m:
                self.external = m.group(1).strip()
        return failed


def time_words(seconds):
    seconds = int(round(seconds))
    if seconds >= 3600 and seconds % 3600 == 0:
        n, unit = seconds // 3600, 'hour'
    elif seconds >= 60 and seconds % 60 == 0:
        n, unit = seconds // 60, 'minute'
    else:
        n, unit = seconds, 'second'
    return '%d %s%s' % (n, unit, '' if n == 1 else 's')


# ---------------------------------------------------------------------------------------------------------- the manager

class Manager:
    def __init__(self):
        self.lock = threading.RLock()
        self.cfg = load_config()
        self.flags = package_flags()
        self.version = package_version()
        self.token = secrets.token_urlsafe(24)       # this machine's command line tools (STATE)
        self.sessions = {}                           # a signed in browser: cookie -> expiry
        self.lines = []                              # the manager's own last notes
        self.run = {}                                # server number -> its processes and state
        self.backend = None                          # our own backend (LAN only, official)
        self.backend_mode = ''
        self.upnp = Upnp()
        self.ports_note = ''
        self.countdowns = []                         # running countdowns
        self.sent = []                               # the last messages sent
        self.seq = int(time.time())
        self.install = {'state': 'idle', 'text': '', 'done': 0, 'total': 0}
        self.update = {'state': 'idle', 'latest': '', 'text': '', 'checked': 0, 'asset': '', 'progress': 0}
        self.quit = threading.Event()
        self.restart_after_quit = False
        os.makedirs(LOGS, exist_ok=True)

    # ---- notes
    def note(self, text):
        line = '%s %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), text)
        with self.lock:
            self.lines.append(line)
            del self.lines[:-400]
        with contextlib.suppress(OSError):
            with open(os.path.join(LOGS, 'manager.log'), 'a', encoding='utf-8') as f:
                f.write(line + '\n')

    # ---- the servers
    def server_dir(self, n):
        return os.path.join(LOGS, 'server%d' % n)

    def official_key(self):
        return os.path.join(DATA, 'official.key')

    def own_backend(self):
        """'official', 'lan' or '': which backend of our own this package runs."""
        if self.flags['official']:
            return 'official' if self.flags['main_backend'] else ''  # a second official machine: the main backend's
        return 'lan' if self.cfg['lan_only'] else ''

    def public_address(self):
        return self.cfg['public_address'] or self.upnp.external or ''

    def ensure_backend(self):
        mode = self.own_backend()
        with self.lock:
            if self.backend and (self.backend.poll() is not None or self.backend_mode != mode):
                stop_process(self.backend)
                self.backend = None
            if not mode or self.backend:
                return
            args = [sys.executable, '-I', os.path.join(HERE, 'plasma.py'), '--host', BACKEND_BIND or '0.0.0.0',
                    '--log', os.path.join(LOGS, 'backend.log')]
            public = self.public_address()
            if mode == 'lan':
                args.append('--lan')
                if public:
                    args += ['--theater-host', public]
            else:
                if not os.path.exists(self.official_key()):
                    raise RuntimeError('This is the official server package, and official.key is not in %s. Copy it in '
                                       'from your own copy (it is never in a download).' % DATA)
                args += ['--theater-host', public or (lan_addresses() or ['127.0.0.1'])[0],
                         '--official-key-file', self.official_key()]
                if self.cfg['required_version']:
                    args += ['--required-version', self.cfg['required_version']]
                elif self.cfg['version_gate']:
                    args += ['--version-feed', FEED]
            self.backend = spawn(args, output=os.path.join(LOGS, 'backend.out'))
            self.backend_mode = mode
            self.note('the %s started (PID %d)' % ('main backend' if mode == 'official' else 'LAN backend', self.backend.pid))
        time.sleep(1.0)

    def commands(self, n):
        s = self.cfg['servers'][n - 1]
        maps = [m for m in s['maps'] if m in MAP_LEVEL] or ['wake']
        directory = self.server_dir(n)
        address = '127.0.0.%d' % (LOOPBACK_BASE + n)
        game = [game_exe(), '--game_data_root=' + os.path.join(DATA, 'game'),
                '--user_data_root=' + os.path.join(directory, 'user'),
                '--log_file=' + os.path.join(directory, 'server.log'), '--fullscreen=false', '--window_start=hidden',
                '--server', '--cl=-dedicated -level ' + MAP_LEVEL[maps[0]], '--bf_map_rotation=' + ','.join(maps),
                '--bf_round_end_delay=%d' % s['round_end_delay']]
        # The game simulates at the rate set here: the server's own, else the manager's.
        tick = s.get('tick') or self.cfg['tick']
        game.append('--bf_tick_rate=%d' % tick)
        backend = (BACKEND_BIND or '127.0.0.1') if self.own_backend() else self.cfg['backend']
        host = [sys.executable, '-I', os.path.join(HERE, 'gmhost.py'), '--backend', backend + ':18250',
                '--listen', '0.0.0.0:%d' % s['port'], '--game-server', '%s:%d' % (address, GAME_PORT),
                '--name', s['name'].replace('"', "'"), '--map', MAP_TITLE[maps[0]],
                '--maps', ','.join(MAP_TITLE[m] for m in maps),
                '--map-file', os.path.join(directory, 'user', 'current_map.txt'),
                '--status-file', os.path.join(directory, 'status.json'), '--log', os.path.join(directory, 'host.log'),
                '--max-players', str(s['max_players']), '--tick', str(tick),
                '--broadcast-file', os.path.join(directory, 'broadcast.json')]
        if self.cfg['region']:
            host += ['--region', self.cfg['region']]
        if s['description']:
            host += ['--description', s['description'].replace('"', "'")]
        if not s['listed']:
            host.append('--unlisted')
        if s['night'] and self.flags['night_ops']:
            host.append('--night')
        if self.flags['official']:
            if not os.path.exists(self.official_key()):
                raise RuntimeError('This is the official server package, and official.key is not in %s. Copy it in '
                                   'from your own copy (it is never in a download).' % DATA)
            host += ['--official-key-file', self.official_key()]
        env = dict(os.environ)
        env.update({'BF_LOOPBACK_ONLY': '1', 'BF_LOOPBACK_ADDR': address, 'SDL_WINDOW_ACTIVATE_WHEN_SHOWN': '0'})
        if not WINDOWS:  # no display on a server: SDL's offscreen video driver; the program's own libraries first
            env.setdefault('SDL_VIDEO_DRIVER', 'offscreen')
            env.setdefault('SDL_VIDEODRIVER', 'offscreen')
            env['LD_LIBRARY_PATH'] = os.pathsep.join(x for x in (os.path.dirname(game_exe()), env.get('LD_LIBRARY_PATH')) if x)
        return game, host, env, directory

    def start_server(self, n, why='started'):
        with self.lock:
            if not 1 <= n <= len(self.cfg['servers']):
                raise RuntimeError('There is no server %d.' % n)
            if not game_installed():
                raise RuntimeError('The game is not installed yet: install it from your own copy on the Game files page.')
            if not os.path.exists(game_exe()):
                raise RuntimeError('The server program is missing (%s).' % game_exe())
            self.stop_server(n, keep_wanted=True)
            self.ensure_backend()
            game, host, env, directory = self.commands(n)
            os.makedirs(os.path.join(directory, 'user'), exist_ok=True)
            for old in (os.path.join(directory, 'user', 'current_map.txt'), os.path.join(directory, 'status.json'),
                        os.path.join(directory, 'broadcast.json')):
                with contextlib.suppress(OSError):
                    os.remove(old)
            state = self.run.setdefault(n, {'restarts': [], 'wait_until': 0})
            state['game'] = spawn(game, env=env, cwd=os.path.dirname(game_exe()))
            state['host'] = spawn(host, output=os.path.join(directory, 'host.out'))
            state['wanted'], state['since'], state['why'] = True, time.time(), ''
            write_atomic(os.path.join(directory, 'pids.txt'), '%d\n%d\n' % (state['game'].pid, state['host'].pid))
            self.note('server %d (%s) %s: UDP %d, %s' % (n, self.cfg['servers'][n - 1]['name'], why,
                                                         self.cfg['servers'][n - 1]['port'],
                                                         ', '.join(MAP_TITLE[m] for m in self.cfg['servers'][n - 1]['maps'])))
        self.save_wanted()

    def stop_server(self, n, keep_wanted=False):
        with self.lock:
            state = self.run.get(n)
            if not state:
                return
            running = any(state.get(k) and state[k].poll() is None for k in ('game', 'host'))
            for key in ('host', 'game'):
                stop_process(state.get(key))
                state[key] = None
            if not keep_wanted:
                state['wanted'] = False
            for name in ('pids.txt', 'status.json'):
                with contextlib.suppress(OSError):
                    os.remove(os.path.join(self.server_dir(n), name))
            if running and not keep_wanted:
                self.note('server %d stopped' % n)
        if not keep_wanted:
            self.save_wanted()

    def start_all(self):
        errors = []
        for n in range(1, len(self.cfg['servers']) + 1):
            try:
                self.start_server(n)
            except RuntimeError as e:
                errors.append(str(e))
                break
        self.open_ports()
        if errors:
            raise RuntimeError(errors[0])

    def stop_all(self):
        for n in list(self.run):
            self.stop_server(n)
        with self.lock:
            if self.backend:
                stop_process(self.backend)
                self.backend = None
                self.note('the backend stopped')

    def save_wanted(self):
        """Which servers were running, for the next start of the manager (and after an update)."""
        with self.lock:
            wanted = sorted(n for n, s in self.run.items() if s.get('wanted'))
        with contextlib.suppress(OSError):
            write_atomic(os.path.join(LOGS, 'wanted.json'), json.dumps(wanted))

    def clear_leftovers(self):
        """Servers an earlier manager left behind (it was closed hard, or it crashed): stopped, by the process numbers
        each start writes down, when those are still our programs."""
        if not os.path.isdir(LOGS):
            return
        for name in os.listdir(LOGS):
            pids = os.path.join(LOGS, name, 'pids.txt')
            if not name.startswith('server') or not os.path.exists(pids):
                continue
            for text in read_text(pids).split():
                if not text.isdigit():
                    continue
                image = pid_image(int(text)).lower()
                if 'bf1943' in image or 'python' in image:
                    kill_pid(int(text))
                    self.note('stopped a leftover of %s (PID %s)' % (name, text))
            with contextlib.suppress(OSError):
                os.remove(pids)

    def open_ports(self):
        """The router's ports (UPnP), where the settings ask for it; what could not be opened is said on the page."""
        if not self.cfg['upnp']:
            self.ports_note = ''
            return
        ports = [(s['port'], 'UDP') for s in self.cfg['servers']]
        if self.own_backend():
            ports += [(p, 'TCP') for p in BACKEND_PORTS]

        def work():
            failed = self.upnp.open(ports)
            self.ports_note = ('Open these on your router: ' + ', '.join(failed)) if failed else 'Router ports opened (UPnP).'
            self.note('router ports: ' + self.ports_note)
        threading.Thread(target=work, daemon=True).start()

    def server_status(self, n):
        s = self.cfg['servers'][n - 1]
        state = self.run.get(n) or {}
        game_up = bool(state.get('game')) and state['game'].poll() is None
        host_up = bool(state.get('host')) and state['host'].poll() is None
        out = {'n': n, 'name': s['name'], 'port': s['port'], 'wanted': bool(state.get('wanted')), 'state': 'stopped',
               'players': 0, 'names': [], 'map': '', 'next_map': '', 'why': state.get('why', ''), 'reachable': None,
               'reachable_local': None, 'address': '', 'since': state.get('since', 0), 'listed': s['listed'],
               'official': False}
        if state.get('wanted') and not (game_up and host_up):
            wait = state.get('wait_until', 0) - time.time()
            out['state'] = 'waiting' if wait > 0 else 'restarting'
            return out
        if not (game_up and host_up):
            return out
        out['state'] = 'starting'
        try:
            status = json.loads(read_text(os.path.join(self.server_dir(n), 'status.json'), '') or '{}')
        except ValueError:
            status = {}
        if status.get('state') == 'registered':
            out['state'] = 'running'
        elif status.get('state') == 'refused':
            out['state'], out['why'] = 'refused', str(status.get('why', ''))
        for key in ('players', 'map', 'next_map', 'reachable', 'reachable_local', 'address', 'official'):
            if key in status:
                out[key] = status[key]
        if isinstance(status.get('names'), list):
            out['names'] = [str(x)[:32] for x in status['names']][:64]
        return out

    def watch(self):
        """Every two seconds: a server whose game or host has gone (a crash) is started again; three times in ten
        minutes and it waits five. Our own backend too. Then the countdowns."""
        while not self.quit.wait(2.0):
            try:
                with self.lock:
                    numbers = [n for n, s in self.run.items() if s.get('wanted')]
                for n in numbers:
                    with self.lock:
                        state = self.run.get(n)
                        if not state or not state.get('wanted') or n > len(self.cfg['servers']):
                            continue
                        gone = [k for k in ('game', 'host') if not state.get(k) or state[k].poll() is not None]
                        if not gone or time.time() < state.get('wait_until', 0):
                            continue
                        recent = [t for t in state['restarts'] if t > time.time() - 600]
                        if len(recent) >= 3:
                            state['wait_until'], state['restarts'] = time.time() + 300, []
                            state['why'] = 'started again 3 times in 10 minutes; the next try in 5 minutes'
                            self.note('server %d: %s' % (n, state['why']))
                            continue
                        state['restarts'] = recent + [time.time()]
                        what = 'its game process ended' if 'game' in gone else 'its host ended'
                    self.note('server %d: %s; starting it again' % (n, what))
                    try:
                        self.start_server(n, 'started again')
                    except RuntimeError as e:
                        self.note('server %d could not be started again: %s' % (n, e))
                with self.lock:
                    backend_gone = self.backend and self.backend.poll() is not None
                if backend_gone:
                    self.note('the backend ended; starting it again')
                    with contextlib.suppress(RuntimeError):
                        self.ensure_backend()
                self.tick_countdowns()
            except Exception as e:  # the watch never ends for one bad turn
                self.note('watch: %s: %s' % (type(e).__name__, e))

    # ---- messages to the players
    def say(self, n, kind, text):
        """One message to everyone on server n: the host reads this file and sends each new seq (gmhost.py)."""
        text = clean_text(text, 200)
        if not text:
            return
        with self.lock:
            self.seq += 1
            seq = self.seq
        directory = self.server_dir(n)
        if not os.path.isdir(directory):
            return
        write_atomic(os.path.join(directory, 'broadcast.json'),
                     json.dumps({'seq': seq, 'kind': kind if kind in ('update', 'restart') else 'custom', 'text': text,
                                 'seconds': 10}))

    def running_servers(self):
        with self.lock:
            return sorted(n for n, s in self.run.items() if s.get('wanted') and n <= len(self.cfg['servers']))

    def broadcast(self, kind, seconds, text='', servers=None, restart=False, update=False):
        """A message now; with a time, a countdown: said now, said again as the time runs out, and at its end the
        servers are restarted (`restart`) or the update is installed (`update`)."""
        kind = kind if kind in ('update', 'restart', 'custom') else 'custom'
        targets = [n for n in (servers or self.running_servers()) if n in self.running_servers()]
        template = PRESETS.get(kind) or clean_text(text, 200)
        if not template:
            raise RuntimeError('Type the message first.')
        if not targets and not update:
            raise RuntimeError('No server is running: there is nobody to tell.')
        seconds = max(0, min(24 * 3600, int(seconds)))
        if '{time}' not in template or seconds <= 0:
            if '{time}' in template:
                raise RuntimeError('Pick how long until it happens.')
            for n in targets:
                self.say(n, kind, template)
            self.remember(kind, template, targets)
            return None
        countdown = {'id': secrets.token_hex(4), 'kind': kind, 'template': template, 'servers': targets,
                     'end': time.time() + seconds, 'restart': bool(restart), 'update': bool(update),
                     'marks': [m for m in ANNOUNCE_AT if m < seconds], 'seconds': seconds}
        with self.lock:
            self.countdowns.append(countdown)
        self.announce(countdown, seconds)
        return countdown['id']

    def announce(self, countdown, left):
        text = countdown['template'].replace('{time}', time_words(left))
        for n in countdown['servers']:
            self.say(n, countdown['kind'], text)
        self.remember(countdown['kind'], text, countdown['servers'])

    def remember(self, kind, text, servers):
        with self.lock:
            self.sent.append({'at': int(time.time()), 'kind': kind, 'text': text, 'servers': list(servers)})
            del self.sent[:-50]
        self.note('message to server%s %s: %s' % ('' if len(servers) == 1 else 's',
                                                   ', '.join(map(str, servers)) or '(none)', text))

    def cancel_countdown(self, ident):
        with self.lock:
            found = [c for c in self.countdowns if c['id'] == ident]
            self.countdowns = [c for c in self.countdowns if c['id'] != ident]
        for c in found:
            self.note('countdown cancelled (%s)' % c['kind'])
        return bool(found)

    def tick_countdowns(self):
        with self.lock:
            active = list(self.countdowns)
        for c in active:
            left = c['end'] - time.time()
            while c['marks'] and left <= c['marks'][0] + 1:
                mark = c['marks'].pop(0)
                if left > mark - 5:  # not one that was passed long ago (the machine slept)
                    self.announce(c, mark)
            if left > 0:
                continue
            with self.lock:
                self.countdowns = [x for x in self.countdowns if x['id'] != c['id']]
            if c['update']:
                threading.Thread(target=self.apply_update, daemon=True).start()
            elif c['restart']:
                for n in c['servers']:
                    self.note('server %d: restarting, its countdown is over' % n)
                    with contextlib.suppress(RuntimeError):
                        self.start_server(n, 'restarted')

    # ---- the game's files
    def install_game(self, source):
        source = os.path.abspath(os.path.expanduser(source))
        if not os.path.exists(source):
            raise RuntimeError('There is no file or folder at %s.' % source)
        if not os.path.exists(game_exe()):
            raise RuntimeError('The server program is missing (%s).' % game_exe())
        with self.lock:
            if self.install['state'] == 'running':
                raise RuntimeError('An install is running already.')
            if any(s.get('wanted') for s in self.run.values()):
                raise RuntimeError('Stop the servers first: they are using the game files.')
            total = 0
            if os.path.isfile(source):
                total = os.path.getsize(source)
            else:
                for folder, _, files in os.walk(source):
                    total += sum(os.path.getsize(os.path.join(folder, f)) for f in files)
            self.install = {'state': 'running', 'text': 'Installing from %s' % source, 'done': 0, 'total': total}

        def work():
            game = os.path.join(DATA, 'game')
            out = os.path.join(LOGS, 'install', 'install.out')
            os.makedirs(os.path.dirname(out), exist_ok=True)
            env = dict(os.environ)
            if not WINDOWS:
                env.setdefault('SDL_VIDEO_DRIVER', 'offscreen')
                env.setdefault('SDL_VIDEODRIVER', 'offscreen')
            proc = spawn([game_exe(), '--install_game=' + source, '--game_data_root=' + game,
                          '--user_data_root=' + os.path.join(LOGS, 'install'), '--server'], env=env,
                         cwd=os.path.dirname(game_exe()), output=out)
            while proc.poll() is None:
                done = 0
                for folder, _, files in os.walk(game):
                    for f in files:
                        with contextlib.suppress(OSError):
                            done += os.path.getsize(os.path.join(folder, f))
                self.install['done'] = done
                time.sleep(0.5)
            tail = read_text(out).strip().splitlines()[-3:]
            # A folder that holds the game unpacked already (a player's own game folder): the program has checked
            # that it is this game and only names it ("installed: <that folder>"), so its files are copied here,
            # default.xex last: a copy cut short is never taken for a finished install.
            named = next((l[len('installed: '):].strip() for l in reversed(tail) if l.startswith('installed: ')), '')
            if (proc.returncode == 0 and named and not game_installed()
                    and os.path.isfile(os.path.join(named, 'default.xex'))):
                self.install['text'] = 'Copying the game from %s' % named
                try:
                    done, last = 0, []
                    for folder, _, files in os.walk(named):
                        inner = os.path.relpath(folder, named)
                        dest = game if inner == '.' else os.path.join(game, inner)
                        os.makedirs(dest, exist_ok=True)
                        for f in files:
                            pair = (os.path.join(folder, f), os.path.join(dest, f))
                            if inner == '.' and f.lower() == 'default.xex':
                                last.append(pair)
                                continue
                            shutil.copyfile(*pair)
                            done += os.path.getsize(pair[1])
                            self.install['done'] = done
                    for pair in last:
                        shutil.copyfile(*pair)
                except OSError as e:
                    tail = ['The files could not be copied: %s' % e]
            ok = game_installed()
            self.install = {'state': 'done' if ok else 'failed', 'done': self.install['total'] if ok else 0,
                            'total': self.install['total'],
                            'text': 'The game is installed.' if ok else ('The game could not be installed. ' + ' '.join(tail))}
            self.note('game install from %s: %s' % (source, self.install['text']))
        threading.Thread(target=work, daemon=True).start()

    # ---- updates
    def asset_name(self, names):
        """This platform's server package among a release's files."""
        # One package for every host: the official one differs only in what is kept at an update (package.json and
        # official.key, the owner's own), so it updates from the same file.
        pattern = r'^BF1943-Reborn-Server-.*-windows\.zip$' if WINDOWS else r'^BF1943-Reborn-Server-.*-linux\.tar\.gz$'
        return next((n for n in names if re.match(pattern, n)), '')

    def check_update(self):
        """Looks at the release feed: the newest version and this platform's server package in it."""
        self.update.update(state='checking', text='Looking for a new release')
        try:
            request = urllib.request.Request(os.environ.get('BF_SERVER_FEED') or FEED,
                                             headers={'User-Agent': 'BF1943-Reborn-Server', 'Accept': 'application/vnd.github+json'})
            with urllib.request.urlopen(request, timeout=20) as r:
                release = json.loads(r.read().decode('utf-8'))
            latest = str(release.get('tag_name', '')).lstrip('v')
            assets = {a.get('name', ''): a.get('browser_download_url', '') for a in release.get('assets', [])}
            name = self.asset_name(list(assets))
            newer = version_key(latest) > version_key(self.version) and self.version != 'dev'
            self.update.update(latest=latest, checked=int(time.time()), asset=name, url=assets.get(name, ''),
                               sums=assets.get('SHA256SUMS.txt') or assets.get('SHA256SUMS', ''),
                               state='available' if newer and name else 'current',
                               text=('Version %s is out.' % latest) if newer and name
                               else ('Version %s is out, without a server package for this system yet.' % latest) if newer
                               else 'This is the newest version.')
        except (OSError, ValueError) as e:
            self.update.update(state='error', checked=int(time.time()), text='The release feed did not answer (%s).' % e)
        return self.update['state'] == 'available'

    def download_update(self):
        """The new package into DATA/updates, checked against the release's SHA256SUMS."""
        name, url = self.update.get('asset'), self.update.get('url')
        if not name or not url:
            raise RuntimeError('There is nothing to download.')
        folder = os.path.join(DATA, 'updates')
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, name)
        self.update.update(state='downloading', text='Downloading %s' % name, progress=0)
        digest = hashlib.sha256()
        request = urllib.request.Request(url, headers={'User-Agent': 'BF1943-Reborn-Server'})
        with urllib.request.urlopen(request, timeout=60) as r, open(path + '.part', 'wb') as f:
            total = int(r.headers.get('Content-Length') or 0)
            done = 0
            while True:
                block = r.read(1 << 16)
                if not block:
                    break
                f.write(block)
                digest.update(block)
                done += len(block)
                self.update['progress'] = int(100 * done / total) if total else 0
        if self.update.get('sums'):
            request = urllib.request.Request(self.update['sums'], headers={'User-Agent': 'BF1943-Reborn-Server'})
            with urllib.request.urlopen(request, timeout=30) as r:
                sums = r.read().decode('utf-8', 'replace')
            want = next((line.split()[0].lower() for line in sums.splitlines()
                         if line.strip() and line.split()[-1].lstrip('*') == name), '')
            if not want or want != digest.hexdigest():
                with contextlib.suppress(OSError):
                    os.remove(path + '.part')
                raise RuntimeError('The download does not match the release (its SHA-256 is not the one published).')
        else:
            raise RuntimeError('The release has no SHA256SUMS file: not installed.')
        os.replace(path + '.part', path)
        self.update.update(state='downloaded', text='Version %s is downloaded.' % self.update['latest'], file=path)
        return path

    def apply_update(self):
        """Stops the servers, puts the new package's files in place, and starts the manager again (it starts the
        servers that were running)."""
        wanted = []
        try:
            path = self.update.get('file')
            if not path or not os.path.exists(path):
                path = self.download_update()
            self.update.update(state='installing', text='Installing version %s' % self.update.get('latest', ''))
            self.note('update: installing %s' % os.path.basename(path))
            wanted = self.running_servers()
            self.stop_all()
            with contextlib.suppress(OSError):
                write_atomic(os.path.join(LOGS, 'wanted.json'), json.dumps(wanted))
            target = update_target()
            count = install_package(path, target)
            self.note('update: %d files of version %s in %s' % (count, self.update.get('latest', ''), target))
            self.update.update(state='restarting', text='Starting again on version %s' % self.update.get('latest', ''))
            self.restart_after_quit = True
            self.quit.set()
        except (OSError, RuntimeError, ValueError, zipfile.BadZipFile, tarfile.TarError) as e:
            self.update.update(state='error', text='The update was not installed: %s' % e)
            self.note('update failed: %s' % e)
            for n in wanted:  # the servers that were stopped for it come back, on the version they had
                try:
                    self.start_server(n, why='started again (the update was not installed)')
                except (RuntimeError, OSError) as again:
                    self.note('server %d not started again: %s' % (n, again))

    def update_now(self, warn_minutes=None):
        """An update found: with players on, told first ("Game has updated..."), then installed when the time is up;
        with nobody on, installed at once."""
        if self.update.get('state') not in ('available', 'downloaded'):
            if not self.check_update():
                return False
        with contextlib.suppress(RuntimeError, OSError):
            if self.update.get('state') == 'available':
                self.download_update()
        if self.update.get('state') != 'downloaded':
            return False
        players = sum(self.server_status(n)['players'] or 0 for n in self.running_servers())
        minutes = self.cfg['update_warning_minutes'] if warn_minutes is None else warn_minutes
        if players and minutes > 0:
            if not any(c['update'] for c in self.countdowns):
                self.broadcast('update', minutes * 60, update=True)
                self.note('update: %d players told, installing in %d minutes' % (players, minutes))
        else:
            threading.Thread(target=self.apply_update, daemon=True).start()
        return True

    def update_loop(self):
        """With auto update on: the feed looked at ten minutes after the start and every half hour."""
        wait = 600 if self.version != 'dev' else 10 ** 9
        with contextlib.suppress(ValueError):  # a test's first look, in seconds
            wait = float(os.environ.get('BF_SERVER_UPDATE_FIRST') or wait)
        while not self.quit.wait(wait):
            wait = 1800
            if not self.cfg['auto_update'] or self.version == 'dev':
                continue
            if self.flags['official']:
                continue  # official servers change only on their owner's word (the Updates page's button)
            with contextlib.suppress(Exception):
                if self.check_update():
                    self.update_now()

    # ---- what the page shows
    def state(self):
        with self.lock:
            cfg = json.loads(json.dumps(self.cfg))
            cfg['web_password'] = bool(cfg['web_password'])
            servers = [self.server_status(n) for n in range(1, len(self.cfg['servers']) + 1)]
            countdowns = [{'id': c['id'], 'kind': c['kind'], 'servers': c['servers'], 'left': max(0, int(c['end'] - time.time())),
                           'restart': c['restart'], 'update': c['update'],
                           'text': c['template'].replace('{time}', time_words(max(0, c['end'] - time.time())))}
                          for c in self.countdowns]
            backend_up = bool(self.backend) and self.backend.poll() is None
            return {
                'version': self.version, 'platform': 'windows' if WINDOWS else 'linux', 'official': self.flags['official'],
                'night_ops': self.flags['night_ops'], 'config': cfg, 'servers': servers,
                'backend': {'mode': self.own_backend(), 'running': backend_up},
                'game_installed': game_installed(), 'install': dict(self.install),
                'update': {k: v for k, v in self.update.items() if k not in ('url', 'sums', 'file')},
                'countdowns': countdowns, 'sent': list(reversed(self.sent[-20:])),
                'addresses': {'lan': lan_addresses(), 'public': self.public_address(), 'upnp': self.ports_note},
                'regions': [{'code': c, 'name': n} for c, n, _ in REGIONS],
                'maps': [{'key': k, 'title': t} for k, t, _ in MAPS],
                'presets': PRESETS, 'tick': self.cfg['tick'], 'data': DATA, 'root': ROOT, 'max_servers': MAX_SERVERS,
                'backend_ports': list(BACKEND_PORTS), 'now': int(time.time()),
                'autostart': autostart_enabled(),
            }

    def set_config(self, new):
        """The settings from the page: checked, kept, saved. What a running server reads only at its start (its name,
        its maps, the region, the mode) applies when it is started again: the page says so."""
        with self.lock:
            merged = dict(self.cfg)
            for key in ('servers', 'region', 'tick', 'lan_only', 'public_address', 'upnp', 'auto_update',
                        'update_warning_minutes', 'start_servers', 'version_gate', 'required_version', 'web_remote'):
                if key in new:
                    merged[key] = new[key]
            cleaned = clean_config(merged)
            cleaned['web_port'], cleaned['web_password'], cleaned['backend'] = self.cfg['web_port'], self.cfg['web_password'], self.cfg['backend']
            if cleaned['web_remote'] and not cleaned['web_password']:
                raise RuntimeError('Set a password before the page is opened to other machines.')
            removed = len(cleaned['servers']) < len(self.cfg['servers'])
            if removed:
                for n in range(len(cleaned['servers']) + 1, len(self.cfg['servers']) + 1):
                    self.stop_server(n)
            self.cfg = cleaned
            save_config(cleaned)
        return cleaned


def install_package(archive, target):
    """A release's server package (a zip, or a tar.gz) over the program in `target`. The host's own things are never
    touched: server.json, official.key, the game, the logs, the updates. A file in use on Windows (the running
    Python's) is moved aside first."""
    keep_names = {'server.json', 'official.key', 'package.json'}
    keep_folders = {'game', 'logs', 'updates', 'program'}
    os.makedirs(target, exist_ok=True)
    work = os.path.join(target, '.update-%d' % os.getpid())
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    try:
        if archive.lower().endswith('.zip'):
            with zipfile.ZipFile(archive) as z:
                for info in z.infolist():
                    name = info.filename.replace('\\', '/')
                    if name.startswith('/') or '..' in name.split('/'):
                        raise RuntimeError('The package holds a path outside its folder (%s).' % info.filename)
                z.extractall(work)
        else:
            with tarfile.open(archive) as t:
                for member in t.getmembers():
                    name = member.name.replace('\\', '/')
                    if name.startswith('/') or '..' in name.split('/') or member.islnk() or member.issym() or member.isdev():
                        raise RuntimeError('The package holds something that is not a plain file in its folder (%s).' % member.name)
                t.extractall(work)
        top = work
        entries = os.listdir(work)
        if len(entries) == 1 and os.path.isdir(os.path.join(work, entries[0])):
            top = os.path.join(work, entries[0])  # a package with its one folder at the top
        if not os.path.exists(os.path.join(top, 'server', 'manager.py')):
            raise RuntimeError('This is not a server package (no server/manager.py in it).')
        count = 0
        for folder, folders, files in os.walk(top):
            relative = os.path.relpath(folder, top)
            parts = [] if relative == '.' else relative.split(os.sep)
            if parts and parts[0] in keep_folders:
                folders[:] = []
                continue
            os.makedirs(os.path.join(target, *parts), exist_ok=True)
            for name in files:
                if not parts and name in keep_names:
                    continue
                source, destination = os.path.join(folder, name), os.path.join(target, *parts, name)
                try:
                    os.replace(source, destination)
                except OSError:
                    old = destination + '.old'
                    with contextlib.suppress(OSError):
                        os.remove(old)
                    os.replace(destination, old)
                    os.replace(source, destination)
                count += 1
        return count
    finally:
        shutil.rmtree(work, ignore_errors=True)


def remove_update_leftovers():
    """The files an update moved aside because they were in use (Windows: the running Python's own), removed at a
    later start, once nothing holds them."""
    folders = [os.path.join(ROOT, sub) for sub in ('bin', 'python', 'server')]
    names = [os.path.join(ROOT, name) for name in os.listdir(ROOT)] if os.path.isdir(ROOT) else []
    for folder in folders:
        for where, _, files in os.walk(folder):
            names += [os.path.join(where, name) for name in files]
    for path in names:
        if path.endswith('.old') and os.path.isfile(path):
            with contextlib.suppress(OSError):
                os.remove(path)


# ---------------------------------------------------------------------------------------------------------- at boot

RUN_KEY, RUN_NAME = r'Software\Microsoft\Windows\CurrentVersion\Run', 'BF1943RebornServer'


def autostart_command():
    python = sys.executable
    if WINDOWS and os.path.basename(python).lower() == 'python.exe':
        windowless = os.path.join(os.path.dirname(python), 'pythonw.exe')
        python = windowless if os.path.exists(windowless) else python
    return '"%s" "%s" run --no-browser' % (python, os.path.join(HERE, 'manager.py'))


def autostart_enabled():
    if not WINDOWS:
        return None  # Linux: a systemd unit, shown on the page
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            return bool(winreg.QueryValueEx(key, RUN_NAME)[0])
    except OSError:
        return False


def set_autostart(on):
    if not WINDOWS:
        raise RuntimeError('On Linux, start the manager from a systemd unit (the Settings page shows one).')
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
        if on:
            winreg.SetValueEx(key, RUN_NAME, 0, winreg.REG_SZ, autostart_command())
        else:
            with contextlib.suppress(OSError):
                winreg.DeleteValue(key, RUN_NAME)


# ---------------------------------------------------------------------------------------------------------- the page

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = 'BF1943RebornServer'
    manager = None

    def log_message(self, fmt, *args):  # quiet: the manager's own notes are the log
        pass

    # ---- who is asking
    def local(self):
        return self.client_address[0] in ('127.0.0.1', '::1', '::ffff:127.0.0.1')

    def cookie(self):
        for part in (self.headers.get('Cookie') or '').split(';'):
            name, _, value = part.strip().partition('=')
            if name == 'bfsession':
                return value
        return ''

    def allowed(self):
        """This machine's own tools by their token; a browser on this machine when no password is set; anyone who
        signed in with the password."""
        m = self.manager
        if self.local() and hmac.compare_digest(self.headers.get('X-BF-Token') or '', m.token):
            return True
        if not m.cfg['web_password']:
            return self.local()
        expiry = m.sessions.get(self.cookie(), 0)
        return expiry > time.time()

    def host_ok(self):
        """The page is asked for by this machine's own name or address (a web page elsewhere cannot point a browser
        here under another name), or, opened to other machines, by any."""
        if self.manager.cfg['web_remote']:
            return True
        host = (self.headers.get('Host') or '').rsplit(':', 1)[0].strip('[]').lower()
        return host in ('127.0.0.1', 'localhost', '::1')

    def send(self, code, body, kind='application/json', headers=()):
        data = body if isinstance(body, bytes) else (json.dumps(body) if kind == 'application/json' else str(body)).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', kind + ('; charset=utf-8' if kind.startswith('text/') or kind == 'application/json' else ''))
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('X-Frame-Options', 'DENY')
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        length = int(self.headers.get('Content-Length') or 0)
        if length > 1 << 20:
            raise ValueError('too large')
        raw = self.rfile.read(length) if length else b'{}'
        value = json.loads(raw.decode('utf-8') or '{}')
        if not isinstance(value, dict):
            raise ValueError('not an object')
        return value

    def do_GET(self):
        m = self.manager
        if not self.host_ok():
            return self.send(403, {'error': 'Open the manager by this machine\'s own address.'})
        path, _, query = self.path.partition('?')
        params = urllib.parse.parse_qs(query)
        if path in ('/', '/index.html'):
            page = read_text(os.path.join(UI, 'index.html'))
            if not page:
                return self.send(500, 'The manager\'s page is missing (server/manager_ui/index.html).', 'text/plain')
            return self.send(200, page, 'text/html')
        if path == '/api/session':
            return self.send(200, {'signed_in': self.allowed(), 'password': bool(m.cfg['web_password'])})
        if not path.startswith('/api/'):
            return self.send(404, {'error': 'not found'})
        if not self.allowed():
            return self.send(401, {'error': 'Sign in first.'})
        if path == '/api/state':
            return self.send(200, m.state())
        if path == '/api/log':
            which = (params.get('which') or ['manager'])[0]
            n = int((params.get('n') or ['1'])[0] or 1)
            files = {'manager': os.path.join(LOGS, 'manager.log'), 'backend': os.path.join(LOGS, 'backend.log'),
                     'host': os.path.join(m.server_dir(n), 'host.log'), 'game': os.path.join(m.server_dir(n), 'server.log')}
            path_ = files.get(which)
            if not path_:
                return self.send(404, {'error': 'no such log'})
            return self.send(200, {'file': path_, 'text': tail_text(path_, 300)})
        if path == '/api/browse':
            return self.send(200, browse((params.get('path') or [''])[0]))
        return self.send(404, {'error': 'not found'})

    def do_POST(self):
        m = self.manager
        if not self.host_ok():
            return self.send(403, {'error': 'Open the manager by this machine\'s own address.'})
        # A page from elsewhere cannot send this header without asking first, and it is never allowed to.
        if self.headers.get('X-Requested-With') != 'bf1943' and not self.headers.get('X-BF-Token'):
            return self.send(403, {'error': 'not from the manager\'s page'})
        origin = self.headers.get('Origin')
        if origin and urllib.parse.urlparse(origin).netloc != (self.headers.get('Host') or ''):
            return self.send(403, {'error': 'not from the manager\'s page'})
        try:
            data = self.body()
        except (ValueError, UnicodeDecodeError):
            return self.send(400, {'error': 'The request could not be read.'})
        path = self.path.partition('?')[0]
        if path == '/api/login':
            if m.cfg['web_password'] and password_ok(m.cfg['web_password'], str(data.get('password', ''))):
                session = secrets.token_urlsafe(24)
                m.sessions[session] = time.time() + 12 * 3600
                return self.send(200, {'ok': True}, headers=[('Set-Cookie', 'bfsession=%s; HttpOnly; SameSite=Strict; Path=/' % session)])
            time.sleep(1.0)
            return self.send(401, {'error': 'That is not the password.'})
        if not self.allowed():
            return self.send(401, {'error': 'Sign in first.'})
        try:
            result = self.act(path, data)
        except RuntimeError as e:
            return self.send(400, {'error': str(e)})
        except Exception as e:  # said to the page, and noted
            m.note('%s failed: %s: %s' % (path, type(e).__name__, e))
            return self.send(500, {'error': '%s: %s' % (type(e).__name__, e)})
        reply = {'ok': True, 'state': m.state()}
        if isinstance(result, dict):
            reply.update(result)
        return self.send(200, reply)

    def act(self, path, data):
        m = self.manager
        n = int(data.get('n') or 0)
        if path == '/api/config':
            m.set_config(data.get('config') or {})
            m.open_ports()
        elif path == '/api/start':
            if n:
                m.start_server(n)
                m.open_ports()
            else:
                m.start_all()
        elif path == '/api/stop':
            m.stop_server(n) if n else m.stop_all()
        elif path == '/api/restart':
            numbers = [n] if n else m.running_servers()
            for number in numbers:
                m.start_server(number, 'restarted')
        elif path == '/api/broadcast':
            servers = [int(x) for x in data.get('servers') or []]
            ident = m.broadcast(str(data.get('kind', 'custom')), int(data.get('seconds') or 0), str(data.get('text', '')),
                                servers or None, bool(data.get('restart')))
            return {'countdown': ident}
        elif path == '/api/broadcast/cancel':
            m.cancel_countdown(str(data.get('id', '')))
        elif path == '/api/update/check':
            m.check_update()
        elif path == '/api/update/apply':
            if not m.update_now(int(data['minutes']) if 'minutes' in data else None):
                raise RuntimeError(m.update.get('text') or 'There is no update to install.')
        elif path == '/api/install':
            m.install_game(str(data.get('path', '')))
        elif path == '/api/password':
            new = str(data.get('password', ''))
            if m.cfg['web_password'] and not password_ok(m.cfg['web_password'], str(data.get('current', ''))):
                raise RuntimeError('The current password is not right.')
            if new and len(new) < 8:
                raise RuntimeError('A password needs 8 characters or more.')
            with m.lock:
                m.cfg['web_password'] = password_record(new) if new else None
                if not new:
                    m.cfg['web_remote'] = False
                save_config(m.cfg)
        elif path == '/api/autostart':
            set_autostart(bool(data.get('on')))
        elif path == '/api/firewall':
            open_firewall(m)
        elif path == '/api/quit':
            if not data.get('keep_servers'):
                threading.Thread(target=m.stop_all, daemon=True).start()
            m.note('the manager is closing%s' % (' (the servers keep running until this machine restarts)' if data.get('keep_servers') else ''))
            threading.Timer(0.5, m.quit.set).start()
        else:
            raise RuntimeError('There is no such action.')
        return None


def tail_text(path, lines):
    try:
        with open(path, 'rb') as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 96 * 1024))
            text = f.read().decode('utf-8', 'replace')
    except OSError:
        return ''
    return '\n'.join(text.splitlines()[-lines:])


def browse(path):
    """A folder's entries for the page's file picker (the game's package is on this machine, not in the browser)."""
    if not path:
        if WINDOWS:
            drives = ['%s:\\' % c for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' if os.path.exists('%s:\\' % c)]
            return {'path': '', 'parent': None, 'entries': [{'name': d, 'dir': True, 'path': d} for d in drives]}
        path = os.path.expanduser('~')
    path = os.path.abspath(path)
    entries = []
    try:
        for name in sorted(os.listdir(path), key=str.lower):
            full = os.path.join(path, name)
            is_dir = os.path.isdir(full)
            size = 0
            if not is_dir:
                with contextlib.suppress(OSError):
                    size = os.path.getsize(full)
            entries.append({'name': name, 'dir': is_dir, 'path': full, 'size': size})
    except OSError as e:
        return {'path': path, 'parent': os.path.dirname(path) or None, 'entries': [], 'error': str(e)}
    parent = os.path.dirname(path)
    if parent == path:
        parent = '' if WINDOWS else None
    return {'path': path, 'parent': parent, 'entries': entries[:2000]}


def open_firewall(m):
    """Windows Firewall: the servers' ports, with its own question to the administrator (open_firewall.ps1)."""
    if not WINDOWS:
        raise RuntimeError('On Linux, open the ports in your firewall yourself (the Network page lists them).')
    script = os.path.join(ROOT, 'open_firewall.ps1')
    if not os.path.exists(script):
        raise RuntimeError('open_firewall.ps1 is not in the package.')
    subprocess.Popen(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script],
                     creationflags=0x08000000)
    m.note('Windows Firewall: asked to open the servers\' ports')


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = not WINDOWS


# ---------------------------------------------------------------------------------------------------------- commands

def running_manager():
    """The manager that runs for this data folder: (port, token), or None. It is asked on the port it wrote down,
    with its token: its process number says nothing from inside another sandbox (each start of a Flatpak has its
    own numbers), and a manager that died leaves the file behind."""
    try:
        info = json.loads(read_text(STATE, '') or '{}')
        port, token = int(info.get('port') or 0), str(info.get('token') or '')
    except (ValueError, TypeError):
        return None
    if not port or not token:
        return None
    request = urllib.request.Request('http://127.0.0.1:%d/api/session' % port, headers={'X-BF-Token': token})
    try:
        with urllib.request.urlopen(request, timeout=3) as r:
            answer = json.loads(r.read().decode('utf-8'))
    except (OSError, ValueError):
        return None
    return (port, token) if answer.get('signed_in') is True else None


def ask(path, data=None, timeout=30):
    """A request to the running manager from this machine's command line."""
    found = running_manager()
    if not found:
        raise RuntimeError('The manager is not running (start it with: manager.py run).')
    port, token = found
    body = None if data is None else json.dumps(data).encode()
    request = urllib.request.Request('http://127.0.0.1:%d%s' % (port, path), body,
                                     {'X-BF-Token': token, 'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as r:
            return json.loads(r.read().decode('utf-8'))
    except urllib.error.HTTPError as e:
        try:
            raise RuntimeError(json.loads(e.read().decode('utf-8')).get('error') or str(e))
        except ValueError:
            raise RuntimeError(str(e))
    except OSError as e:
        raise RuntimeError('The manager did not answer (%s).' % e)


def start_in_background(extra=()):
    """The manager as a process of its own, with no window, that outlives this one; True once it answers."""
    python = sys.executable
    if WINDOWS:
        windowless = os.path.join(os.path.dirname(python), 'pythonw.exe')
        python = windowless if os.path.exists(windowless) else python
    os.makedirs(LOGS, exist_ok=True)
    with open(os.path.join(LOGS, 'manager.out'), 'ab') as out:
        subprocess.Popen([python, os.path.abspath(__file__), 'run', '--no-browser'] + list(extra),
                         stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                         **({'creationflags': 0x08000008} if WINDOWS else {'start_new_session': True}))
    for _ in range(60):
        time.sleep(0.25)
        if running_manager():
            with contextlib.suppress(RuntimeError):
                ask('/api/state', timeout=3)
                return True
    return False


def open_browser(port):
    url = 'http://127.0.0.1:%d/' % port
    if os.environ.get('BF_SERVER_NO_BROWSER'):
        return
    with contextlib.suppress(Exception):
        import webbrowser
        webbrowser.open(url)


def newer_program():
    """A newer program this manager installed outside a package that cannot be written (a Flatpak, an AppImage):
    its manager.py, or None."""
    other = os.path.join(DATA, 'program', 'server', 'manager.py')
    if os.path.abspath(other) == os.path.abspath(__file__) or not os.path.exists(other):
        return None
    theirs = package_version(os.path.join(DATA, 'program'))
    return other if version_key(theirs) > version_key(package_version()) else None


def run(args):
    other = newer_program()
    if other and not os.environ.get('BF_SERVER_NO_HANDOVER'):
        env = dict(os.environ, BF_SERVER_ROOT=os.path.join(DATA, 'program'), BF_SERVER_DATA=DATA)
        os.execve(sys.executable, [sys.executable, other] + sys.argv[1:], env)
    found = running_manager()
    if found:
        print('The manager is running already: http://127.0.0.1:%d/' % found[0])
        if not args.no_browser:
            open_browser(found[0])
        return 0
    m = Manager()
    port = args.port or m.cfg['web_port']
    Handler.manager = m
    try:
        server = Server(('0.0.0.0' if m.cfg['web_remote'] and m.cfg['web_password'] else '127.0.0.1', port), Handler)
    except OSError as e:
        print('The manager\'s page could not be opened on port %d (%s). Another program may use it: pass --port.' % (port, e))
        return 1
    write_atomic(STATE, json.dumps({'pid': os.getpid(), 'port': port, 'token': m.token}))
    m.note('manager %s started: http://127.0.0.1:%d/ (data in %s)' % (m.version, port, DATA))
    m.clear_leftovers()
    remove_update_leftovers()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    threading.Thread(target=m.watch, daemon=True).start()
    threading.Thread(target=m.update_loop, daemon=True).start()
    # The servers that were running come back: at a start with "start the servers with the manager" on, and always
    # after an update (the manager stopped them itself to install it).
    after_update = bool(os.environ.pop('BF_SERVER_AFTER_UPDATE', ''))
    if (m.cfg['start_servers'] or after_update) and not args.no_start:
        try:
            wanted = json.loads(read_text(os.path.join(LOGS, 'wanted.json'), '') or 'null')
        except ValueError:
            wanted = None
        numbers = wanted if isinstance(wanted, list) else [n for n, s in enumerate(m.cfg['servers'], 1) if s['autostart']]
        if game_installed():
            for n in numbers:
                if isinstance(n, int) and 1 <= n <= len(m.cfg['servers']):
                    try:
                        m.start_server(n)
                    except RuntimeError as e:
                        m.note('server %d not started: %s' % (n, e))
                        break
            m.open_ports()
    print('Battlefield 1943 Reborn server manager %s: http://127.0.0.1:%d/' % (m.version, port), flush=True)
    if not args.no_browser:
        open_browser(port)
    try:
        while not m.quit.wait(0.5):
            pass
    except KeyboardInterrupt:
        m.note('the manager is closing (Ctrl+C)')
    m.quit.set()
    restart = m.restart_after_quit
    if not restart:
        m.stop_all()
    server.shutdown()
    with contextlib.suppress(OSError):
        os.remove(STATE)
    if restart:  # after an update: the new manager.py, from where it was installed
        root = update_target()
        script = os.path.join(root, 'server', 'manager.py')
        env = dict(os.environ, BF_SERVER_NO_BROWSER='1', BF_SERVER_AFTER_UPDATE='1')
        if root != ROOT:
            env.update(BF_SERVER_ROOT=root, BF_SERVER_DATA=DATA)
        command = [sys.executable, script, 'run', '--no-browser'] + (['--port', str(port)] if args.port else [])
        if WINDOWS:
            subprocess.Popen(command, env=env, creationflags=0x08000008)  # DETACHED_PROCESS | CREATE_NO_WINDOW
        else:
            os.execve(sys.executable, command, env)
    return 0


def main():
    ap = argparse.ArgumentParser(description='Battlefield 1943 Reborn server manager')
    sub = ap.add_subparsers(dest='command')
    r = sub.add_parser('run', help='the manager: its page, and the servers kept running')
    for parser in (ap, r):
        parser.add_argument('--no-browser', action='store_true', help='do not open the page in a browser')
        parser.add_argument('--no-start', action='store_true', help='do not start the servers')
        parser.add_argument('--port', type=int, default=0, help='the page\'s port (default: server.json web_port, %d)' % WEB_PORT)
    sub.add_parser('status', help='what runs')
    for name in ('start', 'stop', 'restart'):
        p = sub.add_parser(name)
        p.add_argument('n', type=int, nargs='?', default=0, help='the server\'s number (default: every server)')
    b = sub.add_parser('broadcast', help='a message to the players')
    b.add_argument('--kind', choices=['update', 'restart', 'custom'], default='custom')
    b.add_argument('--minutes', type=float, default=0)
    b.add_argument('--text', default='')
    b.add_argument('--server', type=int, action='append', default=[])
    b.add_argument('--no-restart', action='store_true', help='a restart or update countdown that only tells the players')
    sub.add_parser('update', help='look for a new release now, and install it')
    i = sub.add_parser('install', help='the game\'s files from your own copy of the game')
    i.add_argument('path')
    q = sub.add_parser('quit', help='stops the manager and its servers')
    q.add_argument('--keep-servers', action='store_true')
    sub.add_parser('where', help='the data folder (server.json, game, logs)')
    sub.add_parser('open', help='the page in a browser; starts the manager in the background when it is not running')
    args = ap.parse_args()
    command = args.command or 'run'
    try:
        if command == 'run':
            return run(args)
        if command == 'where':
            print(DATA)
            return 0
        if command == 'open':
            if not running_manager() and not start_in_background():
                raise RuntimeError('The manager did not start: see %s.' % os.path.join(LOGS, 'manager.out'))
            port = running_manager()[0]
            print('Battlefield 1943 Reborn server manager: http://127.0.0.1:%d/' % port)
            open_browser(port)
            return 0
        if command == 'status':
            state = ask('/api/state')
            print('manager %s, %s' % (state['version'], 'LAN only' if state['config']['lan_only'] else
                                      'official' if state['official'] else 'online'))
            for s in state['servers']:
                print('  %d  %-10s %-32s UDP %-5d %2s players  %s' % (s['n'], s['state'], s['name'][:32], s['port'],
                                                                      s['players'], s['map']))
            return 0
        if command in ('start', 'stop', 'restart'):
            if command == 'start' and not running_manager() and not start_in_background(['--no-start']):
                raise RuntimeError('The manager did not start: see %s.' % os.path.join(LOGS, 'manager.out'))
            ask('/api/' + command, {'n': args.n}, timeout=120)
            print('%s: done' % command)
            return 0
        if command == 'broadcast':
            ask('/api/broadcast', {'kind': args.kind, 'seconds': int(args.minutes * 60), 'text': args.text,
                                   'servers': args.server, 'restart': args.kind == 'restart' and not args.no_restart})
            print('sent')
            return 0
        if command == 'update':
            reply = ask('/api/update/apply', {}, timeout=600)
            print(reply['state']['update'].get('text', ''))
            return 0
        if command == 'install':
            ask('/api/install', {'path': os.path.abspath(args.path)})
            while True:
                time.sleep(1)
                install = ask('/api/state')['install']
                if install['state'] != 'running':
                    print(install['text'])
                    return 0 if install['state'] == 'done' else 1
        if command == 'quit':
            ask('/api/quit', {'keep_servers': args.keep_servers})
            print('the manager is closing')
            return 0
    except RuntimeError as e:
        print(str(e))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
