"""The server owner's dashboard (TekRantGaming, 20 Nov 2026: "it would be nice for server owners to have a built in
dashboard to monitor things ... something like this but expanded", after a community admin's live stats page; his
answers: a Dashboard tab in the manager and an optional public page, live status, activity and history, a playtime
leaderboard, machine health, "anything you think would be cool", 90 days of history, names on the public page but
never player ids or addresses). docs/DASHBOARD.md has the whole design.

What it keeps, in the manager's data folder (dashboard/), one file a day (UTC), the last HISTORY_DAYS of them:
  players   name -> seconds played, sessions, seconds per map and per server, the game's counts (kills, deaths,
            captures... from server_stats.cpp's lines), the first and last time seen that day
  samples   every SAMPLE_SECONDS: players on each server, the machine's CPU and memory, each server's processes
  events    joins, leaves, map changes, a server starting, stopping or started again after its game or host ended
Names only: no player id and no address is ever written here (moderation keeps those, docs/MODERATION.md).

The manager calls tick() from its watch loop (every two seconds) with each server's state, and view() for its page;
public_view() is what the public page may show.
"""
import ctypes
import datetime
import json
import os
import sys
import time

HISTORY_DAYS = 90
SAMPLE_SECONDS = 60
SAVE_SECONDS = 60
EVENTS_KEPT = 400        # a day file keeps at most this many events
STAT_KEYS = ('kills', 'deaths', 'captures', 'assists', 'defends', 'repairs', 'heals', 'melee', 'score', 'roadkills',
             'headshots', 'vehicles')
WINDOWS = os.name == 'nt'


