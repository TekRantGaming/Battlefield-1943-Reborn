"""The shared bot data (TekRantGaming, 26 Nov 2026: "we do this for the official servers only, and then we can, when we
do like hot fixes and releases, also include updated bot information for community servers").

The bots on an official server learn each map from how people play it (bf/src/bots.cpp, docs/BOTS-AND-CUSTOM-MATCH.md
"What bots learn") and hand what they learned since the last hand-off to the main backend: bots/outbox/ files that the
server's host (gmhost.py) sends over its link, and the main backend (plasma.py --bot-data) keeps here. This module
merges them into one set, signs it with the official machine's network key, and the main backend sends it back to
every official server (gmhost.py writes it to bots/network/; the bots use it from their next level). Every release
carries the latest set beside the program (bf/data/bots/, filled by `botdata.py install`), so community servers and
offline Custom Matches get it with each update. Nothing here comes from or goes to a community server.

What is kept, under the main backend's --bot-data folder:
  in/<day>/<server>/<file>  each hand-off as it came in (checked again at the merge), kept 90 days (the privacy
                            page's promise; a server's own hand-offs can be taken out with `rebuild --exclude`)
  in/.changed               touched at each hand-off: the next merge has work
  state/<map>.txt, style.txt  the merged totals: every count multiplied by FADE each day (old play fades), plus what
                            came in since; merged hand-offs are moved to merged/<day>/... (same 90 days)
  grids/<map>.grid          the most complete finished level grid handed in (geometry only, no players in it)
  out/<version>/            the set as published: <map>.txt, <map>.grid, style.txt, version.txt, manifest.json and
                            manifest.sig; out/latest.txt names the newest. The last three are kept.

The files hold no names, ids, addresses or times: counts per 2 m cell of the map, sides, flag and vehicle names. A
hand-off is made only once at least three different players have played the map since the last one (bots.cpp
bf_bots_share_min_players), so none is one person's routes.

  python botdata.py merge --root <folder> [--key-file official.key] [--force]
  python botdata.py rebuild --root <folder> [--exclude <server>] [--key-file official.key]
  python botdata.py verify <set folder> [--key <public hex>]
  python botdata.py install --from <set folder> --to <repo>/bf/data/bots [--key <public hex>]
"""
import argparse
import datetime
import hashlib
import hmac
import json
import math
import os
import re
import shutil
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:  # python -I leaves the script's own folder out
    sys.path.insert(0, HERE)

FADE = 0.97              # each day every count is multiplied by this: about half after three weeks
KEEP_DAYS = 90           # hand-offs as they came in are deleted after this (the privacy page)
KEEP_VERSIONS = 3        # published sets kept in out/
SMALLEST = 0.01          # a faded count below this is dropped (the set stays bounded)
MAX_FILE = 32 << 20      # a hand-off's size, as it comes in
MAX_LINES = 8000000
GRID_MAGIC = 0x42475232  # "BGR2", bots.cpp SaveGrid from 0.4.8.1
# "BGR1": 0.4.8's grids. Its servers kept the last level's water height over a level change (Wake's sea, 215 m, on
# Guadalcanal and Iwo Jima): those levels were "mapped" in no time as mostly sea (139 cells in 0 s), and the bots took
# the land for water. Only Wake's own (the first level, its water found on it) are kept; a BGR2 grid takes over a BGR1.
GRID_MAGIC_OLD = 0x42475231
OLD_GRID_MAPS = ('Wake_Island',)
MAX_GRID_CELLS = 20000000
CELL_LIMIT = 1 << 20     # as bots.cpp ReadLearned
HEADER_LEARNED = 'BF1943 Reborn bots, what they learned on %s (1)'
HEADER_STYLE = "BF1943 Reborn bots, the human players' style (1)"
STYLE_FIELDS = ('time', 'sprint', 'still', 'fire', 'ads', 'bursts', 'burst_time', 'kills', 'kill_dist', 'flank_kills',
                'attack', 'defend', 'kit0', 'kit1', 'kit2')
