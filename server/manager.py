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
    manager.py broadcast --kind maintenance --minutes 15 --to official
    manager.py broadcast --kind custom --text "..." --to everyone
                                                 official packages: --to official through the main backend to every
                                                 official server (here and on the other machines), --to everyone to
                                                 every server registered with it, community ones too (network
                                                 broadcasts: they tell the players, never restart a server)
    manager.py community                         the community servers registered now, with their host keys
    manager.py promote <host key> [--note TEXT]  that host's servers listed with the Reborn ones, marked
                                                 TRUSTED COMMUNITY SERVER
    manager.py demote <host key> | promoted      back to the Community tab; the promoted list
    manager.py maintenance on --preset deploy [--minutes 15] [--allow ADDRESS]
    manager.py maintenance on --text "Back after the new maps are in" | off | status
                                                 maintenance mode on the machine that runs the main backend: new joins
                                                 are refused with the text; players in a match play on
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
# Tests only: the registry port the manager's own questions go to (signed lookups, broadcasts); 18250 otherwise.
REGISTRY_PORT = int(os.environ.get('BF_SERVER_REGISTRY_PORT') or 18250)
MAIN_BACKEND = '127.0.0.1'
FEED = 'https://api.github.com/repos/TekRantGaming/Battlefield-1943-Reborn/releases/latest'
# The messages an admin can send from a list (TekRantGaming, 13 Nov 2026), {time} the time left as words.
PRESETS = {
    'update': 'Game has updated to a new version server restarting in {time}',
    'restart': 'This server will restart in {time}',
    'maintenance': 'This server goes down for maintenance in {time}',
}
# Network broadcasts (--to official, --to everyone): they reach servers of other machines that this one does not
# restart, so their words say what happens to the Reborn servers, not "this server".
NETWORK_PRESETS = {
    'update': 'A new version of Battlefield 1943 Reborn is out. The Reborn servers update in {time}',
    'restart': 'The Reborn servers restart in {time}',
    'maintenance': 'The Reborn servers go down for maintenance in {time}',
}
KINDS = ('update', 'restart', 'maintenance', 'custom')
NETWORK_SCOPES = ('official', 'everyone')
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
# Maintenance mode (TekRantGaming, 15 Nov 2026): the switch our own backend reads (plasma.py --maintenance-file,
# maintenance_now). Kept in the data folder, so it holds through a restart of the backend or of the manager; a deploy
# to a new folder copies it with server.json.
MAINTENANCE_FILE = os.path.join(DATA, 'maintenance.json')
# Promoted community servers (TekRantGaming, 16 Nov 2026): their host keys, read by our own main backend
# (plasma.py --promoted-file, Promoted) within seconds of a change. manager.py promote, demote, promoted.
PROMOTED_FILE = os.path.join(DATA, 'promoted_servers.json')
# Moderation (server/bans.py, docs/MODERATION.md): this manager's own ban list (its servers only), the Reborn network's
# list (only on the machine that runs the main backend: the official manager), the moderation log, and the players its
# servers had in the last week (the page's lookup; addresses in it kept SEEN_DAYS at most).
BANS_FILE = os.path.join(DATA, 'bans.json')
NETWORK_BANS_FILE = os.path.join(DATA, 'network_bans.json')
MODERATION_LOG = os.path.join(DATA, 'moderation_log.jsonl')
SEEN_FILE = os.path.join(DATA, 'players_seen.json')
SEEN_DAYS = 7
# The dashboard (server/dashboard.py, docs/DASHBOARD.md): its 90 days of history, and the public stats page's port
# (off unless the owner turns it on). BF_SERVER_PUBLIC_BIND (tests): the address that page listens on (every one).
DASHBOARD_DIR = os.path.join(DATA, 'dashboard')
PUBLIC_PORT = 18290
# The community page (TekRantGaming, 20 Nov 2026: "a fully customizable community server external page that they can
# host as a website for people to look at their server"): the public stats page with the owner's branding. Its logo
# lives in the data folder; the project's own links are these.
PUBLIC_LOGO = os.path.join(DATA, 'public_logo')        # + .png, .jpg, .webp or .gif
PUBLIC_LOGO_TYPES = {'png': 'image/png', 'jpg': 'image/jpeg', 'webp': 'image/webp', 'gif': 'image/gif'}
PUBLIC_LOGO_MOST = 1 << 20
PUBLIC_THEMES = ('field', 'night', 'desert', 'steel', 'light')
PUBLIC_SECTIONS = ('connect', 'cards', 'servers', 'players', 'maps', 'hours', 'leaderboard', 'activity', 'network',
                   'rules')
# Their own code (TekRantGaming, 20 Nov 2026: "they don't have to use our kind of implementation, they can use their own
# custom ... they can code it themselves and then it'll display"): CSS over our page, or a whole site of their own in
# this folder (index.html and its files), which reads the same numbers from /stats.json.
PUBLIC_CSS_MOST = 20000
COMMUNITY_SITE = os.path.join(DATA, 'community_site')
SITE_TYPES = {'.html': 'text/html', '.htm': 'text/html', '.css': 'text/css', '.js': 'text/javascript',
              '.json': 'application/json', '.txt': 'text/plain', '.png': 'image/png', '.jpg': 'image/jpeg',
              '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp', '.svg': 'image/svg+xml',
              '.ico': 'image/x-icon', '.woff': 'font/woff', '.woff2': 'font/woff2', '.ttf': 'font/ttf',
              '.mp4': 'video/mp4', '.webm': 'video/webm', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg'}
SITE_FILE_MOST = 20 << 20
PROJECT_DOWNLOAD = 'https://github.com/TekRantGaming/Battlefield-1943-Reborn/releases/latest'
PROJECT_SITE = 'https://github.com/TekRantGaming/Battlefield-1943-Reborn'
PROJECT_DISCORD = 'https://discord.gg/8eakYSvBZS'
PUBLIC_BIND = os.environ.get('BF_SERVER_PUBLIC_BIND') or ''


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


def write_atomic_bytes(path, data):
    """A file's bytes written whole (the community page's logo)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = '%s.tmp%d' % (path, os.getpid())
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, path)


def notices():
    """plasma.py, for its maintenance presets and its text check: one list for the backend and the manager."""
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import plasma
    return plasma


def read_maintenance():
    """The maintenance switch as it is on disk ({} when there is none)."""
    try:
        raw = json.loads(read_text(MAINTENANCE_FILE, '') or '{}')
    except ValueError:
        return {}
    return raw if isinstance(raw, dict) else {}


def maintenance_words(raw=None):
    """One line for the switch: on or off, the preset, the time left, the text the players see."""
    raw = read_maintenance() if raw is None else raw
    state = notices().clean_maintenance(raw)
    if state and state['until'] and time.time() >= state['until']:
        return 'maintenance: off (its end time has passed)'
    if not state:
        return 'maintenance: off' + (' (the file does not check: %s)' % MAINTENANCE_FILE if raw.get('on') else '')
    left = (', %s left' % time_words(60 * ((int(state['until'] - time.time()) + 59) // 60)) if state['until']
            else ', until switched off')
    allow = ', let in: %s' % ', '.join(sorted(state['allow'])) if state['allow'] else ''
    return 'maintenance: on (%s%s%s). Players see: %s: %s' % (state['kind'], left, allow, state['title'], state['text'])


def set_maintenance(on, kind='', text='', minutes=0.0, allow=()):
    """Maintenance mode on or off: checked, then written where the backend reads it. Returns the line for it."""
    plasma = notices()
    if not on:
        raw = dict(read_maintenance(), on=False)
    else:
        text = ' '.join(str(text or '').split())
        kind = str(kind or ('custom' if text else 'deploy'))
        if kind not in plasma.NOTICE_PRESETS:
            raise RuntimeError('There is no such preset: %s (%s).' % (kind, ', '.join(plasma.NOTICE_PRESETS)))
        if kind == 'custom':
            problem = plasma.notice_text_problem(text)
            if problem:
                raise RuntimeError('That text cannot be shown to the players: %s.' % problem)
        elif text:
            raise RuntimeError('A preset has its own text: leave out --text, or use --preset custom.')
        try:
            minutes = float(minutes or 0)
        except (TypeError, ValueError):
            raise RuntimeError('The minutes must be a number.')
        if minutes < 0 or minutes > 24 * 60:
            raise RuntimeError('The minutes must be 0 (until switched off) to 1440.')
        allow = [str(a).strip() for a in allow or () if str(a).strip()]
        for a in allow:
            try:
                socket.inet_pton(socket.AF_INET6 if ':' in a else socket.AF_INET, a)
            except OSError:
                raise RuntimeError('%s is not an address.' % a)
        raw = {'on': True, 'kind': kind, 'text': text if kind == 'custom' else '',
               'until': int(time.time() + minutes * 60) if minutes else 0, 'allow': allow, 'since': int(time.time())}
    write_atomic(MAINTENANCE_FILE, json.dumps(raw, indent=1) + '\n')
    return maintenance_words(raw)


def bans_module():
    """server/bans.py: the ban lists and the moderation log."""
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import bans
    return bans


def dashboard_module():
    """server/dashboard.py: the dashboard's history and views."""
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import dashboard
    return dashboard


def host_module():
    """gmhost.py, for the network broadcast's signing (sign_network, sign_admin)."""
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import gmhost
    return gmhost


def official_key_path():
    return os.path.join(DATA, 'official.key')


def backend_registry():
    """Where this package's servers register: our own backend, or the main one (server.json backend)."""
    flags = package_flags()
    if flags['official'] and not flags['main_backend']:
        cfg = load_config()
        return (cfg.get('backend') or MAIN_BACKEND, REGISTRY_PORT)
    return (BACKEND_BIND or '127.0.0.1', REGISTRY_PORT)


def ask_backend(obj, timeout=10):
    """One request to the main backend's registry (one JSON line each way)."""
    address = backend_registry()
    try:
        with socket.create_connection(address, timeout=timeout) as s:
            s.sendall((json.dumps(obj) + '\n').encode())
            line = s.makefile('rb').readline()
    except OSError as e:
        raise RuntimeError('The main backend at %s:%d did not answer (%s).' % (address[0], address[1], e))
    try:
        return json.loads(line)
    except ValueError:
        raise RuntimeError('The main backend at %s:%d gave no answer.' % address)


def official_key_or_fail(what):
    if not package_flags()['official']:
        raise RuntimeError('%s only works on the official server package (it needs official.key).' % what)
    if not os.path.exists(official_key_path()):
        raise RuntimeError('official.key is not in %s.' % DATA)
    return host_module().read_key(official_key_path())


def send_network(kind, text, scope):
    """A network broadcast, signed here with official.key, through the main backend. Returns its reply."""
    key = official_key_or_fail('A broadcast to other machines\' servers')
    reply = ask_backend(host_module().sign_network(key, scope, kind, text))
    if not reply.get('ok'):
        raise RuntimeError('The main backend refused the network broadcast: %s.' % (reply.get('why') or 'no reason'))
    return reply


def community_hosts():
    """The community servers registered with the main backend now, with their host keys (a signed question)."""
    key = official_key_or_fail('The list of community servers')
    reply = ask_backend(host_module().sign_admin(key, 'hosts'))
    if not reply.get('ok'):
        raise RuntimeError('The main backend refused the question: %s.' % (reply.get('why') or 'no reason'))
    return reply.get('hosts') or []


def read_promoted():
    try:
        raw = json.loads(read_text(PROMOTED_FILE, '') or '{}')
    except ValueError:
        raise RuntimeError('%s is not valid JSON: fix it or delete it first.' % PROMOTED_FILE)
    rows = raw.get('servers') if isinstance(raw, dict) else None
    return [r for r in rows if isinstance(r, dict)] if isinstance(rows, list) else []


def write_promoted(rows):
    write_atomic(PROMOTED_FILE, json.dumps({'servers': rows}, indent=1) + '\n')


def pick_host(text, hosts):
    """A host key from what was typed: the whole key, or the start of one of the registered servers' keys (8 or
    more characters), or a registered server's game number."""
    text = str(text).strip().lower()
    if len(text) == 64 and all(c in '0123456789abcdef' for c in text):
        return text
    keys = sorted({h['host_key'] for h in hosts if h.get('host_key') and
                   (h['host_key'].startswith(text) and len(text) >= 8 or str(h.get('gid')) == text)})
    if len(keys) == 1:
        return keys[0]
    if not keys:
        raise RuntimeError('No community server registered now has the host key %s (manager.py community lists them; '
                           'a whole key of 64 characters is taken as it is).' % text)
    raise RuntimeError('%s is the start of more than one host key: type more of it.' % text)


def promote(text, note=''):
    hosts = []
    if not (len(text.strip()) == 64):
        hosts = community_hosts()
    else:
        with contextlib.suppress(RuntimeError):
            hosts = community_hosts()
    key = pick_host(text, hosts)
    names = sorted({h['name'] for h in hosts if h.get('host_key') == key})
    rows = read_promoted()
    if any(str(r.get('key', '')).lower() == key for r in rows):
        return 'host %s is promoted already' % key[:16]
    rows.append({'key': key, 'note': clean_text(note, 80) or ', '.join(names)[:80],
                 'since': time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())})
    write_promoted(rows)
    live = [h for h in hosts if h.get('host_key') == key]
    hidden = [h for h in live if h.get('hidden') or not h.get('listed')]
    words = 'promoted host %s (%s)' % (key[:16], ', '.join(names) or 'not registered now')
    if hidden:
        words += '. Not shown while it fails the build check or is not listed: %s' % hidden[0].get('hidden', 'not listed')
    return words + '. The backend lists it with the Reborn servers within a few seconds.'