def day_of(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y-%m-%d')


def clean_name(value):
    return ''.join(c for c in str(value or '') if c.isalnum() or c in ' _[]-')[:24].strip()


def write_atomic(path, text):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.replace(tmp, path)


# ---- the machine and its processes (the standard library only, as the rest of the manager)
class Usage:
    """CPU time and memory of processes and of the whole machine, as two readings apart give a percentage."""

    def __init__(self):
        self.last = {}  # key -> (wall time, cpu seconds)
        self.cores = os.cpu_count() or 1

    @staticmethod
    def process(pid):
        """(CPU seconds used so far, resident memory in bytes) of a process, or None."""
        try:
            if WINDOWS:
                k32 = ctypes.WinDLL('kernel32', use_last_error=True)
                handle = k32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
                if not handle:
                    return None
                try:
                    ft = [ctypes.c_ulonglong() for _ in range(4)]
                    if not k32.GetProcessTimes(handle, *[ctypes.byref(x) for x in ft]):
                        return None

                    class Counters(ctypes.Structure):
                        _fields_ = [('cb', ctypes.c_ulong), ('PageFaultCount', ctypes.c_ulong),
                                    ('PeakWorkingSetSize', ctypes.c_size_t), ('WorkingSetSize', ctypes.c_size_t),
                                    ('QuotaPeakPagedPoolUsage', ctypes.c_size_t), ('QuotaPagedPoolUsage', ctypes.c_size_t),
                                    ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                                    ('QuotaNonPagedPoolUsage', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                                    ('PeakPagefileUsage', ctypes.c_size_t)]
                    c = Counters()
                    c.cb = ctypes.sizeof(c)
                    k32.K32GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb)
                    return (ft[2].value + ft[3].value) / 1e7, int(c.WorkingSetSize)
                finally:
                    k32.CloseHandle(handle)
            with open('/proc/%d/stat' % int(pid)) as f:
                fields = f.read().rsplit(')', 1)[1].split()
            ticks = os.sysconf('SC_CLK_TCK')
            with open('/proc/%d/statm' % int(pid)) as f:
                rss = int(f.read().split()[1]) * os.sysconf('SC_PAGE_SIZE')
            return (int(fields[11]) + int(fields[12])) / ticks, rss
        except (OSError, ValueError, IndexError, AttributeError):
            return None

    @staticmethod
    def machine():
        """(CPU seconds busy, CPU seconds in all, memory used, memory in all), or None."""
        try:
            if WINDOWS:
                k32 = ctypes.WinDLL('kernel32')
                idle, kernel, user = ctypes.c_ulonglong(), ctypes.c_ulonglong(), ctypes.c_ulonglong()
                if not k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
                    return None

                class Memory(ctypes.Structure):
                    _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                                ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                                ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                                ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                                ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
                m = Memory()
                m.dwLength = ctypes.sizeof(m)
                k32.GlobalMemoryStatusEx(ctypes.byref(m))
                total = (kernel.value + user.value) / 1e7  # kernel time includes the idle time
                return total - idle.value / 1e7, total, m.ullTotalPhys - m.ullAvailPhys, m.ullTotalPhys
            with open('/proc/stat') as f:
                cpu = [int(x) for x in f.readline().split()[1:]]
            ticks = os.sysconf('SC_CLK_TCK')
            idle = cpu[3] + (cpu[4] if len(cpu) > 4 else 0)
            info = {}
            with open('/proc/meminfo') as f:
                for line in f:
                    k, _, v = line.partition(':')
                    info[k] = int(v.split()[0]) * 1024
            total_mem = info.get('MemTotal', 0)
            return (sum(cpu) - idle) / ticks, sum(cpu) / ticks, total_mem - info.get('MemAvailable', 0), total_mem
        except (OSError, ValueError, IndexError, AttributeError):
            return None

    def percent(self, key, now, busy, total=None):
        """CPU % since the last reading of this key: of the whole machine (all its cores)."""
        last = self.last.get(key)
        self.last[key] = (now if total is None else total, busy)
        if not last:
            return None
        span = ((now if total is None else total) - last[0]) * (self.cores if total is None else 1)
        return max(0.0, min(100.0, 100.0 * (busy - last[1]) / span)) if span > 0 else None


class Dashboard:
    def __init__(self, folder, note=None, now=None):
        self.folder = folder
        self.note = note or (lambda text: None)
        os.makedirs(folder, exist_ok=True)
        self.days = {}            # 'YYYY-MM-DD' -> its record
        self.dirty = set()
        self.online = {}          # (server n, name lower) -> {'name', 'since', 'map'}
        self.maps = {}            # server n -> its map as last seen
        self.states = {}          # server n -> its state as last seen
        self.last_tick = None
        self.sampled = 0.0
        self.saved = time.time()
        self.usage = Usage()
        self.live = {}            # server n -> the last tick's view of it (and its processes' use)
        self.machine = {}
        self.stats_at = {}        # server n -> bytes of its stats file read
        self.ticks = {}           # server n -> the last "BF server ticks" line of its log, read
        self.ticks_looked = 0.0
        self.network = None       # the main backend's list, when asked (Manager)
        self.load(now)

    # ---- the files
    def path(self, day):
        return os.path.join(self.folder, 'day-%s.json' % day)

    def load(self, now=None):
        now = now or time.time()
        keep = {day_of(now - d * 86400) for d in range(HISTORY_DAYS)}
        for name in sorted(os.listdir(self.folder)):
            if not (name.startswith('day-') and name.endswith('.json')):
                continue
            day = name[4:-5]
            if day not in keep:
                try:
                    os.remove(os.path.join(self.folder, name))  # older than the 90 days: gone
                except OSError:
                    pass
                continue
            try:
                with open(os.path.join(self.folder, name), encoding='utf-8') as f:
                    rec = json.load(f)
                if isinstance(rec, dict):
                    rec.setdefault('players', {}), rec.setdefault('samples', []), rec.setdefault('events', [])
                    self.days[day] = rec
            except (OSError, ValueError):
                self.note('dashboard: %s could not be read' % name)

    def record(self, t):
        day = day_of(t)
        rec = self.days.get(day)
        if rec is None:
            rec = self.days[day] = {'day': day, 'players': {}, 'samples': [], 'events': []}
            for old in [d for d in self.days if d < day_of(t - (HISTORY_DAYS - 1) * 86400)]:
                del self.days[old]
                try:
                    os.remove(self.path(old))
                except OSError:
                    pass
        self.dirty.add(day)
        return rec

    def save(self, force=False):
        if not force and time.time() - self.saved < SAVE_SECONDS:
            return
        self.saved = time.time()
        for day in list(self.dirty):
            if day in self.days:
                try:
                    write_atomic(self.path(day), json.dumps(self.days[day], separators=(',', ':')))
                except OSError as e:
                    self.note('dashboard: %s not saved (%s)' % (day, e))
        self.dirty.clear()

    # ---- what happens
    def event(self, kind, t=None, **fields):
        t = t or time.time()
        rec = self.record(t)
        rec['events'].append(dict({k: v for k, v in fields.items() if v not in (None, '')}, t=int(t), kind=kind))
        del rec['events'][:-EVENTS_KEPT]

    def player(self, rec, name):
        p = rec['players'].get(name.lower())
        if p is None:
            p = rec['players'][name.lower()] = {'name': name, 's': 0, 'n': 0, 'maps': {}, 'srv': {}, 'c': {},
                                                'first': 0, 'last': 0}
        return p

    def tick(self, servers, processes=None, now=None, folders=None):
        """servers: [{'n', 'name', 'state', 'map', 'next_map', 'players', 'max', 'roster': [{'name', 'ping', ...}],
        'reachable', 'round'}]; processes: {n: {'game': pid, 'host': pid}}; folders: {n: its log folder}."""
        now = now or time.time()
        dt = 0.0 if self.last_tick is None else max(0.0, min(30.0, now - self.last_tick))
        self.last_tick = now
        seen = set()
        for sv in servers:
            n = sv['n']
            state = sv.get('state', 'stopped')
            was = self.states.get(n)
            if was is not None and was != state and state in ('running', 'stopped'):
                self.event('server_up' if state == 'running' else 'server_down', now, server=n, name=sv.get('name'))
            self.states[n] = state
            mp = sv.get('map') or ''
            if mp and self.maps.get(n) not in (None, mp) and state == 'running':
                self.event('map', now, server=n, map=mp)
            if mp:
                self.maps[n] = mp
            for r in sv.get('roster') or []:
                name = clean_name(r.get('name'))
                if not name:
                    continue
                key = (n, name.lower())
                seen.add(key)
                rec = self.record(now)
                p = self.player(rec, name)
                if key not in self.online:
                    self.online[key] = {'name': name, 'since': now}
                    p['n'] += 1
                    p['first'] = p['first'] or int(now)
                    self.event('join', now, server=n, name=name, map=mp)
                p['last'] = int(now)
                if dt:
                    p['s'] = round(p['s'] + dt, 1)
                    if mp:
                        p['maps'][mp] = round(p['maps'].get(mp, 0) + dt, 1)
                    p['srv'][str(n)] = round(p['srv'].get(str(n), 0) + dt, 1)
        for key in [k for k in self.online if k not in seen]:
            o = self.online.pop(key)
            self.event('leave', now, server=key[0], name=o['name'], map=self.maps.get(key[0], ''),
                       played=int(now - o['since']))
        self.read_stats(folders or {}, now)
        self.read_ticks(folders or {}, now)
        if now - self.sampled >= SAMPLE_SECONDS:
            self.sample(servers, processes or {}, now)
        self.live = {sv['n']: sv for sv in servers}
        self.save()

    def note_restart(self, n, why, now=None):
        """The manager started a server again after its game or host ended (a crash, most often)."""
        self.event('restart', now, server=n, why=why)

    def sample(self, servers, processes, now):
        self.sampled = now
        m = Usage.machine()
        machine = {}
        if m:
            busy, total, used, mem = m
            cpu = self.usage.percent('machine', now, busy, total)
            machine = {'cpu': None if cpu is None else round(cpu, 1), 'mem': used, 'mem_total': mem}
        procs = {}
        for n, pids in processes.items():
            entry = {}
            for role, pid in pids.items():
                u = Usage.process(pid) if pid else None
                if u:
                    cpu = self.usage.percent((n, role, pid), now, u[0])
                    entry[role] = {'cpu': None if cpu is None else round(cpu, 1), 'mem': u[1]}
            procs[str(n)] = entry
        self.machine = dict(machine, procs=procs, at=int(now))
        rec = self.record(now)
        rec['samples'].append({'t': int(now), 'p': {str(sv['n']): int(sv.get('players') or 0) for sv in servers
                                                     if sv.get('state') == 'running'},
                               'cpu': machine.get('cpu'), 'mem': machine.get('mem'),
                               'proc': {n: {r: [v.get('cpu'), v.get('mem')] for r, v in e.items()} for n, e in procs.items()}})

    def read_stats(self, folders, now):
        """Each server's player counts (server_stats.cpp writes lines of "player_stats" every 5 s: new counts only)."""
        for n, folder in folders.items():
            path = os.path.join(folder, 'stats.txt')
            try:
                size = os.path.getsize(path)
            except OSError:
                self.stats_at[n] = 0
                continue
            at = self.stats_at.get(n)
            if at is None:
                self.stats_at[n] = size  # what is there as the manager starts was counted before
                continue
            if size < at:
                at = 0  # the file started again
            if size == at:
                continue
            with open(path, 'rb') as f:
                f.seek(at)
                data = f.read(min(size - at, 1 << 20))
            end = data.rfind(b'\n') + 1
            self.stats_at[n] = at + end
            rec = self.record(now)
            for line in data[:end].splitlines():
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                for row in (m.get('rows') or [])[:64] if isinstance(m, dict) else []:
                    if not isinstance(row, dict):
                        continue
                    name = clean_name(row.get('name'))
                    if not name or name.upper().startswith('[BOT]'):
                        continue
                    p = self.player(rec, name)
                    for k in STAT_KEYS:
                        v = row.get(k)
                        if isinstance(v, (int, float)) and 0 < v <= 100000:
                            p['c'][k] = p['c'].get(k, 0) + int(v)

    def read_ticks(self, folders, now):
        """Each server's last "BF server ticks" line (server_rotation.cpp): the rate it held, its longest gap, its
        tick's own work."""
        if now - self.ticks_looked < 30:
            return
        self.ticks_looked = now
        for n, folder in folders.items():
            path = os.path.join(folder, 'server.log')
            try:
                size = os.path.getsize(path)
                with open(path, 'rb') as f:
                    f.seek(max(0, size - 65536))
                    tail = f.read().decode('utf-8', 'replace')
            except OSError:
                continue
            at = tail.rfind('BF server ticks: ')
            if at < 0:
                continue
            line = tail[at:tail.find('\n', at) if tail.find('\n', at) > 0 else None]
            out = {'line': line[:200]}
            try:
                out['rate'] = float(line.split('ticks: ', 1)[1].split(' ', 1)[0])
                if 'longest gap ' in line:
                    out['gap_ms'] = float(line.split('longest gap ', 1)[1].split(' ', 1)[0])
                if "own work " in line:
                    out['work_ms'] = float(line.split("own work ", 1)[1].split(' ', 1)[0])
            except (ValueError, IndexError):
                pass
            self.ticks[n] = out

    # ---- what the pages show
    def window(self, days, now):
        start = day_of(now - (days - 1) * 86400)
        return [self.days[d] for d in sorted(self.days) if d >= start]

    def leaderboard(self, days, now, most=25, server=None):
        totals = {}
        for rec in self.window(days, now):
            for key, p in rec['players'].items():
                t = totals.setdefault(key, {'name': p['name'], 's': 0, 'n': 0, 'maps': {}, 'c': {}, 'last': 0})
                s = p['srv'].get(str(server), 0) if server else p['s']
                t['s'] += s
                t['n'] += p['n']
                t['last'] = max(t['last'], p['last'])
                t['name'] = p['name']
                for m, v in p['maps'].items():
                    t['maps'][m] = t['maps'].get(m, 0) + v
                for k, v in p['c'].items():
                    t['c'][k] = t['c'].get(k, 0) + v
        rows = []
        for t in totals.values():
            if t['s'] < 60:
                continue
            kills, deaths = t['c'].get('kills', 0), t['c'].get('deaths', 0)
            rows.append({'name': t['name'], 'seconds': int(t['s']), 'sessions': t['n'], 'last': t['last'],
                         'map': max(t['maps'], key=t['maps'].get) if t['maps'] else '', 'kills': kills,
                         'deaths': deaths, 'kd': round(kills / deaths, 2) if deaths else (float(kills) if kills else 0.0),
                         'captures': t['c'].get('captures', 0)})
        rows.sort(key=lambda r: -r['seconds'])
        return rows[:most], len(totals)

    def series(self, hours, now, step=None):
        """Players over the last `hours`: [t, total, {server: players}], a point each `step` seconds (the busiest
        sample in it)."""
        step = step or max(SAMPLE_SECONDS, hours * 3600 // 144)
        start = now - hours * 3600
        buckets = {}
        for rec in self.window(int(hours // 24) + 2, now):
            for s in rec['samples']:
                if s['t'] < start:
                    continue
                b = int((s['t'] - start) // step)
                total = sum(s['p'].values())
                if b not in buckets or total > buckets[b][1]:
                    buckets[b] = [int(start + b * step), total, s['p']]
        return [buckets[b] for b in sorted(buckets)]

    def health_series(self, hours, now):
        start = now - hours * 3600
        out = []
        for rec in self.window(int(hours // 24) + 2, now):
            for s in rec['samples']:
                if s['t'] >= start:
                    out.append([s['t'], s.get('cpu'), s.get('mem')])
        step = max(1, len(out) // 240)
        return out[::step]

    def heatmap(self, days, now):
        """The average players on all the servers for each hour of each weekday (UTC) over the last `days`."""
        sums = [[0.0] * 24 for _ in range(7)]
        counts = [[0] * 24 for _ in range(7)]
        for rec in self.window(days, now):
            for s in rec['samples']:
                d = datetime.datetime.fromtimestamp(s['t'], datetime.timezone.utc)
                sums[d.weekday()][d.hour] += sum(s['p'].values())
                counts[d.weekday()][d.hour] += 1
        return [[round(sums[w][h] / counts[w][h], 1) if counts[w][h] else None for h in range(24)] for w in range(7)]

    def view(self, now=None, public=False, names=True):
        now = now or time.time()
        day = self.days.get(day_of(now), {'players': {}, 'samples': [], 'events': []})
        last24 = [s for rec in self.window(2, now) for s in rec['samples'] if s['t'] >= now - 86400]
        peak = max([sum(s['p'].values()) for s in last24] or [0])
        hours24 = sum(sum(s['p'].values()) for s in last24) * SAMPLE_SECONDS / 3600.0
        mvp = max(day['players'].values(), key=lambda p: p['s'], default=None)
        board, tracked = self.leaderboard(HISTORY_DAYS, now)
        week, _ = self.leaderboard(7, now, most=200)
        maps = {}
        for rec in self.window(7, now):
            for p in rec['players'].values():
                for m, v in p['maps'].items():
                    maps[m] = maps.get(m, 0) + v / 60.0
        events = [e for rec in self.window(2, now) for e in rec['events']][-60:][::-1]
        restarts = [e for rec in self.window(2, now) for e in rec['events']
                    if e['kind'] == 'restart' and e['t'] >= now - 86400]
        sessions24 = len([e for rec in self.window(2, now) for e in rec['events']
                          if e['kind'] == 'join' and e['t'] >= now - 86400])
        top_killer = max(week, key=lambda r: r['kills'], default=None)
        live = []
        for n, sv in sorted(self.live.items()):
            roster = []
            for r in sv.get('roster') or []:
                name = clean_name(r.get('name'))
                key = (n, name.lower())
                roster.append({'name': name if names else '', 'ping': r.get('ping'),
                               'since': int(self.online[key]['since']) if key in self.online else None})
            entry = {'n': n, 'name': sv.get('name', ''), 'state': sv.get('state'), 'map': sv.get('map', ''),
                     'next_map': sv.get('next_map', ''), 'players': sv.get('players', 0), 'max': sv.get('max', 0),
                     'tick': sv.get('tick'), 'round': sv.get('round'), 'since': sv.get('since', 0),
                     'region': sv.get('region', ''), 'roster': roster if names else [],
                     'held': self.ticks.get(n, {}).get('rate'), 'gap_ms': self.ticks.get(n, {}).get('gap_ms'),
                     'work_ms': self.ticks.get(n, {}).get('work_ms')}
            pings = [r.get('ping') for r in sv.get('roster') or [] if isinstance(r.get('ping'), (int, float))]
            entry['ping_avg'] = round(sum(pings) / len(pings)) if pings else None
            if not public:
                entry.update(reachable=sv.get('reachable'), why=sv.get('why', ''), wanted=sv.get('wanted'),
                             procs=self.machine.get('procs', {}).get(str(n), {}))
            live.append(entry)
        out = {
            'now': int(now),
            'cards': {'online': sum(int(sv.get('players') or 0) for sv in self.live.values()
                                    if sv.get('state') == 'running'),
                      'slots': sum(int(sv.get('max') or 0) for sv in self.live.values() if sv.get('state') == 'running'),
                      'peak24': peak, 'hours24': round(hours24, 1), 'sessions24': sessions24, 'tracked': tracked,
                      'mvp': {'name': mvp['name'], 'seconds': int(mvp['s'])} if mvp and names and mvp['s'] >= 60 else None,
                      'top_killer': {'name': top_killer['name'], 'kills': top_killer['kills']}
                      if top_killer and names and top_killer['kills'] else None,
                      'restarts24': len(restarts)},
            'servers': live,
            'series': {'6h': self.series(6, now), '24h': self.series(24, now), '7d': self.series(168, now)},
            'maps': {m: round(v) for m, v in sorted(maps.items(), key=lambda kv: -kv[1])},
            'heatmap': self.heatmap(30, now),
            'leaderboard': board if names else [],
            'events': [e for e in events if names or e['kind'] not in ('join', 'leave')][:60],
            'machine': {'cpu': self.machine.get('cpu'), 'mem': self.machine.get('mem'),
                        'mem_total': self.machine.get('mem_total')},
            'health': self.health_series(24, now),
            'network': self.network,
        }
        if public:
            for e in out['events']:
                e.pop('why', None)
        return out