SIGNED_PREFIX = 'bf1943-botdata|1|'

# A hand-off's file name: "<map>.<ms>.txt" (what was learned on a map), "<map>.<ms>.grid" (a finished level grid),
# "style.<ms>.txt" (the players' style). <map> as bots.cpp MapFileName makes it: letters, digits, "_".
NAME_RE = re.compile(r'^([A-Za-z0-9_]{1,48})\.(\d{10,16})\.(txt|grid)$')
TOKEN_RE = re.compile(r'^[^\s]{1,64}$')
SERVER_RE = re.compile(r'[^A-Za-z0-9_-]+')


def utc_day(t=None):
    return datetime.datetime.fromtimestamp(time.time() if t is None else t, datetime.timezone.utc).date()


def server_folder(name):
    """A folder name for a server: its name, letters, digits, "_" and "-" only."""
    return SERVER_RE.sub('_', str(name)).strip('_')[:40] or 'server'


# ---- the set's form (bots.cpp WriteLearned / ReadLearned)

def new_set():
    return {'c': {}, 's': {}, 'k': {}, 'a': {}, 'vt': {}, 'vc': {}, 'vk': {}}


def _fine(v):
    return math.isfinite(v) and 0 <= v < 1e9


def _cell(x, z):
    return abs(x) <= CELL_LIMIT and abs(z) <= CELL_LIMIT


def _step_x(x):
    """A step's x as the game writes it (bots.cpp WriteLearned: CellX(key >> 3) keeps 29 bits, so a cell left of 0
    comes out as x + 2^29; the game's StepKey gives the same key for either): back to its own value."""
    x &= 0x1FFFFFFF
    return x - (1 << 29) if x >= (1 << 28) else x


def parse_learned(text):
    """A learned set from its text: every line checked as the game checks it (bots.cpp ReadLearned); bad lines are
    left out. Returns (the set, lines read, lines left out)."""
    out = new_set()
    lines = text.split('\n')
    bad = 0
    for line in lines[1:MAX_LINES]:
        p = line.split()
        if not p:
            continue
        try:
            kind = p[0]
            if kind == 'c' and len(p) == 4:
                x, z, y = int(p[1]), int(p[2]), float(p[3])
                if _cell(x, z) and math.isfinite(y) and abs(y) < 1e5:
                    out['c'][(x, z)] = y
                    continue
            elif kind == 's' and len(p) == 5:
                x, z, d, w = _step_x(int(p[1])), int(p[2]), int(p[3]), float(p[4])
                if _cell(x, z) and 0 <= d < 8 and _fine(w):
                    out['s'][(x, z, d)] = out['s'].get((x, z, d), 0.0) + w
                    continue
            elif kind in ('k', 'd', 'w') and len(p) == 5:
                side, x, z, w = int(p[1]), int(p[2]), int(p[3]), float(p[4])
                if side in (1, 2) and _cell(x, z) and _fine(w):
                    key = (kind, side, x, z)
                    out['k'][key] = out['k'].get(key, 0.0) + w
                    continue
            elif kind == 'a' and len(p) == 6:
                flag, x, z, d, w = p[1], _step_x(int(p[2])), int(p[3]), int(p[4]), float(p[5])
                if TOKEN_RE.match(flag) and _cell(x, z) and 0 <= d < 8 and _fine(w):
                    key = (flag, x, z, d)
                    out['a'][key] = out['a'].get(key, 0.0) + w
                    continue
            elif kind == 'vt' and len(p) == 5:
                key, a, b, c = p[1], float(p[2]), float(p[3]), float(p[4])
                if TOKEN_RE.match(key) and _fine(a) and _fine(b) and _fine(c):
                    t = out['vt'].setdefault(key, [0.0, 0.0, 0.0])
                    t[0] += a
                    t[1] += b
                    t[2] += c
                    continue
            elif kind == 'vc' and len(p) == 6:
                key, x, z, h, n = p[1], int(p[2]), int(p[3]), float(p[4]), float(p[5])
                if TOKEN_RE.match(key) and _cell(x, z) and math.isfinite(h) and abs(h) < 1e5 and _fine(n) and n > 0:
                    c = out['vc'].setdefault((key, x, z), [0.0, 0.0])
                    c[0] += h * n
                    c[1] += n
                    continue
            elif kind == 'vk' and len(p) == 5:
                key, x, z, w = p[1], int(p[2]), int(p[3]), float(p[4])
                if TOKEN_RE.match(key) and _cell(x, z) and _fine(w):
                    out['vk'][(key, x, z)] = out['vk'].get((key, x, z), 0.0) + w
                    continue
        except ValueError:
            pass
        bad += 1
    return out, len(lines) - 1, bad