def demote(text):
    rows = read_promoted()
    text = str(text).strip().lower()
    keep = [r for r in rows if not (str(r.get('key', '')).lower() == text or
                                    len(text) >= 8 and str(r.get('key', '')).lower().startswith(text))]
    gone = len(rows) - len(keep)
    if gone == 0:
        raise RuntimeError('No promoted host has the key %s (manager.py promoted lists them).' % text)
    if gone > 1:
        raise RuntimeError('%s is the start of more than one promoted key: type more of it.' % text)
    write_promoted(keep)
    return 'host %s is no longer promoted: back on the Community tab within a few seconds' % text[:16]


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
            'night': False, 'port': port, 'max_players': 24, 'autostart': True, 'tick': 0,
            'team_balance': True, 'balance_gap': 2, 'balance_min_players': 4, 'balance_delay': 20,
            'balance_block_stacking': True,
            # The server's world (gmhost.py's World; 14 Nov 2026): its clock and weather, per cent where it says, and
            # the player IDs who may change them in game.
            'day_night': False, 'time': 16.0, 'day_minutes': 0, 'cloud_cover': 35, 'cloud_rate': 33, 'weather': 0,
            'fog': 0, 'ground_fog': 0, 'wind': 30, 'cloud_height': 450, 'admins': ''}


def clean_text(value, limit):
    return ' '.join(''.join(c if c.isprintable() else ' ' for c in str(value or '')).split())[:limit]


def clean_long_text(value, limit):
    """A page's text with its lines kept (the community page's description and rules): printable, no runs of empty
    lines, at most `limit` characters."""
    lines = [' '.join(''.join(c if c.isprintable() else ' ' for c in line).split())
             for line in str(value or '').replace('\r\n', '\n').split('\n')]
    out = '\n'.join(lines)
    while '\n\n\n' in out:
        out = out.replace('\n\n\n', '\n\n')
    return out.strip()[:limit]


def clean_url(value):
    """An http or https link the community page may show, or ''."""
    url = str(value or '').strip()
    if not url or len(url) > 300 or any(c.isspace() or c in '"<>\\`' for c in url):
        return ''
    parts = urllib.parse.urlsplit(url)
    return url if parts.scheme in ('http', 'https') and parts.netloc and '@' not in parts.netloc else ''