def add_set(dst, src, weight=1.0):
    """dst += weight * src (the walked ground's heights are not counts: kept where dst has one)."""
    for k, y in src['c'].items():
        dst['c'].setdefault(k, y)
    for part in ('s', 'k', 'a', 'vk'):
        d = dst[part]
        for k, w in src[part].items():
            d[k] = d.get(k, 0.0) + weight * w
    for k, t in src['vt'].items():
        d = dst['vt'].setdefault(k, [0.0, 0.0, 0.0])
        for i in range(3):
            d[i] += weight * t[i]
    for k, (hs, n) in src['vc'].items():
        d = dst['vc'].setdefault(k, [0.0, 0.0])
        d[0] += weight * hs
        d[1] += weight * n


def fade_set(s, factor):
    """Every count times factor; what falls below SMALLEST is dropped (the heights of the walked ground stay)."""
    for part in ('s', 'k', 'a', 'vk'):
        s[part] = {k: w * factor for k, w in s[part].items() if w * factor >= SMALLEST}
    s['vt'] = {k: [v * factor for v in t] for k, t in s['vt'].items() if t[0] * factor >= SMALLEST}
    s['vc'] = {k: [hs * factor, n * factor] for k, (hs, n) in s['vc'].items() if n * factor >= SMALLEST}


def _num(v):
    return '%.6g' % v


def format_learned(s, map_name):
    """The set in the game's text form (bots.cpp WriteLearned's lines)."""
    out = [HEADER_LEARNED % map_name]
    out += ['c %d %d %s' % (x, z, _num(y)) for (x, z), y in sorted(s['c'].items())]
    out += ['s %d %d %d %s' % (x, z, d, _num(w)) for (x, z, d), w in sorted(s['s'].items())]
    out += ['%s %d %d %d %s' % (kind, side, x, z, _num(w)) for (kind, side, x, z), w in sorted(s['k'].items())]
    out += ['a %s %d %d %d %s' % (f, x, z, d, _num(w)) for (f, x, z, d), w in sorted(s['a'].items())]
    for key in sorted(set(k for k in s['vt']) | set(k for k, _, _ in s['vc']) | set(k for k, _, _ in s['vk'])):
        t = s['vt'].get(key, [0.0, 0.0, 0.0])
        out.append('vt %s %s %s %s' % (key, _num(t[0]), _num(t[1]), _num(t[2])))
    out += ['vc %s %d %d %s %s' % (k, x, z, _num(hs / n), _num(n)) for (k, x, z), (hs, n) in sorted(s['vc'].items()) if n > 0]
    out += ['vk %s %d %d %s' % (k, x, z, _num(w)) for (k, x, z), w in sorted(s['vk'].items())]
    return '\n'.join(out) + '\n'


def parse_style(text):
    out = {}
    for line in text.split('\n')[1:64]:
        p = line.split()
        if len(p) == 2 and p[0] in STYLE_FIELDS:
            try:
                v = float(p[1])
            except ValueError:
                continue
            if _fine(v):
                out[p[0]] = out.get(p[0], 0.0) + v
    return out


def format_style(s):
    return HEADER_STYLE + '\n' + ''.join('%s %s\n' % (f, _num(s.get(f, 0.0))) for f in STYLE_FIELDS)


def check_grid(data, m=None):
    """'' when data is a finished level grid (bots.cpp SaveGrid: magic, done 1, count, 15 bytes a cell) of map m (a
    0.4.8 grid only of the maps in OLD_GRID_MAPS), else why not."""
    if len(data) < 16:
        return 'too short for a grid'
    magic, done, count = struct.unpack_from('<IIQ', data, 0)
    if magic == GRID_MAGIC_OLD:
        if m not in OLD_GRID_MAPS:
            return "a 0.4.8 grid of a level after the first (the last level's water height)"
    elif magic != GRID_MAGIC:
        return 'not a grid'
    if done != 1:
        return 'a scan in part'
    if count > MAX_GRID_CELLS or len(data) != 16 + 15 * count:
        return 'a grid of the wrong size'
    return ''


def grid_cells(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(16)
        return struct.unpack_from('<IIQ', head, 0)[2] if len(head) == 16 else 0
    except OSError:
        return 0


def grid_better(data, path, m):
    """Whether the grid data should take the place of the one at path (map m): a good one over a bad or missing one,
    a 0.4.8.1 grid over a 0.4.8 one, else the one with more cells."""
    try:
        have = _read(path)
    except OSError:
        return True
    if check_grid(have, m):
        return True
    new_v, have_v = struct.unpack_from('<I', data, 0)[0], struct.unpack_from('<I', have, 0)[0]
    if new_v != have_v:
        return new_v == GRID_MAGIC
    return struct.unpack_from('<Q', data, 8)[0] > struct.unpack_from('<Q', have, 8)[0]


# ---- writing

def write_whole(path, data):
    """A file written beside, then moved over the old one (a reader never sees half of it)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.new'
    with open(tmp, 'wb') as f:
        f.write(data if isinstance(data, bytes) else data.encode('utf-8'))
    os.replace(tmp, path)


# ---- the main backend's side: a hand-off as it comes in (plasma.py, from an official server only)

def ingest(root, server, name, data):
    """Keeps a hand-off for the next merge. Light (the main backend's loop runs it): the name and size are checked
    here, every line at the merge. Returns '' or why it is refused. The same file sent again (its acknowledgement was
    lost) replaces itself: nothing counts twice."""
    if not isinstance(name, str) or not NAME_RE.match(name):
        return 'a file name that is not a hand-off\'s'
    if not isinstance(data, (bytes, bytearray)) or not data or len(data) > MAX_FILE:
        return 'an empty or too big file'
    if name.endswith('.grid'):
        why = check_grid(data, _map_of(name))
        if why:
            return why
    elif not data.startswith(b'BF1943 Reborn bots, '):
        return 'not a bots file'
    path = os.path.join(root, 'in', utc_day().isoformat(), server_folder(server), name)
    write_whole(path, bytes(data))
    write_whole(os.path.join(root, 'in', '.changed'), str(time.time()))
    return ''


# ---- the merge (its own process: plasma.py starts it, so the main backend's loop never waits for it)

def _day_folders(base):
    out = []
    try:
        for d in os.listdir(base):
            try:
                out.append((datetime.date.fromisoformat(d), os.path.join(base, d)))
            except ValueError:
                continue
    except OSError:
        pass
    return sorted(out)


def _read(path):
    with open(path, 'rb') as f:
        return f.read(MAX_FILE + 1)


def _map_of(name):
    m = NAME_RE.match(name)
    return m.group(1) if m else None


def _load_state(root):
    maps, style = {}, {}
    sd = os.path.join(root, 'state')
    try:
        names = os.listdir(sd)
    except OSError:
        names = []
    for n in names:
        if n == 'style.txt':
            style = parse_style(_read(os.path.join(sd, n)).decode('utf-8', 'replace'))
        elif n.endswith('.txt') and re.match(r'^[A-Za-z0-9_]{1,48}\.txt$', n):
            maps[n[:-4]] = parse_learned(_read(os.path.join(sd, n)).decode('utf-8', 'replace'))[0]
    meta = {}
    try:
        with open(os.path.join(sd, 'state.json'), encoding='utf-8') as f:
            meta = json.load(f)
    except (OSError, ValueError):
        pass
    return maps, style, meta


def _take(maps, style, root, path, name, note, weight=1.0):
    """One hand-off into the totals: its lines checked (bad ones left out), a grid kept when it is the most complete."""
    data = _read(path)
    if name.endswith('.grid'):
        m = _map_of(name)
        why = check_grid(data, m)
        if why or not m:
            note('  %s: left out (%s)' % (path, why or 'its name'))
            return
        have = os.path.join(root, 'grids', m + '.grid')
        if grid_better(data, have, m):
            write_whole(have, data)
            note('  %s: the most complete grid of %s now (%d cells)' % (path, m, struct.unpack_from('<Q', data, 8)[0]))
        return
    text = data.decode('utf-8', 'replace')
    if name.startswith('style.'):
        for k, v in parse_style(text).items():
            style[k] = style.get(k, 0.0) + weight * v
        return
    m = _map_of(name)
    s, lines, bad = parse_learned(text)
    if bad:
        note('  %s: %d of %d lines left out' % (path, bad, lines))
    add_set(maps.setdefault(m, new_set()), s, weight)


def _fade_to(maps, style, meta, today):
    """The totals faded to today (FADE a day since the day they were last faded to)."""
    last = meta.get('faded_to')
    try:
        days = (today - datetime.date.fromisoformat(last)).days if last else 0
    except ValueError:
        days = 0
    if days > 0:
        f = FADE ** days
        for s in maps.values():
            fade_set(s, f)
        for k in list(style):
            style[k] *= f
    meta['faded_to'] = today.isoformat()


def _publish(root, maps, style, key, note):
    """A new set in out/<version>/: every map, the grids, the style, version.txt, the manifest and its signature."""
    latest = 0
    try:
        with open(os.path.join(root, 'out', 'latest.txt'), encoding='utf-8') as f:
            latest = int(f.read().strip() or 0)
    except (OSError, ValueError):
        pass
    version = max(latest + 1, int(time.time()))
    files = {}
    for m, s in sorted(maps.items()):
        files[m + '.txt'] = format_learned(s, m).encode('utf-8')
    if style:
        files['style.txt'] = format_style(style).encode('utf-8')
    gd = os.path.join(root, 'grids')
    try:
        for n in sorted(os.listdir(gd)):
            if n.endswith('.grid') and re.match(r'^[A-Za-z0-9_]{1,48}\.grid$', n):
                d = _read(os.path.join(gd, n))
                why = check_grid(d, n[:-5])
                if why:
                    note('  grids/%s: not published (%s)' % (n, why))
                    continue
                files[n] = d
    except OSError:
        pass
    built = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')
    files['version.txt'] = ('version %d\nbuilt %s\n' % (version, built)).encode('utf-8')
    manifest = json.dumps({'version': version, 'built': built,
                           'files': {n: {'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()}
                                     for n, d in sorted(files.items())}}, sort_keys=True, separators=(',', ':'))
    out = os.path.join(root, 'out', str(version))
    os.makedirs(out, exist_ok=True)
    for n, d in files.items():
        write_whole(os.path.join(out, n), d)
    write_whole(os.path.join(out, 'manifest.json'), manifest)
    if key:
        write_whole(os.path.join(out, 'manifest.sig'), sign_manifest(key, manifest))
    write_whole(os.path.join(root, 'out', 'latest.txt'), '%d\n' % version)
    for d, p in sorted(((int(d), os.path.join(root, 'out', d)) for d in os.listdir(os.path.join(root, 'out'))
                        if d.isdigit()), reverse=True)[KEEP_VERSIONS:]:
        shutil.rmtree(p, ignore_errors=True)
    note('published set %d: %s' % (version, ', '.join('%s %d KB' % (n, len(d) // 1024) for n, d in sorted(files.items()))))
    return version


def _save_state(root, maps, style, meta):
    sd = os.path.join(root, 'state')
    for m, s in maps.items():
        write_whole(os.path.join(sd, m + '.txt'), format_learned(s, m))
    if style:
        write_whole(os.path.join(sd, 'style.txt'), format_style(style))
    write_whole(os.path.join(sd, 'state.json'), json.dumps(meta, sort_keys=True))


def _forget_old(root, today, note):
    for base in ('in', 'merged'):
        for day, path in _day_folders(os.path.join(root, base)):
            if (today - day).days > KEEP_DAYS:
                shutil.rmtree(path, ignore_errors=True)
                note('deleted the hand-offs of %s (older than %d days)' % (day, KEEP_DAYS))


def _seed(root, maps, style, meta, seed, note, today):
    """Totals that start empty start from the set shipped with this package (its bin/data/bots): the official machine
    set up again or its botdata folder lost. Without it the first set would hold only what came in since, and the
    official servers would take that over the shipped one."""
    if maps or style or not seed or not os.path.exists(os.path.join(seed, 'version.txt')):
        return
    for n in sorted(os.listdir(seed)):
        path = os.path.join(seed, n)
        if n == 'style.txt':
            style.update(parse_style(_read(path).decode('utf-8', 'replace')))
        elif re.match(r'^[A-Za-z0-9_]{1,48}\.txt$', n) and n != 'version.txt':
            maps[n[:-4]] = parse_learned(_read(path).decode('utf-8', 'replace'))[0]
        elif re.match(r'^[A-Za-z0-9_]{1,48}\.grid$', n) and not check_grid(_read(path), n[:-5]):
            have = os.path.join(root, 'grids', n)
            if grid_better(_read(path), have, n[:-5]):
                write_whole(have, _read(path))
    meta['faded_to'] = today.isoformat()
    note('started from the shipped set (%s): %s' % (seed, ', '.join(sorted(maps)) or 'its style only'))


def merge(root, key=None, force=False, note=print, today=None, seed=None):
    """New hand-offs into the faded totals, then a new set published. Returns its version, or 0 when nothing came in.
    seed: the shipped set's folder, for totals that start empty (_seed)."""
    today = today or utc_day()
    changed = os.path.join(root, 'in', '.changed')
    if not force and not os.path.exists(changed):
        return 0
    try:
        os.remove(changed)  # (a hand-off from now on makes the next merge)
    except OSError:
        pass
    maps, style, meta = _load_state(root)
    _seed(root, maps, style, meta, seed, note, today)
    _fade_to(maps, style, meta, today)
    taken = 0
    for day, daydir in _day_folders(os.path.join(root, 'in')):
        for server in sorted(os.listdir(daydir)):
            sdir = os.path.join(daydir, server)
            if not os.path.isdir(sdir):
                continue
            for name in sorted(os.listdir(sdir)):
                if not NAME_RE.match(name):
                    continue
                path = os.path.join(sdir, name)
                try:
                    # A hand-off of an earlier day than the totals' (a merge that did not run): faded as it would have been.
                    _take(maps, style, root, path, name, note, FADE ** max(0, (today - day).days))
                except OSError as e:
                    note('  %s: could not be read (%s)' % (path, e))
                    continue
                dest = os.path.join(root, 'merged', day.isoformat(), server, name)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                os.replace(path, dest)
                taken += 1
    _save_state(root, maps, style, meta)
    _forget_old(root, today, note)
    note('merged %d hand-offs: %s' % (taken, ', '.join('%s %d entries' % (m, entries(s)) for m, s in sorted(maps.items()))))
    return _publish(root, maps, style, key, note)


def rebuild(root, exclude=(), key=None, note=print, today=None):
    """The totals made again from every hand-off kept (the last KEEP_DAYS days), each faded by its age, leaving out
    the servers named: a server whose hand-offs turned out wrong is taken out of the set."""
    today = today or utc_day()
    exclude = {server_folder(x) for x in exclude}
    maps, style = {}, {}
    for base in ('merged', 'in'):
        for day, daydir in _day_folders(os.path.join(root, base)):
            for server in sorted(os.listdir(daydir)):
                if server in exclude or not os.path.isdir(os.path.join(daydir, server)):
                    continue
                for name in sorted(os.listdir(os.path.join(daydir, server))):
                    if NAME_RE.match(name) and not name.endswith('.grid'):
                        _take(maps, style, root, os.path.join(daydir, server, name), name, note,
                              FADE ** max(0, (today - day).days))
    shutil.rmtree(os.path.join(root, 'state'), ignore_errors=True)
    _save_state(root, maps, style, {'faded_to': today.isoformat()})
    note('rebuilt from the hand-offs kept%s' % (' without %s' % ', '.join(sorted(exclude)) if exclude else ''))
    return _publish(root, maps, style, key, note)


def entries(s):
    return sum(len(s[p]) for p in s)


# ---- signing and checking a published set

def seed_from_key(official_key):
    """The network signing key (gmhost.py broadcast_seed): made from official.key, which only the official machine has."""
    return hmac.new(official_key, b'bf1943 network broadcast signing key 1', hashlib.sha256).digest()


def signed_text(manifest):
    return SIGNED_PREFIX + hashlib.sha256(manifest.encode('utf-8')).hexdigest()


def sign_manifest(official_key, manifest):
    import ed25519
    return ed25519.sign(seed_from_key(official_key), signed_text(manifest).encode('utf-8')).hex()


def check_manifest(manifest, sig, keys):
    """'' when the manifest's signature checks with one of keys (bytes), else why not; then (why, the manifest)."""
    import ed25519
    try:
        raw = bytes.fromhex(sig.strip())
        m = json.loads(manifest)
    except (ValueError, AttributeError):
        return 'a manifest or signature that cannot be read', None
    if not keys:
        return 'no official key is known to check it with', None
    if len(raw) != 64 or not any(ed25519.verify(k, signed_text(manifest).encode('utf-8'), raw) for k in keys):
        return 'the signature does not check (not from the official machine)', None
    if not isinstance(m, dict) or not isinstance(m.get('version'), int) or not isinstance(m.get('files'), dict):
        return 'a manifest without its version or files', None
    for n, f in m['files'].items():
        if not isinstance(n, str) or not re.match(r'^[A-Za-z0-9_]{1,48}\.(txt|grid)$', n) or not isinstance(f, dict):
            return 'a file name that does not belong in a set: %r' % n, None
    return '', m


def check_file(m, name, data):
    f = m['files'].get(name)
    if not f:
        return 'not in the manifest'
    if len(data) != f.get('size') or hashlib.sha256(data).hexdigest() != f.get('sha256'):
        return 'its size or checksum is not the manifest\'s'
    return ''


def official_keys(extra=()):
    """The official machine's public keys a set may be signed with (gmhost.py's, and any given)."""
    import gmhost
    keys = []
    for k in list(gmhost.OFFICIAL_BROADCAST_KEYS) + [x for x in os.environ.get('BF_DEV_BROADCAST_KEY', '').split(',')] + list(extra):
        k = k.strip().lower()
        if len(k) == 64:
            try:
                keys.append(bytes.fromhex(k))
            except ValueError:
                pass
    return keys


def verify_folder(folder, keys):
    """'' when a set's folder is whole and signed by the official machine; then (why, the manifest)."""
    try:
        with open(os.path.join(folder, 'manifest.json'), encoding='utf-8') as f:
            manifest = f.read()
        with open(os.path.join(folder, 'manifest.sig'), encoding='utf-8') as f:
            sig = f.read()
    except OSError as e:
        return 'no manifest and signature in %s (%s)' % (folder, e), None
    why, m = check_manifest(manifest, sig, keys)
    if why:
        return why, None
    for n in m['files']:
        try:
            data = _read(os.path.join(folder, n))
        except OSError:
            return '%s is missing' % n, None
        why = check_file(m, n, data)
        if why:
            return '%s: %s' % (n, why), None
    return '', m


def install(src, dest, keys, note=print):
    """A published set into a release (the repository's bf/data/bots/, which every package carries beside the program):
    checked first; the old set's files are replaced, version.txt last."""
    why, m = verify_folder(src, keys)
    if why:
        raise SystemExit('the set in %s is not installed: %s' % (src, why))
    os.makedirs(dest, exist_ok=True)
    for n in os.listdir(dest):
        if n not in m['files'] and n not in ('manifest.json', 'manifest.sig', 'README.md'):
            os.remove(os.path.join(dest, n))
    for n in sorted(m['files'], key=lambda n: n == 'version.txt'):
        write_whole(os.path.join(dest, n), _read(os.path.join(src, n)))
    for n in ('manifest.json', 'manifest.sig'):
        write_whole(os.path.join(dest, n), _read(os.path.join(src, n)))
    note('installed the shared bot data %d (%s) into %s: %s' % (m['version'], m.get('built', ''), dest,
                                                               ', '.join(sorted(m['files']))))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('merge', help='merge the new hand-offs and publish a set')
    p.add_argument('--root', required=True)
    p.add_argument('--key-file', default='', help='official.key: the set is signed with it')
    p.add_argument('--force', action='store_true', help='publish even when nothing new came in')
    p.add_argument('--seed', default='', help="the shipped set (the package's bin/data/bots): totals that start empty start from it")
    p = sub.add_parser('rebuild', help='make the totals again from the hand-offs kept')
    p.add_argument('--root', required=True)
    p.add_argument('--exclude', action='append', default=[], help="a server's folder name whose hand-offs are left out")
    p.add_argument('--key-file', default='')
    p = sub.add_parser('verify', help='check a published set')
    p.add_argument('folder')
    p.add_argument('--key', action='append', default=[], help='a public key (hex) besides the official one')
    p = sub.add_parser('install', help='put a published set into the release (bf/data/bots)')
    p.add_argument('--from', dest='src', required=True)
    p.add_argument('--to', dest='dest', required=True)
    p.add_argument('--key', action='append', default=[])
    a = ap.parse_args(argv)
    if a.cmd in ('merge', 'rebuild'):
        key = None
        if a.key_file:
            with open(a.key_file, 'rb') as f:
                key = f.read().strip()
        if a.cmd == 'merge':
            v = merge(a.root, key, a.force, seed=a.seed or None)
        else:
            v = rebuild(a.root, a.exclude, key)
        print('version %d' % v)
    elif a.cmd == 'verify':
        why, m = verify_folder(a.folder, official_keys(a.key))
        if why:
            raise SystemExit('not a good set: %s' % why)
        print('a good set: version %d, built %s, %d files' % (m['version'], m.get('built', ''), len(m['files'])))
    else:
        src = a.src
        if os.path.isfile(os.path.join(src, 'latest.txt')):  # a backend's out/ folder: its newest set
            with open(os.path.join(src, 'latest.txt'), encoding='utf-8') as f:
                src = os.path.join(src, f.read().strip())
        install(src, a.dest, official_keys(a.key))


if __name__ == '__main__':
    main()