def clean_public_page(page):
    """The public stats page's settings: the community page's branding, sections, links and how to join."""
    page = page if isinstance(page, dict) else {}
    port = PUBLIC_PORT
    with contextlib.suppress(TypeError, ValueError):
        port = int(page.get('port') or PUBLIC_PORT)
    accent = str(page.get('accent') or '').strip().lower()
    sections = page.get('sections') if isinstance(page.get('sections'), dict) else {}
    links = page.get('links') if isinstance(page.get('links'), dict) else {}
    connect = page.get('connect') if isinstance(page.get('connect'), dict) else {}
    custom = []
    for link in (links.get('custom') or [])[:6] if isinstance(links.get('custom'), list) else []:
        if isinstance(link, dict) and clean_url(link.get('url')):
            custom.append({'label': clean_text(link.get('label'), 40) or 'Link', 'url': clean_url(link.get('url'))})
    logo = str(page.get('logo') or '')
    return {'on': page.get('on') is True, 'port': port if 1024 <= port <= 65535 else PUBLIC_PORT,
            'names': page.get('names') is not False, 'title': clean_text(page.get('title'), 60),
            'description': clean_long_text(page.get('description'), 1000),
            'theme': page.get('theme') if page.get('theme') in PUBLIC_THEMES else 'field',
            'accent': accent if re.fullmatch(r'#[0-9a-f]{6}', accent) else '',
            'logo': logo if logo in PUBLIC_LOGO_TYPES else '',
            'sections': {k: sections.get(k) is not False for k in PUBLIC_SECTIONS},
            'links': {'download': links.get('download') is not False, 'discord': links.get('discord') is not False,
                      'custom': custom},
            'connect': {'address': connect.get('address') is not False,
                        'custom_address': clean_text(connect.get('custom_address'), 80),
                        'instructions': clean_long_text(connect.get('instructions'), 600)},
            'rules': clean_long_text(page.get('rules'), 1500),
            # Their own code: CSS over our page, or their own site (COMMUNITY_SITE) in its place.
            'custom_css': ''.join(c for c in str(page.get('custom_css') or '') if c.isprintable() or c in '\n\t')[:PUBLIC_CSS_MOST],
            'own_site': page.get('own_site') is True}


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
        entry['day_night'] = s.get('day_night') is True
        with contextlib.suppress(TypeError, ValueError):
            entry['time'] = max(0.0, min(23.75, float(s.get('time', 16.0))))
        for key, top in (('day_minutes', 1440), ('cloud_cover', 100), ('cloud_rate', 100), ('weather', 100), ('fog', 100),
                         ('ground_fog', 100), ('wind', 100)):
            with contextlib.suppress(TypeError, ValueError):
                entry[key] = max(0, min(top, int(s.get(key, entry[key]))))
        with contextlib.suppress(TypeError, ValueError):
            entry['cloud_height'] = max(100, min(1500, int(s.get('cloud_height', entry['cloud_height']))))
        entry['admins'] = ','.join(a for a in (''.join(c for c in part if c in '0123456789abcdefABCDEF').upper()[:8]
                                               for part in str(s.get('admins') or '').replace(';', ',').replace(' ', ',').split(','))
                                   if a)[:400]
        entry['autostart'] = s.get('autostart') is not False
        # Team balance (TekRantGaming, 15 Nov 2026: matches ended 7 against 3; bf/src/team_balance.cpp): on unless
        # turned off. A player who has died on the bigger team moves to the other once the teams are balance_gap or
        # more apart for balance_delay seconds, with balance_min_players on the server; balance_block_stacking refuses
        # a Change Team that would leave them so.
        entry['team_balance'] = s.get('team_balance') is not False
        entry['balance_block_stacking'] = s.get('balance_block_stacking') is not False
        for key, low, high in (('balance_gap', 2, 6), ('balance_min_players', 2, 24), ('balance_delay', 5, 300)):
            with contextlib.suppress(TypeError, ValueError):
                entry[key] = max(low, min(high, int(s.get(key) if s.get(key) is not None else entry[key])))
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
    # The public stats page (the dashboard's, docs/DASHBOARD.md): off unless turned on; player names on it or counts only.
    out['public_page'] = clean_public_page(raw.get('public_page'))
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
    port to this machine and for the router's public address. Each forward lasts LEASE seconds unless asked for again
    (the manager renews them while it runs) and is taken away when the manager closes (TekRantGaming, 16 Nov 2026: the
    forwards were permanent and never removed, so a host who stopped hosting kept an open port on the router)."""

    LEASE = 3600  # seconds; the manager asks again every RENEW
    RENEW = 1200

    def __init__(self):
        self.control, self.service, self.local, self.looked = None, None, None, 0.0
        self.external, self.error = '', ''
        self.mapped = {}  # (port, 'UDP' or 'TCP') -> the lease the router took (0: permanent, all some routers take)

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

    def add(self, port, proto, lease):
        self.call('AddPortMapping', [('NewRemoteHost', ''), ('NewExternalPort', port), ('NewProtocol', proto),
                                     ('NewInternalPort', port), ('NewInternalClient', self.local),
                                     ('NewEnabled', 1),
                                     ('NewPortMappingDescription', 'Battlefield 1943 Reborn %d' % port),
                                     ('NewLeaseDuration', lease)])

    def open(self, ports, permanent=False):
        """Asks the router for each (port, 'UDP' or 'TCP') for LEASE seconds (or for good: `permanent`, or a router
        that takes nothing else, its error 725 OnlyPermanentLeasesSupported); the ones it would not open."""
        if not self.find() or not self.local:
            return ['%s %d' % (proto, port) for port, proto in ports]
        failed = []
        for port, proto in ports:
            lease = 0 if permanent else self.LEASE
            try:
                try:
                    self.add(port, proto, lease)
                except urllib.error.HTTPError as e:
                    if not lease or e.code != 500:  # a SOAP fault comes as 500
                        raise
                    lease = 0
                    self.add(port, proto, 0)
                self.mapped[(port, proto)] = lease
            except (OSError, ValueError):
                failed.append('%s %d' % (proto, port))
        with contextlib.suppress(OSError, ValueError):
            m = re.search(r'<NewExternalIPAddress>([^<]*)<', self.call('GetExternalIPAddress', []))
            if m:
                self.external = m.group(1).strip()
        return failed

    def close(self, ports=None):
        """Takes the forwards this manager made away again (all, or these of them); the ones the router would not
        take away (a leased one still ends by itself)."""
        keys = list(self.mapped) if ports is None else [k for k in ports if k in self.mapped]
        failed = []
        for port, proto in keys:
            try:
                if self.control:
                    self.call('DeletePortMapping', [('NewRemoteHost', ''), ('NewExternalPort', port), ('NewProtocol', proto)])
            except (OSError, ValueError):
                failed.append('%s %d' % (proto, port))
            self.mapped.pop((port, proto), None)
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
        self.ports_lock = threading.Lock()           # one router request round at a time (open, renew, close)
        self.ports_at = 0.0                          # when the router's forwards were last asked for
        self.keep_servers = False                    # quit with the servers left running (their forwards kept)
        self.countdowns = []                         # running countdowns
        self.sent = []                               # the last messages sent
        self.seq = int(time.time())
        self.install = {'state': 'idle', 'text': '', 'done': 0, 'total': 0}
        self.update = {'state': 'idle', 'latest': '', 'text': '', 'checked': 0, 'asset': '', 'progress': 0}
        self.quit = threading.Event()
        self.restart_after_quit = False
        os.makedirs(LOGS, exist_ok=True)
        # Moderation: the lists (read again when they change), the log, the players seen (moderation methods below).
        bans = bans_module()
        self.bans = bans.BanList(BANS_FILE, self.note)
        self.netbans = bans.BanList(NETWORK_BANS_FILE, self.note, network=True)
        self.modlog = bans.ModerationLog(MODERATION_LOG)
        self.seen = {}
        self.seen_saved = 0.0
        try:
            raw = json.loads(read_text(SEEN_FILE, '') or '{}')
            self.seen = {k: v for k, v in raw.items() if isinstance(v, dict)} if isinstance(raw, dict) else {}
        except ValueError:
            self.seen = {}
        # The dashboard: its history (90 days), the main backend's list as last asked, the public page's server.
        self.dash = dashboard_module().Dashboard(DASHBOARD_DIR, self.note)
        self.network_asked = 0.0
        self.public = None

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

    def host_key(self):
        """This host's own key (gmhost.host_proof), made once: the main backend knows the host by it."""
        path = os.path.join(DATA, 'host.key')
        if not os.path.exists(path):
            try:
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, 'w') as f:
                    f.write(os.urandom(32).hex())
            except FileExistsError:
                pass
        return path

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
                    '--log', os.path.join(LOGS, 'backend.log'), '--maintenance-file', MAINTENANCE_FILE]
            public = self.public_address()
            if mode == 'lan':
                args.append('--lan')
                if public:
                    args += ['--theater-host', public]
            else:
                if not os.path.exists(self.official_key()):
                    raise RuntimeError('This is the official server package, and official.key is not in %s. Copy it in '
                                       'from your own copy (it is never in a download).' % DATA)
                # The players' accounts (Protect your name) and the leaderboard: kept in this package's data folder, so
                # a restart of the backend loses neither.
                args += ['--theater-host', public or (lan_addresses() or ['127.0.0.1'])[0],
                         '--official-key-file', self.official_key(),
                         '--accounts', os.path.join(DATA, 'accounts.json'),
                         '--leaderboard', os.path.join(DATA, 'leaderboard.json'),
                         # the website's totals (server/stats.py): players who joined a match, hours played
                         '--stats', os.path.join(DATA, 'stats.json'),
                         '--player-stats', os.path.join(DATA, 'player_stats.json')]
                if self.cfg['required_version']:
                    args += ['--required-version', self.cfg['required_version']]
                elif self.cfg['version_gate']:
                    args += ['--version-feed', FEED]
                # The build check (16 Nov 2026): only official builds' community servers are listed. The official
                # builds' manifests (tools/build_manifest.py) go in the data folder's attest folder at each release.
                args += ['--check-servers', os.path.join(DATA, 'attest'),
                         '--blocked-file', os.path.join(DATA, 'blocked_servers.json'),
                         # Promoted community servers (manager.py promote): the Reborn tab, Trusted Community Servers.
                         '--promoted-file', PROMOTED_FILE,
                         # The Reborn network's ban list (the Moderation page's network list, manager.py ban --network)
                         '--network-bans', NETWORK_BANS_FILE]
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
        # Team balance: the server's own switches, and the file its moves go to for the host to tell the players.
        game += ['--bf_team_balance=%s' % ('true' if s['team_balance'] else 'false'),
                 '--bf_balance_gap=%d' % s['balance_gap'], '--bf_balance_min_players=%d' % s['balance_min_players'],
                 '--bf_balance_delay=%d' % s['balance_delay'],
                 '--bf_balance_block_stacking=%s' % ('true' if s['balance_block_stacking'] else 'false'),
                 '--bf_balance_events=' + os.path.join(directory, 'balance.txt'),
                 # Each player's stats (server_stats.cpp), for the host to send to the backend.
                 '--bf_stats_file=' + os.path.join(directory, 'stats.txt')]
        # The game simulates at the rate set here: the server's own, else the manager's.
        tick = s.get('tick') or self.cfg['tick']
        game.append('--bf_tick_rate=%d' % tick)
        # Frostbite's idle job threads look for work every 1 ms; woken at once by a job anyway, they look every 5 ms on
        # a server (14 Nov 2026: an empty Linux server 30% of a core before this and the SDK's own wait fixes, 11%
        # after; the tick's work the same under a live match).
        game.append('--job_wait_floor_ms=5')
        # More game flags for every server (tests and tuning): BF_SERVER_GAME_ARGS, separated by spaces.
        game += os.environ.get('BF_SERVER_GAME_ARGS', '').split()
        backend = (BACKEND_BIND or '127.0.0.1') if self.own_backend() else self.cfg['backend']
        host = [sys.executable, '-I', os.path.join(HERE, 'gmhost.py'), '--backend', backend + ':18250',
                '--listen', '0.0.0.0:%d' % s['port'], '--game-server', '%s:%d' % (address, GAME_PORT),
                '--name', s['name'].replace('"', "'"), '--map', MAP_TITLE[maps[0]],
                '--maps', ','.join(MAP_TITLE[m] for m in maps),
                '--map-file', os.path.join(directory, 'user', 'current_map.txt'),
                '--match-file', os.path.join(directory, 'user', 'match.txt'),
                '--status-file', os.path.join(directory, 'status.json'), '--log', os.path.join(directory, 'host.log'),
                '--max-players', str(s['max_players']), '--tick', str(tick),
                '--broadcast-file', os.path.join(directory, 'broadcast.json'),
                '--balance-file', os.path.join(directory, 'balance.txt'), '--host-key-file', self.host_key(),
                '--stats-file', os.path.join(directory, 'stats.txt'),
                # Moderation: the ban list and log this manager keeps for all its servers, and which one this is.
                '--bans-file', BANS_FILE, '--moderation-log', MODERATION_LOG, '--server-number', str(n)]
        if self.cfg['region']:
            host += ['--region', self.cfg['region']]
        if s['description']:
            host += ['--description', s['description'].replace('"', "'")]
        if not s['listed']:
            host.append('--unlisted')
        if s['night'] and self.flags['night_ops']:
            host.append('--night')
        host += ['--time', str(s['time']), '--day-minutes', str(s['day_minutes']),
                 '--cloud-cover', str(s['cloud_cover'] / 100), '--cloud-rate', str(s['cloud_rate'] / 100),
                 '--weather', str(s['weather'] / 100), '--fog', str(s['fog'] / 100),
                 '--ground-fog', str(s['ground_fog'] / 100), '--wind', str(s['wind'] / 100),
                 '--cloud-height', str(s['cloud_height'])]
        if s['day_night']:
            host.append('--day-night')
        if s['admins']:
            host += ['--admins', s['admins']]
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
            # The last run's log and error output kept for one more start (14 Nov 2026: a crash of the US server left
            # nothing behind, its log written over by the restart and its error output thrown away).
            for name in ('server.log', 'game.out'):
                with contextlib.suppress(OSError):
                    os.replace(os.path.join(directory, name), os.path.join(directory, name + '.prev'))
            state = self.run.setdefault(n, {'restarts': [], 'wait_until': 0})
            state['game'] = spawn(game, env=env, cwd=os.path.dirname(game_exe()), output=os.path.join(directory, 'game.out'))
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

    def wanted_ports(self):
        ports = [(s['port'], 'UDP') for s in self.cfg['servers']]
        if self.own_backend():
            ports += [(p, 'TCP') for p in BACKEND_PORTS]
        if self.cfg['public_page']['on']:
            ports.append((self.cfg['public_page']['port'], 'TCP'))  # the public stats page
        return ports

    def open_ports(self, quiet=False):
        """The router's ports (UPnP), where the settings ask for it; what could not be opened is said on the page.
        Forwards no longer wanted (a server removed, its port changed, UPnP turned off) are taken away; the wanted ones
        are asked for again, which also renews their lease (watch, every Upnp.RENEW)."""
        if not self.cfg['upnp']:
            self.ports_note = ''
            if self.upnp.mapped:
                threading.Thread(target=self.close_ports, args=('UPnP turned off',), daemon=True).start()
            return
        ports = self.wanted_ports()

        def work():
            with self.ports_lock:
                gone = [k for k in self.upnp.mapped if k not in ports]
                if gone:
                    self.upnp.close(gone)
                    self.note('router ports closed (no longer used): ' + ', '.join('%s %d' % (p, n) for n, p in gone))
                failed = self.upnp.open(ports)
                self.ports_at = time.time()
            note = ('Open these on your router: ' + ', '.join(failed)) if failed else 'Router ports opened (UPnP).'
            if not quiet or note != self.ports_note:
                self.ports_note = note
                self.note('router ports: ' + note)
        threading.Thread(target=work, daemon=True).start()

    def close_ports(self, why):
        """Takes away every router forward this manager made (when it closes, or UPnP is turned off)."""
        with self.ports_lock:
            if not self.upnp.mapped:
                return
            names = ', '.join('%s %d' % (p, n) for n, p in self.upnp.mapped)
            failed = self.upnp.close()
        self.note('router ports closed (%s): %s%s' % (why, names, ('; the router would not take away ' + ', '.join(failed) +
                                                                    ' (they end within an hour by themselves)') if failed else ''))

    def keep_ports(self):
        """Quit with the servers left running: their forwards are asked for for good, since nothing renews them now
        (the next manager start takes them over again and closes them when it closes)."""
        with self.ports_lock:
            if self.cfg['upnp'] and self.upnp.mapped:
                failed = self.upnp.open(list(self.upnp.mapped), permanent=True)
                self.note('router ports kept open for the servers left running%s' % (
                    (': could not keep ' + ', '.join(failed)) if failed else ''))

    def server_status(self, n):
        s = self.cfg['servers'][n - 1]
        state = self.run.get(n) or {}
        game_up = bool(state.get('game')) and state['game'].poll() is None
        host_up = bool(state.get('host')) and state['host'].poll() is None
        out = {'n': n, 'name': s['name'], 'port': s['port'], 'wanted': bool(state.get('wanted')), 'state': 'stopped',
               'players': 0, 'names': [], 'map': '', 'next_map': '', 'why': state.get('why', ''), 'reachable': None,
               'reachable_local': None, 'address': '', 'since': state.get('since', 0), 'listed': s['listed'],
               'official': False, 'hidden': ''}
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
        for key in ('players', 'map', 'next_map', 'reachable', 'reachable_local', 'address', 'official', 'hidden'):
            if key in status:
                out[key] = status[key]
        if isinstance(status.get('names'), list):
            out['names'] = [str(x)[:32] for x in status['names']][:64]
        if isinstance(status.get('roster'), list):  # moderation: each player's id, address and account (gmhost.py)
            bans = bans_module()
            pings = status.get('pings') if isinstance(status.get('pings'), dict) else {}
            out['roster'] = [{'name': bans.clean_name(r.get('name')), 'id': bans.clean_id(r.get('id')),
                              'ip': bans.clean_ip(r.get('ip')), 'account': bans.clean_account(r.get('account')),
                              'since': int(r.get('since') or 0),
                              'ping': pings.get(r.get('name')) if isinstance(pings.get(r.get('name')), int) else None}
                             for r in status['roster'][:128] if isinstance(r, dict)]
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
                    self.dash.note_restart(n, what)
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
                with contextlib.suppress(Exception):
                    self.note_seen()
                try:
                    self.dash_tick()
                except Exception as e:  # never the watch loop for the dashboard
                    if time.time() - getattr(self, 'dash_error_at', 0) > 600:
                        self.dash_error_at = time.time()
                        self.note('dashboard: %s: %s' % (type(e).__name__, e))
                # The router's forwards are leased (Upnp.LEASE): asked for again before they end.
                if self.cfg['upnp'] and any(self.upnp.mapped.values()) and time.time() - self.ports_at > Upnp.RENEW:
                    self.ports_at = time.time()
                    self.open_ports(quiet=True)
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
                     json.dumps({'seq': seq, 'kind': kind if kind in KINDS else 'custom', 'text': text,
                                 'seconds': 10}))

    def running_servers(self):
        with self.lock:
            return sorted(n for n, s in self.run.items() if s.get('wanted') and n <= len(self.cfg['servers']))

    def broadcast(self, kind, seconds, text='', servers=None, restart=False, update=False, network=''):
        """A message now; with a time, a countdown: said now, said again as the time runs out, and at its end the
        servers are restarted (`restart`) or the update is installed (`update`). network "official" or "everyone":
        through the main backend to every official server, or to every server registered with it (send_network);
        such a message only tells, it never restarts or updates anything."""
        kind = kind if kind in KINDS else 'custom'
        if network:
            if network not in NETWORK_SCOPES:
                raise RuntimeError('Send to "official" or "everyone".')
            if servers:
                raise RuntimeError('A network broadcast goes to every server: leave out the server number.')
            official_key_or_fail('A broadcast to other machines\' servers')
            restart = update = False
        targets = [] if network else [n for n in (servers or self.running_servers()) if n in self.running_servers()]
        template = (NETWORK_PRESETS if network else PRESETS).get(kind) or clean_text(text, 200)
        if not template:
            raise RuntimeError('Type the message first.')
        if kind == 'maintenance':
            restart = False  # it only tells; the restart itself is yours
        if not targets and not update and not network:
            raise RuntimeError('No server is running: there is nobody to tell.')
        seconds = max(0, min(24 * 3600, int(seconds)))
        if '{time}' not in template or seconds <= 0:
            if '{time}' in template:
                raise RuntimeError('Pick how long until it happens.')
            if network:
                self.say_network(kind, template, network, raise_errors=True)
                return None
            for n in targets:
                self.say(n, kind, template)
            self.remember(kind, template, targets)
            return None
        countdown = {'id': secrets.token_hex(4), 'kind': kind, 'template': template, 'servers': targets,
                     'end': time.time() + seconds, 'restart': bool(restart), 'update': bool(update),
                     'marks': [m for m in ANNOUNCE_AT if m < seconds], 'seconds': seconds, 'network': network}
        if network:  # the first one now, so a refusal is said at once and no countdown is left running
            self.say_network(kind, template.replace('{time}', time_words(seconds)), network, raise_errors=True)
            with self.lock:
                self.countdowns.append(countdown)
            return countdown['id']
        with self.lock:
            self.countdowns.append(countdown)
        self.announce(countdown, seconds)
        return countdown['id']

    def announce(self, countdown, left):
        text = countdown['template'].replace('{time}', time_words(left))
        if countdown.get('network'):
            self.say_network(countdown['kind'], text, countdown['network'])
            return
        for n in countdown['servers']:
            self.say(n, countdown['kind'], text)
        self.remember(countdown['kind'], text, countdown['servers'])

    def say_network(self, kind, text, scope, raise_errors=False):
        """One network broadcast (send_network), noted with what the backend says it reached."""
        try:
            reply = send_network(kind, text, scope)
        except RuntimeError as e:
            self.note('network message to %s NOT sent: %s (%s)' % (scope, e, text))
            if raise_errors:
                raise
            return
        sent = reply.get('sent') or {}
        self.remember(kind, text, [], '%s: %d official and %d community servers' % (
            'every official server' if scope == 'official' else 'everyone', sent.get('official', 0),
            sent.get('community', 0)))

    def remember(self, kind, text, servers, network=''):
        with self.lock:
            self.sent.append({'at': int(time.time()), 'kind': kind, 'text': text, 'servers': list(servers),
                              'network': network})
            del self.sent[:-50]
        if network:
            self.note('network message to %s: %s' % (network, text))
            return
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

    # ---- moderation (server/bans.py, docs/MODERATION.md)
    def network_admin(self):
        """True on the machine that runs the main backend: its manager keeps the Reborn network's ban list."""
        return self.own_backend() == 'official'

    def roster(self):
        out = []
        for n in range(1, len(self.cfg['servers']) + 1):
            st = self.server_status(n)
            for r in st.get('roster', []):
                out.append(dict(r, server=n, server_name=st['name']))
        return out

    def note_seen(self):
        """The players on the servers now, into the week's list (the page's lookup)."""
        now = int(time.time())
        changed = False
        for r in self.roster():
            if not r['name']:
                continue
            key = '%s|%s' % (r['name'].lower(), r['id'])
            old = self.seen.get(key)
            if not old or now - old.get('at', 0) > 60 or old.get('ip') != r['ip']:
                changed = True
            self.seen[key] = {'name': r['name'], 'id': r['id'], 'ip': r['ip'], 'account': r['account'],
                              'server': r['server_name'], 'at': now}
        for key in [k for k, v in self.seen.items() if now - int(v.get('at', 0)) > SEEN_DAYS * 86400]:
            del self.seen[key]
            changed = True
        if len(self.seen) > 5000:
            for key in sorted(self.seen, key=lambda k: self.seen[k].get('at', 0))[:500]:
                del self.seen[key]
        if changed and time.time() - self.seen_saved > 30:
            self.seen_saved = time.time()
            with contextlib.suppress(OSError):
                write_atomic(SEEN_FILE, json.dumps(self.seen))

    def moderation(self):
        """What the Moderation page shows."""
        bans = bans_module()
        self.bans.reload(force=True)
        self.netbans.reload(force=True)
        now = time.time()
        return {'players': self.roster(),
                'bans': [self.bans.public(e) for e in self.bans.active(now)],
                'network_admin': self.network_admin(),
                'network': [self.netbans.public(e) for e in self.netbans.active(now)] if self.network_admin() else [],
                'log': self.modlog.read(200), 'servers': [{'n': n, 'name': x['name']} for n, x in
                                                         enumerate(self.cfg['servers'], 1)],
                'kick_minutes': bans.KICK_SECONDS // 60, 'ip_days': bans.IP_DAYS, 'reason_max': bans.REASON_MAX,
                'now': int(now)}

    def lookup(self, text, network=False):
        """Players by name (any part) or player id: this manager's week (its servers), or with network the main
        backend's (who is online, names seen this week), asked with a signed question."""
        bans = bans_module()
        text = str(text or '').strip()[:32]
        if network:
            if not self.network_admin():
                raise RuntimeError('The Reborn network\'s list is kept on the machine that runs the main backend.')
            key = official_key_or_fail('The network lookup')
            reply = ask_backend(host_module().sign_admin(key, 'lookup:' + text if text else 'players'))
            if not reply.get('ok'):
                raise RuntimeError('The main backend refused the question: %s.' % (reply.get('why') or 'no reason'))
            return [{'name': bans.clean_name(r.get('name')), 'id': bans.clean_id(r.get('id')),
                     'ip': bans.clean_ip(r.get('ip')), 'account': bans.clean_account(r.get('account')),
                     'server': str(r.get('server', ''))[:48], 'at': int(r.get('at') or 0),
                     'online': bool(r.get('online', True))} for r in reply.get('players') or []][:200]
        self.note_seen()
        low = text.lower()
        rows = [dict(v) for v in self.seen.values()
                if not low or low in v['name'].lower() or bans.clean_id(text) == v.get('id')]
        online = {(r['name'].lower(), r['id']) for r in self.roster()}
        for r in rows:
            r['online'] = (r['name'].lower(), r['id']) in online
        return sorted(rows, key=lambda r: -r.get('at', 0))[:200]

    def moderate(self, data, by_default='manager'):
        """A kick, ban, mute or unban, from the page or the command line. Returns the entry."""
        bans = bans_module()
        action = str(data.get('action', ''))
        network = data.get('list') == 'network'
        if network and not self.network_admin():
            raise RuntimeError('The Reborn network\'s ban list is kept on the machine that runs the main backend '
                               '(the official manager).')
        target = self.netbans if network else self.bans
        by = bans.clean_reason(data.get('by'))[:32] or by_default
        try:
            if action == 'unban':
                e = target.remove(str(data.get('ban', ''))[:16])
                self.modlog.write(action='unban', list='network' if network else 'server', ban=e['id'], kind=e['kind'],
                                  names=e['names'], ids=e['ids'], by=by)
                self.note('moderation: %s removed %s %s (%s)' % (by, e['kind'], e['id'], ', '.join(e['names']) or
                                                                 ', '.join(e['ids'])))
                return e
            if action not in ('kick', 'ban', 'mute'):
                raise ValueError('the action is kick, ban, mute or unban')
            if network and action != 'ban':
                raise ValueError('the network list holds bans only: kick and mute are for this manager\'s servers')
            p = data.get('player') if isinstance(data.get('player'), dict) else {}
            ids = [p.get('id')] + list(data.get('ids') or [])
            accounts = [p.get('account')] + list(data.get('accounts') or [])
            ips = [p.get('ip')] + list(data.get('ips') or []) if data.get('match_ip', True) else []
            seconds = 0 if action == 'kick' else bans.length_seconds(data.get('length') or 'permanent')
            servers = [] if network else [int(x) for x in (data.get('servers') or []) if str(x).isdigit()]
            e = target.add(action, ids, accounts, ips, [p.get('name')] + list(data.get('names') or []),
                           data.get('reason', ''), by, seconds, servers)
        except ValueError as err:
            raise RuntimeError(str(err).capitalize() + '.')
        self.modlog.write(action=action, list='network' if network else 'server', ban=e['id'], names=e['names'],
                          ids=e['ids'], account=bool(e['accounts']) or None, ip=bool(e['ips']) or None,
                          reason=e['reason'], until=e['until'] or None, servers=e['servers'] or None, by=by)
        self.note('moderation: %s %s %s (%s) %s: %s' % (
            by, {'kick': 'kicked', 'ban': 'banned', 'mute': 'muted'}[action],
            ', '.join(e['names']) or ', '.join(e['ids']) or 'an address', e['id'],
            'on the Reborn network' if network else 'on server%s %s' % ('s' if len(e['servers']) != 1 else '',
                                                                          ', '.join(map(str, e['servers'])) or 'all'),
            e['reason'] or 'no reason'))
        return e

    # ---- the dashboard (server/dashboard.py, docs/DASHBOARD.md)
    def dash_tick(self):
        """Every turn of the watch loop: each server's state, players and processes into the dashboard."""
        servers, procs, folders = [], {}, {}
        for n in range(1, len(self.cfg['servers']) + 1):
            st = self.server_status(n)
            s = self.cfg['servers'][n - 1]
            rnd = None
            with contextlib.suppress(OSError, ValueError, IndexError):
                parts = read_text(os.path.join(self.server_dir(n), 'user', 'match.txt'), '').split()
                if len(parts) >= 2:
                    rnd = {'round': int(parts[0]), 'left': round(float(parts[1]), 3)}
            servers.append(dict(st, max=s['max_players'], tick=s.get('tick') or self.cfg['tick'],
                                region=self.cfg['region'], round=rnd if st['state'] == 'running' else None))
            run = self.run.get(n) or {}
            procs[n] = {role: run[role].pid for role in ('game', 'host')
                        if run.get(role) is not None and run[role].poll() is None}
            folders[n] = self.server_dir(n)
        self.dash.tick(servers, procs, folders=folders)
        if time.time() - self.network_asked > 60:
            self.network_asked = time.time()
            threading.Thread(target=self.ask_network, daemon=True).start()

    def ask_network(self):
        """The main backend's server list (every Reborn and community server listed now), for the dashboard: names,
        players, maps and regions, never addresses."""
        if self.cfg['lan_only']:
            self.dash.network = None
            return
        host = (BACKEND_BIND or '127.0.0.1') if self.own_backend() else self.cfg['backend']
        try:
            with socket.create_connection((host, REGISTRY_PORT), timeout=8) as sock:
                sock.sendall((json.dumps({'op': 'list', 'version': self.version, 'platform': 'windows',
                                          'notice': 1}) + '\n').encode())
                reply = json.loads(sock.makefile('rb').readline() or b'{}')
        except (OSError, ValueError):
            return
        rows = []
        for g in reply.get('servers') or []:
            if not isinstance(g, dict) or str(g.get('gid', '')).startswith('update-'):
                continue
            rows.append({'name': clean_text(g.get('name'), 48), 'players': int(g.get('players') or 0),
                         'max': int(g.get('max_players') or 0), 'map': clean_text(g.get('map'), 32),
                         'region': clean_text(g.get('region'), 16), 'tick': int(g.get('tick') or 0),
                         'official': bool(g.get('official')), 'promoted': bool(g.get('promoted')),
                         'description': clean_text(g.get('description'), 120)})
        rows.sort(key=lambda r: (not r['promoted'], not r['official'], -r['players'], r['name'].lower()))
        self.dash.network = {'servers': rows[:100], 'players': sum(r['players'] for r in rows),
                             'online': int(reply.get('players_online') or 0), 'at': int(time.time())}

    def dashboard(self, public=False):
        view = self.dash.view(public=public, names=not public or self.cfg['public_page']['names'])
        view['title'] = self.cfg['public_page']['title'] or (self.cfg['servers'][0]['name'] if self.cfg['servers'] else '')
        view['public_page'] = dict(self.cfg['public_page'])
        view['page'] = self.community_page()
        if not public:
            view['alerts'] = self.alerts(view)
        return view

    def community_page(self):
        """What the community page shows besides the numbers: the owner's words, look, links and how to join (only
        what the owner set; the address only when the owner leaves it on)."""
        p = self.cfg['public_page']
        links = []
        if p['links']['download']:
            links.append({'label': 'Download Battlefield 1943 Reborn', 'url': PROJECT_DOWNLOAD, 'kind': 'download'})
        if p['links']['discord']:
            links.append({'label': 'Battlefield 1943 Reborn Discord', 'url': PROJECT_DISCORD, 'kind': 'discord'})
        links += [dict(link, kind='custom') for link in p['links']['custom']]
        joins = []
        host = p['connect']['custom_address'] or self.public_address() or ''
        for n, s in enumerate(self.cfg['servers'], 1):
            st = self.server_status(n)
            address = ''
            if p['connect']['address']:
                if p['connect']['custom_address']:
                    address = p['connect']['custom_address'] if len(self.cfg['servers']) == 1 or ':' in host \
                        else '%s:%d' % (host, s['port'])
                elif st.get('address') and not st['address'].startswith('127.'):
                    address = st['address']  # where the main backend saw it register
                elif host:
                    address = '%s:%d' % (host, s['port'])
            joins.append({'name': s['name'], 'state': st['state'], 'players': st['players'], 'max': s['max_players'],
                          'address': address, 'listed': s['listed'], 'lan': self.cfg['lan_only']})
        logo_at = 0
        if p['logo']:
            with contextlib.suppress(OSError):
                logo_at = int(os.path.getmtime(PUBLIC_LOGO + '.' + p['logo']))
        return {'title': p['title'], 'description': p['description'], 'theme': p['theme'], 'accent': p['accent'],
                'logo': 'logo?v=%d' % logo_at if logo_at else '', 'sections': p['sections'], 'links': links,
                'join': joins, 'instructions': p['connect']['instructions'], 'rules': p['rules'],
                'project': PROJECT_SITE, 'css': p['custom_css']}

    def site_info(self):
        """The owner's own site folder for the Community page tab: where it is, whether it has an index.html, its files."""
        files = []
        for root, dirs, names in os.walk(COMMUNITY_SITE):
            dirs[:] = [d for d in dirs if not d.startswith('.')]
            for name in names:
                if not name.startswith('.'):
                    files.append(os.path.relpath(os.path.join(root, name), COMMUNITY_SITE).replace(os.sep, '/'))
            if len(files) > 200:
                break
        return {'folder': COMMUNITY_SITE, 'ready': os.path.isfile(os.path.join(COMMUNITY_SITE, 'index.html')),
                'files': sorted(files)[:200]}

    def make_starter(self):
        """A starting point for the owner's own site: our page as index.html, and a README of the numbers. Files
        already there are kept."""
        os.makedirs(COMMUNITY_SITE, exist_ok=True)
        made = []
        page = read_text(os.path.join(UI, 'public.html'))
        page = page.replace('<script src="dash.js"></script>', '<script src="/dash.js"></script>').replace(
            "fetch('stats.json'", "fetch('/stats.json'")
        header = ('<!-- Your own community page (docs/DASHBOARD.md). Change anything: this folder is served as your site, '
                  'with index.html at its root.\n     Your page reads the live numbers from /stats.json; /dash.js (our '
                  'charts) and /logo (your logo) are there to use too. -->\n')
        for name, text in (('index.html', header + page), ('README.txt', STARTER_README)):
            path = os.path.join(COMMUNITY_SITE, name)
            if not os.path.exists(path):
                write_atomic(path, text)
                made.append(name)
        self.note('community page: starter files in %s (%s)' % (COMMUNITY_SITE, ', '.join(made) or 'kept what was there'))
        return made

    def set_logo(self, data):
        """The community page's logo: a PNG, JPEG, WebP or GIF of 1 MB at most (its own bytes say which), or none."""
        import base64
        if data.get('remove'):
            kind = ''
        else:
            try:
                raw = base64.b64decode(str(data.get('data', '')).split(',', 1)[-1], validate=True)
            except (ValueError, TypeError):
                raise RuntimeError('That picture could not be read.')
            if len(raw) > PUBLIC_LOGO_MOST:
                raise RuntimeError('A logo is 1 MB at most.')
            kind = 'png' if raw.startswith(b'\x89PNG\r\n\x1a\n') else 'jpg' if raw.startswith(b'\xff\xd8\xff') else \
                'gif' if raw[:6] in (b'GIF87a', b'GIF89a') else 'webp' if raw[:4] == b'RIFF' and raw[8:12] == b'WEBP' else ''
            if not kind:
                raise RuntimeError('A logo is a PNG, JPEG, WebP or GIF picture.')
        for ext in PUBLIC_LOGO_TYPES:
            with contextlib.suppress(OSError):
                os.remove(PUBLIC_LOGO + '.' + ext)
        if kind:
            write_atomic_bytes(PUBLIC_LOGO + '.' + kind, raw)
        page = dict(self.cfg['public_page'], logo=kind)
        self.set_config({'public_page': page})
        self.note('community page: logo %s' % ('set (%s, %d KB)' % (kind, len(raw) // 1024) if kind else 'removed'))

    def logo_file(self):
        kind = self.cfg['public_page']['logo']
        path = PUBLIC_LOGO + '.' + kind if kind else ''
        return (path, PUBLIC_LOGO_TYPES[kind]) if path and os.path.exists(path) else (None, None)

    def alerts(self, view):
        """What needs the owner's eye, in a few words each."""
        out = []
        for sv in view['servers']:
            if sv.get('wanted') and sv['state'] in ('waiting', 'restarting'):
                out.append(('bad', '%s is down: %s' % (sv['name'], sv.get('why') or 'starting again')))
            if sv['state'] == 'running' and sv.get('reachable') is False:
                out.append(('bad', '%s cannot be reached from the internet: open its UDP port on your router.' % sv['name']))
            if sv['state'] == 'running' and sv.get('held') and sv.get('tick') and sv['held'] < sv['tick'] * 0.9:
                out.append(('warn', '%s held %.1f of its %d ticks a second: the machine is too busy for it.' % (
                    sv['name'], sv['held'], sv['tick'])))
        if view['cards']['restarts24']:
            out.append(('warn', '%d server restart%s in the last 24 hours after a game or host ended (the Logs page '
                                'has them).' % (view['cards']['restarts24'], '' if view['cards']['restarts24'] == 1 else 's')))
        cpu, mem, total = view['machine'].get('cpu'), view['machine'].get('mem'), view['machine'].get('mem_total')
        if cpu is not None and cpu > 90:
            out.append(('warn', 'The machine\'s CPU is at %d%%.' % cpu))
        if mem and total and mem > total * 0.92:
            out.append(('warn', 'The machine\'s memory is %d%% used.' % (100 * mem / total)))
        with contextlib.suppress(OSError):
            free = shutil.disk_usage(DATA).free
            if free < 2 << 30:
                out.append(('warn', 'Only %.1f GB of disk space is left for the logs and the game.' % (free / (1 << 30))))
        if self.update.get('state') in ('available', 'downloaded'):
            out.append(('info', 'Version %s is out (Updates).' % self.update.get('latest')))
        return [{'level': a, 'text': b} for a, b in out]

    def public_page(self):
        """The public stats page: started, stopped or moved to follow the settings."""
        want = self.cfg['public_page']
        if self.public and (not want['on'] or self.public.server_port != want['port']):
            server, self.public = self.public, None
            threading.Thread(target=server.shutdown, daemon=True).start()
            server.server_close()
            self.note('public stats page stopped')
        if want['on'] and not self.public:
            try:
                self.public = PublicServer((PUBLIC_BIND or '0.0.0.0', want['port']), PublicHandler)
            except OSError as e:
                self.note('public stats page: port %d could not be opened (%s)' % (want['port'], e))
                return
            self.public.manager = self
            threading.Thread(target=self.public.serve_forever, daemon=True).start()
            self.note('public stats page on port %d (TCP)' % want['port'])
            self.open_ports(quiet=True)

    # ---- what the page shows
    def state(self):
        with self.lock:
            cfg = json.loads(json.dumps(self.cfg))
            cfg['web_password'] = bool(cfg['web_password'])
            servers = [self.server_status(n) for n in range(1, len(self.cfg['servers']) + 1)]
            countdowns = [{'id': c['id'], 'kind': c['kind'], 'servers': c['servers'], 'left': max(0, int(c['end'] - time.time())),
                           'restart': c['restart'], 'update': c['update'], 'network': c.get('network', ''),
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
                'presets': PRESETS, 'network_presets': NETWORK_PRESETS,
                'network': self.flags['official'] and os.path.exists(self.official_key()), 'tick': self.cfg['tick'], 'data': DATA, 'root': ROOT, 'max_servers': MAX_SERVERS,
                'backend_ports': list(BACKEND_PORTS), 'now': int(time.time()),
                'maintenance': self.maintenance_line(),
                'autostart': autostart_enabled(),
            }

    def maintenance_line(self):
        """The switch for the page: never a page that fails because plasma.py could not be loaded."""
        try:
            return maintenance_words()
        except Exception as e:  # noqa: BLE001 (a plasma.py that does not load: said, the rest of the page works)
            return 'maintenance: not known (%s: %s)' % (type(e).__name__, e)

    def set_config(self, new):
        """The settings from the page: checked, kept, saved. What a running server reads only at its start (its name,
        its maps, the region, the mode) applies when it is started again: the page says so."""
        with self.lock:
            merged = dict(self.cfg)
            for key in ('servers', 'region', 'tick', 'lan_only', 'public_address', 'upnp', 'auto_update',
                        'update_warning_minutes', 'start_servers', 'version_gate', 'required_version', 'web_remote',
                        'public_page'):
                if key in new:
                    merged[key] = new[key]
            if isinstance(new.get('public_page'), dict) and 'logo' not in new['public_page']:
                merged['public_page'] = dict(new['public_page'], logo=self.cfg['public_page']['logo'])  # (set_logo's)
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
        self.public_page()  # the public stats page on, off or moved
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
        if path == '/dash.js':  # the dashboard's drawing, shared with the public stats page
            return self.send(200, read_text(os.path.join(UI, 'dash.js')), 'text/javascript')
        if path in ('/preview', '/preview/', '/preview/dash.js', '/preview/stats.json', '/preview/logo'):
            # The community page as players would see it (Preview on the Community page tab), signed in only.
            if not self.allowed():
                return self.send(401, {'error': 'Sign in first.'})
            if path == '/preview':
                return self.send(302, b'', headers=[('Location', '/preview/')])
            if path == '/preview/':
                return self.send(200, read_text(os.path.join(UI, 'public.html')), 'text/html')
            if path == '/preview/dash.js':
                return self.send(200, read_text(os.path.join(UI, 'dash.js')), 'text/javascript')
            if path == '/preview/stats.json':
                return self.send(200, m.dashboard(public=True))
            logo, kind = m.logo_file()
            return self.send(200, open(logo, 'rb').read(), kind) if logo else self.send(404, {'error': 'no logo'})
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
        if path == '/api/moderation':
            return self.send(200, m.moderation())
        if path == '/api/dashboard':
            return self.send(200, m.dashboard())
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
        elif path == '/api/moderation/action':
            e = m.moderate(data, 'command line' if self.headers.get('X-BF-Token') else 'manager page')
            return {'entry': m.bans.public(e), 'moderation': m.moderation()}
        elif path == '/api/public/logo':
            m.set_logo(data)
        elif path == '/api/public/starter':
            return {'made': m.make_starter(), 'site': m.site_info()}
        elif path == '/api/public/site':
            return {'site': m.site_info()}
        elif path == '/api/moderation/lookup':
            return {'players': m.lookup(data.get('text', ''), bool(data.get('network')))}
        elif path == '/api/broadcast':
            servers = [int(x) for x in data.get('servers') or []]
            ident = m.broadcast(str(data.get('kind', 'custom')), int(data.get('seconds') or 0), str(data.get('text', '')),
                                servers or None, bool(data.get('restart')), network=str(data.get('network') or ''))
            return {'countdown': ident}
        elif path == '/api/maintenance':
            if data.get('on') and not m.own_backend():
                raise RuntimeError('This machine runs no backend of its own: maintenance mode is switched on the '
                                   'machine that runs the main backend.')
            words = set_maintenance(bool(data.get('on')), str(data.get('kind', '')), str(data.get('text', '')),
                                    data.get('minutes') or 0, data.get('allow') or [])
            m.note(words)
            return {'maintenance': words}
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
            m.keep_servers = bool(data.get('keep_servers'))
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


STARTER_README = """Your own Battlefield 1943 Reborn community page
=============================================

This folder is your page's whole website when "Use my own website" is on (the manager's Community page tab).
index.html is its front page; add any .html, .css, .js, pictures, fonts or videos beside it, in folders if you like.
Files and folders whose names start with a dot are never served.

Three addresses always work on your page, whatever is in this folder:

  /stats.json   the live numbers, made new every 5 seconds (fetch it from your page's script)
  /dash.js      our dashboard's drawing: Dash.render(element, stats, {public: true, sections: {...}})
  /logo         the logo set on the Community page tab (404 when none)

/stats.json holds:
  title                     your page's name
  now                       the time (seconds since 1970, UTC)
  page                      your Community page settings: description, theme, accent, logo, sections, rules, css,
                            links [{label, url, kind}], join [{name, state, players, max, address, listed, lan}],
                            instructions, project
  cards                     online, slots, peak24, hours24, sessions24, tracked, mvp {name, seconds},
                            top_killer {name, kills}, restarts24
  servers                   [{n, name, state, map, next_map, players, max, tick, held, ping_avg, round {round, left},
                            since, region, roster [{name, ping, since}]}]
  series                    {"6h", "24h", "7d"}: [[time, players in all, {server number: players}]]
  maps                      {map: player-minutes over 7 days}
  heatmap                   7 weekdays (Monday first) x 24 hours (UTC): average players, null without data
  leaderboard               [{name, seconds, sessions, kills, deaths, kd, captures, map, last}] (90 days)
  events                    [{t, kind (join, leave, map, restart, server_up, server_down), name, server, map, played}]
  machine                   {cpu, mem, mem_total}
  network                   the Reborn server list: {players, online, servers [{name, players, max, map, region,
                            tick, official, promoted, description}]}
Names are left out (empty) when "Show player names" is off. Player ids and players' addresses are never in it.

docs/DASHBOARD.md in the project has the rest.
"""


class PublicServer(Server):
    """The public stats page (docs/DASHBOARD.md): its own port, open to the internet when the owner turns it on."""
    manager = None


class PublicHandler(http.server.BaseHTTPRequestHandler):
    """Read only: the page and its numbers, nothing else (never the manager's own page or anything it does). What it
    shows is the dashboard's public view: names when the owner allows them, never a player id, an address, a ban or a
    path on this machine. At most 120 requests a minute from one address; the numbers are made once each 5 s."""
    server_version = 'BF1943Reborn'
    sys_version = ''
    hits = {}
    cache = [0.0, b'']
    lock = threading.Lock()

    def log_message(self, fmt, *args):
        pass

    OUR_POLICY = ("default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data:; "
                  "connect-src 'self'")
    # The owner's own site may use web fonts, pictures and libraries from elsewhere (their choice, their page).
    SITE_POLICY = ("default-src 'self' https: data:; script-src 'self' 'unsafe-inline' https:; "
                   "style-src 'self' 'unsafe-inline' https:; img-src 'self' https: data:; font-src 'self' https: data:; "
                   "connect-src 'self' https:; media-src 'self' https:; frame-ancestors 'self'")

    def site_file(self, path):
        """(the file, its type) of the owner's site for this path, or (None, None). Inside the folder only: no "..",
        no hidden files, known types only, 20 MB at most."""
        rel = urllib.parse.unquote(path).lstrip('/')
        if rel == '' or rel.endswith('/'):
            rel += 'index.html'
        parts = rel.split('/')
        if any(not x or x in ('.', '..') or x.startswith('.') or '\\' in x or ':' in x or '\0' in x for x in parts):
            return None, None
        root = os.path.realpath(COMMUNITY_SITE)
        full = os.path.realpath(os.path.join(root, *parts))
        kind = SITE_TYPES.get(os.path.splitext(full)[1].lower())
        if not full.startswith(root + os.sep) or not kind or not os.path.isfile(full) or os.path.getsize(full) > SITE_FILE_MOST:
            return None, None
        return full, kind

    def reply(self, code, body, kind, policy=None):
        data = body if isinstance(body, bytes) else body.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', kind + ('; charset=utf-8' if kind.startswith('text/') or kind.endswith('json') else ''))
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('X-Frame-Options', 'SAMEORIGIN')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', policy or self.OUR_POLICY)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(data)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        now = time.time()
        with PublicHandler.lock:
            ip = self.client_address[0]
            times = [t for t in PublicHandler.hits.get(ip, []) if now - t < 60]
            if len(PublicHandler.hits) > 5000:
                PublicHandler.hits.clear()
            PublicHandler.hits[ip] = times + [now]
        if len(times) >= 120:
            return self.reply(429, 'Too many requests.', 'text/plain')
        path = self.path.partition('?')[0]
        own = self.server.manager.cfg['public_page']['own_site'] and \
            os.path.isfile(os.path.join(COMMUNITY_SITE, 'index.html'))
        if own and path not in ('/dash.js', '/logo', '/stats.json'):
            # The owner's own site in place of ours; the numbers, our charts and the logo stay where they are.
            full, kind = self.site_file(path)
            if not full:
                return self.reply(404, 'Not found.', 'text/plain', self.SITE_POLICY)
            with open(full, 'rb') as f:
                return self.reply(200, f.read(), kind, self.SITE_POLICY)
        if path in ('/', '/index.html'):
            page = read_text(os.path.join(UI, 'public.html'))
            return self.reply(200 if page else 500, page or 'The page is missing.', 'text/html')
        if path == '/dash.js':
            return self.reply(200, read_text(os.path.join(UI, 'dash.js')), 'text/javascript')
        if path == '/logo':  # the community page's logo, when the owner gave one
            logo, kind = self.server.manager.logo_file()
            if not logo:
                return self.reply(404, 'Not found.', 'text/plain')
            with open(logo, 'rb') as f:
                return self.reply(200, f.read(), kind)
        if path == '/stats.json':
            with PublicHandler.lock:
                if now - PublicHandler.cache[0] > 5:
                    PublicHandler.cache[:] = [now, json.dumps(self.server.manager.dashboard(public=True)).encode()]
                body = PublicHandler.cache[1]
            return self.reply(200, body, 'application/json')
        return self.reply(404, 'Not found.', 'text/plain')


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
    m.public_page()  # the public stats page, when it is on
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
    # The router's forwards (UPnP) go with the manager, unless servers are really left running (quit --keep-servers;
    # 16 Nov 2026: the stop_all above still stops them, so today they always go).
    with m.lock:
        alive = any(s.get(k) and s[k].poll() is None for s in m.run.values() for k in ('game', 'host'))
    if alive and not restart:
        m.keep_ports()
    else:
        m.close_ports('an update restarts the manager' if restart else 'the manager closed')
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


def moderation_target(who, network=False):
    """The player the command line names: one on the servers now (by name or player id), one seen this week, or else
    a player id typed in full."""
    bans = bans_module()
    rows = ask('/api/moderation')['players']
    want = who.strip().lower()
    hit = [r for r in rows if r['name'].lower() == want or (bans.clean_id(who) and r['id'] == bans.clean_id(who))]
    if not hit:
        hit = [r for r in ask('/api/moderation/lookup', {'text': who, 'network': network})['players']
               if r['name'].lower() == want or (bans.clean_id(who) and r['id'] == bans.clean_id(who))]
    if hit:
        r = hit[0]
        return {'name': r['name'], 'id': r['id'], 'ip': r['ip'], 'account': r.get('account', '')}
    if bans.clean_id(who) and len(who.strip()) == 8:
        return {'id': bans.clean_id(who)}
    raise RuntimeError('No player %s is on the servers or was seen this week. Give their 8 digit player id, or an '
                       'address with --ip.' % who)


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
    b.add_argument('--kind', choices=list(KINDS), default='custom')
    b.add_argument('--minutes', type=float, default=0)
    b.add_argument('--text', default='')
    b.add_argument('--server', type=int, action='append', default=[])
    b.add_argument('--no-restart', action='store_true', help='a restart or update countdown that only tells the players')
    b.add_argument('--to', choices=list(NETWORK_SCOPES), default='',
                   help='official package only: through the main backend to every official server (official), or to '
                        'every server registered with it, community ones too (everyone). Without it: this machine\'s '
                        'own servers, as before')
    pr = sub.add_parser('promote', help='a community host\'s servers listed with the Reborn ones, as a Trusted '
                                        'Community Server')
    pr.add_argument('key', help='its host key (manager.py community lists them), or the start of it (8 or more)')
    pr.add_argument('--note', default='', help='a note for you (default: its servers\' names)')
    de = sub.add_parser('demote', help='a promoted host back to the Community tab')
    de.add_argument('key', help='its host key, or the start of it (8 or more characters)')
    sub.add_parser('promoted', help='the promoted hosts')
    sub.add_parser('community', help='the community servers registered with the main backend now, with host keys')
    mt = sub.add_parser('maintenance', help='maintenance mode: new joins refused with a message the players see')
    mt.add_argument('action', choices=['on', 'off', 'status'], nargs='?', default='status')
    mt.add_argument('--preset', choices=['deploy', 'update', 'restart', 'emergency', 'custom'], default='',
                    help='the message (default: deploy, or custom with --text)')
    mt.add_argument('--text', default='', help='a custom message, at most 200 characters')
    mt.add_argument('--minutes', type=float, default=0, help='switches itself off after this long (0: never)')
    mt.add_argument('--allow', action='append', default=[], help='a player address let in anyway (an admin)')
    # Moderation (docs/MODERATION.md)
    sub.add_parser('players', help='the players on this manager\'s servers now, with their player ids')
    for name, words in (('kick', 'removes a player now; they can join again in 5 minutes'),
                        ('ban', 'keeps a player out of this manager\'s servers (or, --network, the whole Reborn network)'),
                        ('mute', 'stops a player\'s voice and text chat reaching anyone on this manager\'s servers')):
        mp = sub.add_parser(name, help=words)
        mp.add_argument('who', help='a player on the servers now (name or player id), a player id, or with --ip an address')
        mp.add_argument('--reason', default='', help='shown to the player (120 characters at most)')
        if name != 'kick':
            mp.add_argument('--length', default='permanent', help='1h, 1d, 7d, 30d, permanent, or e.g. 12h (default: permanent)')
            mp.add_argument('--no-ip', action='store_true', help='do not hold the player\'s address (a shared one)')
        mp.add_argument('--ip', action='store_true', help='who is an address')
        mp.add_argument('--server', type=int, action='append', default=[], help='only this server (default: all of them)')
        mp.add_argument('--by', default='', help='your name in the moderation log')
        if name == 'ban':
            mp.add_argument('--network', action='store_true',
                            help='official manager on the main backend\'s machine only: the Reborn network\'s list')
    ub = sub.add_parser('unban', help='removes a ban, kick or mute by its id (manager.py bans lists them)')
    ub.add_argument('ban')
    ub.add_argument('--network', action='store_true')
    ub.add_argument('--by', default='')
    bl = sub.add_parser('bans', help='the bans, kicks and mutes that hold now')
    bl.add_argument('--network', action='store_true')
    sub.add_parser('modlog', help='the moderation log, newest first')
    lk = sub.add_parser('lookup', help='players seen in the last week by name or player id')
    lk.add_argument('text', nargs='?', default='')
    lk.add_argument('--network', action='store_true', help='official: everyone online, or names seen on the network')
    sub.add_parser('update', help='look for a new release now, and install it')
    i = sub.add_parser('install', help='the game\'s files from your own copy of the game')
    i.add_argument('path')
    q = sub.add_parser('quit', help='stops the manager and its servers')
    q.add_argument('--keep-servers', action='store_true')
    sub.add_parser('where', help='the data folder (server.json, game, logs)')
    sub.add_parser('broadcast-key', help='official package: the public key community servers check network '
                                         'broadcasts with (for gmhost.py OFFICIAL_BROADCAST_KEYS)')
    sub.add_parser('open', help='the page in a browser; starts the manager in the background when it is not running')
    args = ap.parse_args()
    command = args.command or 'run'
    try:
        if command == 'run':
            return run(args)
        if command == 'where':
            print(DATA)
            return 0
        if command == 'broadcast-key':
            print(host_module().broadcast_public(official_key_or_fail('The broadcast key')))
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
            reply = ask('/api/broadcast', {'kind': args.kind, 'seconds': int(args.minutes * 60), 'text': args.text,
                                           'servers': args.server, 'network': args.to,
                                           'restart': args.kind == 'restart' and not args.no_restart})
            sent = [x for x in reply['state'].get('sent', []) if x.get('network')][:1] if args.to else []
            print('sent' + (' to %s' % sent[0]['network'] if sent else '') +
                  ('; the countdown runs in the manager' if reply.get('countdown') else ''))
            return 0
        if command == 'promote':
            official_key_or_fail('Promotion')
            print(promote(args.key, args.note))
            return 0
        if command == 'demote':
            official_key_or_fail('Promotion')
            print(demote(args.key))
            return 0
        if command == 'promoted':
            rows = read_promoted()
            live = {}
            with contextlib.suppress(RuntimeError):
                for h in community_hosts():
                    live.setdefault(h.get('host_key'), []).append(h)
            if not rows:
                print('No community server is promoted (%s).' % PROMOTED_FILE)
            for r in rows:
                k = str(r.get('key', '')).lower()
                now = live.get(k, [])
                state = ('; now: ' + ', '.join('%s (%s)' % (h['name'], 'shown as trusted' if h.get('promoted') else
                                                            h.get('hidden') or 'not listed') for h in now)) if now \
                    else '; not registered now'
                print('%s  since %s  %s%s' % (k, r.get('since', '?'), r.get('note', ''), state))
            return 0
        if command == 'community':
            hosts = community_hosts()
            if not hosts:
                print('No community server is registered with the main backend now.')
            for h in hosts:
                print('%-64s  game %-4s %-32s %s:%s  v%s  %d/%d  %s' % (
                    h.get('host_key') or '(no host key: an older package)', h.get('gid'), h.get('name', '')[:32],
                    h.get('from'), h.get('port'), h.get('version') or '?', h.get('players', 0), h.get('max_players', 0),
                    'PROMOTED' if h.get('promoted') else 'promoted, not shown: %s' % (h.get('hidden') or 'not listed')
                    if h.get('promoted_key') else 'hidden: %s' % h['hidden'] if h.get('hidden') else
                    'listed' if h.get('listed') else 'not listed'))
            return 0
        if command == 'maintenance':
            if args.action == 'status':
                print(maintenance_words())
                return 0
            fields = {'on': args.action == 'on', 'kind': args.preset, 'text': args.text, 'minutes': args.minutes,
                      'allow': args.allow}
            if running_manager():  # the running manager writes it, and keeps a note of it in its log
                print(ask('/api/maintenance', fields)['maintenance'])
            else:  # no manager: the file itself (the backend, when it starts, reads it)
                print(set_maintenance(fields['on'], fields['kind'], fields['text'], fields['minutes'], fields['allow']))
            return 0
        if command == 'players':
            rows = ask('/api/moderation')['players']
            if not rows:
                print('Nobody is on these servers now.')
            for r in rows:
                print('  server %-2d %-16s id %-8s %s' % (r['server'], r['name'], r['id'] or '?', r['ip']))
            return 0
        if command in ('kick', 'ban', 'mute'):
            data = {'action': command, 'reason': args.reason, 'servers': args.server, 'by': args.by,
                    'list': 'network' if getattr(args, 'network', False) else 'server',
                    'length': getattr(args, 'length', ''), 'match_ip': not getattr(args, 'no_ip', False)}
            if args.ip:
                data['ips'] = [args.who]
            else:
                found = moderation_target(args.who, getattr(args, 'network', False))
                data['player'] = found
            reply = ask('/api/moderation/action', data)
            e = reply['entry']
            print('%s %s: %s (%s), %s' % (command, e['id'], ', '.join(e['names']) or ', '.join(e['ids']) or 'an address',
                                         'ends ' + e['ends'] if e['until'] else 'permanent', e['reason'] or 'no reason'))
            return 0
        if command == 'unban':
            e = ask('/api/moderation/action', {'action': 'unban', 'ban': args.ban, 'by': args.by,
                                                'list': 'network' if args.network else 'server'})['entry']
            print('removed %s %s (%s)' % (e['kind'], e['id'], ', '.join(e['names']) or ', '.join(e['ids'])))
            return 0
        if command == 'bans':
            mod = ask('/api/moderation')
            rows = mod['network'] if args.network else mod['bans']
            if not rows:
                print('None.')
            for e in rows:
                print('%s  %-4s %-24s ids %-18s %s%s  %s  ends %s  by %s' % (
                    e['id'], e['kind'], ', '.join(e['names'])[:24], ','.join(e['ids'])[:18], ' +account' if e['accounts'] else '',
                    ' +ip' if e['ips'] else '', e['reason'] or '-', e['ends'], e['by']))
            return 0
        if command == 'modlog':
            for line in ask('/api/moderation')['log'][:100]:
                print('%s  %s' % (time.strftime('%Y-%m-%d %H:%M', time.gmtime(line.get('at', 0))),
                                  ' '.join('%s=%s' % (k, v) for k, v in line.items() if k != 'at')))
            return 0
        if command == 'lookup':
            for r in ask('/api/moderation/lookup', {'text': args.text, 'network': args.network})['players']:
                print('  %-16s id %-8s %-15s %s %s' % (r['name'], r['id'] or '?', r['ip'], r.get('server', ''),
                                                       'online' if r.get('online') else ''))
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
