#!/usr/bin/env python3
"""Repeatable benchmark runs for a developer build (BF_DEV_TOOLS), on Windows and Linux. See README.md beside this.

  bench.py run [options]            a LAN server of our own on this PC (loopback only, unlisted), the game joins it by
                                    itself, measures a window of frames, closes both, writes summary.txt in the run folder
  bench.py report <run folder>      summary.txt again for a run
  bench.py compare <run folder>...  one line per run, to compare builds or settings
"""
import argparse
import datetime
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WINDOWS = os.name == 'nt'
PRESETS = ['Potato', 'Low', 'Medium', 'High', 'Ultra']  # the Graphics page's (game_menu.cpp GraphicsPresets)
LEVELS = {'wake': 'Levels/Wake_island_s', 'guadalcanal': 'Levels/Guadal_Canal', 'iwo': 'Levels/Iwo_Jima_s',
          'coral': 'Levels/Coral_sea'}


def find_build(name):
    """The build folder: a path, or a name under bf/out/build; by default the first dev build there is."""
    if name:
        p = Path(name)
        return p if p.is_absolute() or p.exists() else ROOT / 'bf' / 'out' / 'build' / name
    for n in ('bf-dev', 'win-amd64-release-dev', 'linux-amd64-release-dev', 'win-amd64-relwithdebinfo',
              'linux-amd64-relwithdebinfo'):
        p = ROOT / 'bf' / 'out' / 'build' / n
        if (p / exe_name()).exists():
            return p
    sys.exit('no build found under bf/out/build; give one with --build')


def exe_name():
    return 'bf1943.exe' if WINDOWS else 'bf1943'


def default_settings():
    """The player's settings file of a developer build (its default user folder)."""
    places = []
    if WINDOWS:
        for v in ('LOCALAPPDATA', 'APPDATA'):
            if os.environ.get(v):
                places.append(Path(os.environ[v]) / 'bf1943')
    else:
        places.append(Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local' / 'share')) / 'bf1943')
    for p in places:
        if (p / 'bf1943-settings.txt').exists():
            return p / 'bf1943-settings.txt'
    return None


def wait_for(path, text, seconds, alive=None):
    """Wait until the log at `path` holds `text`; False after `seconds` or when the process `alive` ended."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            if text in path.read_text(errors='replace'):
                return True
        except OSError:
            pass
        if alive is not None and alive.poll() is not None:
            return False
        time.sleep(1)
    return False


def line_count(path):
    try:
        with open(path, 'rb') as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


def cpu_snapshot(pid):
    """Per-thread processor time (seconds) now. Linux: by thread name from /proc; elsewhere the process's whole time
    when psutil is installed; else nothing."""
    out = {'t': time.monotonic(), 'threads': {}}
    task = Path(f'/proc/{pid}/task')
    if task.exists():
        hz = os.sysconf('SC_CLK_TCK')
        for t in task.iterdir():
            try:
                s = (t / 'stat').read_text()
            except OSError:
                continue
            name = s[s.index('(') + 1:s.rindex(')')]
            f = s[s.rindex(')') + 2:].split()
            out['threads'][t.name] = {'name': name, 'seconds': (int(f[11]) + int(f[12])) / hz}
        return out
    try:
        import psutil
        c = psutil.Process(pid).cpu_times()
        out['threads']['all'] = {'name': 'bf1943 (all threads)', 'seconds': c.user + c.system}
    except Exception:
        pass
    return out


def stop(proc):
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.send_signal(signal.CTRL_BREAK_EVENT if WINDOWS else signal.SIGTERM)
        proc.wait(5)
    except Exception:
        proc.kill()


def run(a):
    """One run, or one per graphics preset (--preset), then their comparison."""
    presets = [None]
    if a.preset:
        presets = PRESETS if a.preset.lower() == 'all' else [p.strip() for p in a.preset.split(',') if p.strip()]
        for p in presets:
            if p.lower() not in [q.lower() for q in PRESETS]:
                sys.exit(f'no graphics preset {p} ({", ".join(PRESETS)} or all)')
    dirs = []
    for preset in presets:
        dirs.append(run_one(a, preset))
    if len(dirs) > 1:
        print()
        compare(dirs)


def run_one(a, preset):
    build = find_build(a.build)
    exe = build / exe_name()
    if not exe.exists():
        sys.exit(f'no {exe_name()} in {build}')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    name = f'bench-{stamp}-{a.map}-{a.mode}' + (f'-{preset.lower()}' if preset else '')
    out = Path(a.out) / name if a.out else ROOT / 'runs' / name
    (out / 'user').mkdir(parents=True, exist_ok=True)
    (out / 'server-user').mkdir(parents=True, exist_ok=True)
    print(out)
    # The player's settings, a copy (never written back): the game's own options for the run.
    settings = Path(a.settings) if a.settings else default_settings()
    if settings and settings.exists():
        shutil.copy(settings, out / 'user' / 'bf1943-settings.txt')
    else:
        print('no settings file found (give one with --settings): the game starts with its defaults', file=sys.stderr)
    if a.mode == 'uncapped':  # our menu applies its settings over the command line: set them there too
        f = out / 'user' / 'bf1943-settings.txt'
        lines = [l for l in (f.read_text().splitlines() if f.exists() else [])
                 if not l.startswith(('frame_rate = ', 'vsync = '))]
        f.write_text('\n'.join(lines + ['frame_rate = unlimited', 'vsync = off']) + '\n')
    game = Path(a.game) if a.game else (build / 'game' if (build / 'game').exists() else None)
    common = ['--fullscreen=false'] + ([f'--game_data_root={game}'] if game else [])
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if WINDOWS else 0

    env = dict(os.environ, BF_BENCH_DIR=str(out), BF_LOOPBACK_ONLY='1')
    if a.explode:  # the server blows up what the game's free camera asks for (trigger.txt)
        env['BF_BENCH_EXPLODE'] = 'view'
        env['BF_BENCH_EXPLODE_VIEW'] = a.explode
    server_args = [str(exe), *common, f'--user_data_root={out / "server-user"}', f'--log_file={out / "server.log"}',
                   '--window_start=hidden', '--server', f'--cl=-dedicated -level {LEVELS[a.map]}',
                   f'--bf_map_rotation={a.map}', '--bf_round_end_delay=45', '--bf_gm_backend=127.0.0.1:18250',
                   '--bf_gm_listen=127.0.0.1:18300', '--bf_gm_name=bench', '--bf_gm_description=[player host]',
                   '--bf_gm_listed=false', '--bf_gm_upnp=false', '--bf_lan_backend=true', f'--bf_bots={a.bots}']
    if a.storm is not None:
        server_args += [f'--bf_world_weather={a.storm}', '--bf_world_cloud_cover=1', f'--bf_world_wind={a.storm}']
    server_args += a.server_arg
    server = client = None
    try:
        with open(out / 'server-stdout.txt', 'w') as so:
            server = subprocess.Popen(server_args, cwd=build, env=env, stdout=so, stderr=subprocess.STDOUT,
                                      creationflags=flags)
        if not wait_for(out / 'server.log', 'registered with the backend', 120, server):
            sys.exit('the server never registered (server.log)')
        # The props' scan holds the server's thread for 5 to 10 s: before the game joins, or the server drops it.
        if a.explode and not wait_for(out / 'server.log', 'BF explode: scan done', 120, server):
            sys.exit('the server never finished its scan of the props (server.log)')

        cenv = dict(env, BF_AUTO_JOIN='1', BF_FRAME_STATS='1', BF_FRAME_TIMES=str(out / 'frame_times.txt'))
        if preset:  # the game applies it to the run's copy of the settings at its start (game_menu.cpp)
            cenv['BF_BENCH_PRESET'] = preset
        if a.freecam:
            cenv['BF_BENCH_FREECAM'] = a.freecam
            a.turn = 0  # the free camera instead of the soldier's view
        elif a.turn:
            cenv['BF_BENCH_TURN'] = f'{a.turn_at},{a.turn}'
        client_args = [str(exe), *common, f'--user_data_root={out / "user"}', f'--log_file={out / "bf1943.log"}',
                       '--bf_online_host=127.0.0.1', '--native_x_renderer=true', '--bf_engine_d3d_path=true',
                       f'--native_x_renderer_hitch_ms={a.hitch_ms}']
        if a.mode == 'uncapped':
            client_args += ['--bf_max_fps=0', '--vsync=false']
        client_args += a.arg
        with open(out / 'stdout.txt', 'w') as co:
            client = subprocess.Popen(client_args, cwd=build, env=cenv, stdout=co, stderr=subprocess.STDOUT,
                                      creationflags=flags)
        if not wait_for(out / 'bf1943.log', 'level loaded, spawning', 240, client):
            sys.exit('the game never reached the level (bf1943.log)')
        duration = a.duration if a.duration is not None else ('lap' if a.freecam else '60')
        if duration == 'lap':
            # One lap of the free camera's path, from its take off (its log line gives the lap's length; the
            # --freecam delay is the settling time).
            if not a.freecam:
                sys.exit('--duration lap needs --freecam')
            if not wait_for(out / 'bf1943.log', 'BF bench: free camera on', 300, client):
                sys.exit('the free camera never took the view (bf1943.log)')
            m = re.search(r'a lap of ([\d.]+) s', (out / 'bf1943.log').read_text(errors='replace'))
            seconds = float(m.group(1)) if m else 60.0
            print(f'the free camera took off; measuring one lap, {seconds:.0f} s')
        else:
            seconds = float(duration)
            print(f'spawning; settling {a.settle} s, then measuring {seconds:.0f} s')
            time.sleep(a.settle)
        if client.poll() is not None:
            sys.exit('the game ended early')
        f0, w0, c0 = line_count(out / 'frame_times.txt'), time.time(), cpu_snapshot(client.pid)
        time.sleep(seconds)
        f1, w1, c1 = line_count(out / 'frame_times.txt'), time.time(), cpu_snapshot(client.pid)
        meta = {k: v for k, v in vars(a).items() if k not in ('func', 'arg', 'server_arg')}
        meta['duration'] = round(seconds, 1)
        label = a.label or f'{build.name}-{a.mode}'
        meta.update(build=str(build.name), label=f'{label}-{preset}' if preset else label, preset=preset or '',
                    frames0=f0, frames1=f1, wall0=w0, wall1=w1)
        (out / 'bench.json').write_text(json.dumps(meta, indent=1))
        (out / 'cpu.json').write_text(json.dumps({'before': c0, 'after': c1}))
    finally:
        stop(client)
        stop(server)
    report(out)
    return out


def pct(sorted_vals, p):
    return sorted_vals[min(len(sorted_vals) - 1, int(p * len(sorted_vals)))]


def log_times(path, pattern, t0, t1):
    """The lines of a log matching `pattern` between two wall clock times (the log's local time stamps)."""
    out = []
    if not path.exists():
        return out
    rx = re.compile(pattern)
    for line in open(path, errors='replace'):
        m = re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]', line)
        if not m or not rx.search(line):
            continue
        t = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f').timestamp()
        if t0 <= t <= t1:
            out.append(line)
    return out


def summarize(d):
    d = Path(d)
    m = json.loads((d / 'bench.json').read_text())
    out = {k: ('' if m.get(k) is None else m.get(k)) for k in
           ('label', 'build', 'map', 'mode', 'preset', 'turn', 'freecam', 'bots', 'storm', 'explode')}
    ft = [float(x) for x in open(d / 'frame_times.txt') if x.strip()] if (d / 'frame_times.txt').exists() else []
    win = ft[m['frames0']:m['frames1']]
    if not win:
        out['error'] = 'no frames in the window'
        return out, []
    s, total = sorted(win), sum(win)
    n1, n01 = max(1, len(s) // 100), max(1, len(s) // 1000)
    out.update(frames=len(win), seconds=round(total / 1000, 1), fps=round(len(win) / (total / 1000), 1),
               low1_fps=round(1000 / (sum(s[-n1:]) / n1), 1), low01_fps=round(1000 / (sum(s[-n01:]) / n01), 1),
               p50_ms=round(pct(s, .5), 2), p99_ms=round(pct(s, .99), 2), p999_ms=round(pct(s, .999), 2),
               max_ms=round(s[-1], 1))
    for th in (25, 33.4, 50, 100):
        out[f'over{int(th)}'] = sum(x > th for x in win)
    # The renderer's own long frame lines (native_x_renderer_hitch_ms) and DiligentCore's GPU memory pages made or
    # freed, in the window.
    out['renderer_hitches'] = len(log_times(d / 'bf1943.log', r'\] hitch: frame', m['wall0'], m['wall1']))
    out['gpu_page_events'] = len(log_times(d / 'bf1943.log', r'MemoryManager.*page', m['wall0'], m['wall1']))
    out['explosions'] = len(log_times(d / 'server.log', r'BF explode: \d+ of', m['wall0'], m['wall1']))
    threads = []
    if (d / 'cpu.json').exists():
        c = json.loads((d / 'cpu.json').read_text())
        a, b = c['before'], c['after']
        secs = b['t'] - a['t']
        by_name = {}
        for tid, th in b['threads'].items():
            before = a['threads'].get(tid, {'seconds': 0})['seconds']
            name = re.sub(r'\s*\(F[0-9A-F]*\)?$', '', th['name'])  # the guest thread's id, cut at 15 characters
            by_name[name] = by_name.get(name, 0) + (th['seconds'] - before) / secs * 100
        if by_name:
            out['cpu_total_cores'] = round(sum(by_name.values()) / 100, 2)
            threads = sorted(by_name.items(), key=lambda kv: -kv[1])[:10]
    return out, threads


def report(d):
    out, threads = summarize(d)
    lines = [f'{k}: {v}' for k, v in out.items()]
    if threads:
        lines.append('CPU by thread (% of one core over the window):')
        lines += [f'  {pc:6.1f}  {name}' for name, pc in threads]
    text = '\n'.join(lines) + '\n'
    (Path(d) / 'summary.txt').write_text(text)
    print(text, end='')


def compare(dirs):
    keys = ['label', 'mode', 'preset', 'freecam', 'bots', 'storm', 'explode', 'fps', 'low1_fps', 'low01_fps', 'p99_ms', 'p999_ms',
            'max_ms', 'over33', 'over50', 'renderer_hitches', 'gpu_page_events', 'explosions', 'cpu_total_cores']
    rows = [[str(summarize(d)[0].get(k, '')) for k in keys] + [Path(d).name] for d in dirs]
    widths = [max(len(r[i]) for r in rows + [keys + ['run']]) for i in range(len(keys) + 1)]
    for r in [keys + ['run']] + rows:
        print('  '.join(v.ljust(w) for v, w in zip(r, widths)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run', help='one benchmark run')
    r.add_argument('--build', help='build folder, or its name under bf/out/build (default: the first dev build found)')
    r.add_argument('--map', choices=LEVELS, default='wake')
    r.add_argument('--mode', choices=('capped', 'uncapped'), default='uncapped',
                   help='capped: the player\'s frame limit and vsync; uncapped (default): no limit, no vsync')
    r.add_argument('--settle', type=float, default=60,
                   help='seconds after the spawn before measuring (default 60; not with --duration lap)')
    r.add_argument('--duration', help='seconds measured, or lap: one lap of the free camera\'s path from its take off '
                   '(the default with --freecam; otherwise 60)')
    r.add_argument('--label', help='a name for the run in comparisons')
    r.add_argument('--turn', type=float, default=0.5,
                   help='the soldier turns on the spot at this stick strength (default 0.5; 0 stands still)')
    r.add_argument('--turn-at', type=float, default=30, help='seconds after joining the turn starts (default 30)')
    r.add_argument('--freecam', help='<s after joining>[,orbit|flags|props[,<s a lap | m/s>]]: the game\'s free camera '
                   'on a timed path instead of the soldier (bench_freecam.cpp)')
    r.add_argument('--explode', help='<metres>[,<half angle>]: the level\'s explosive props blow up in the free '
                   'camera\'s view (default 25 degrees each side) within that distance, or as near as its path brings '
                   'them (bench_freecam.cpp, bench_explode.cpp)')
    r.add_argument('--bots', type=int, default=0, help='bots on the server (bots.cpp)')
    r.add_argument('--storm', type=float, help='the match\'s weather 0..1 (1: a full storm), clouds and wind with it')
    r.add_argument('--preset', help='graphics preset(s): one of ' + ', '.join(PRESETS) + ', a list (Low,High) or all: '
                   'a run each, then their comparison (applied to the run\'s copy of the settings)')
    r.add_argument('--hitch-ms', type=int, default=40, help='the renderer logs frames longer than this (default 40)')
    r.add_argument('--settings', help='the bf1943-settings.txt to copy (default: the dev build\'s user folder)')
    r.add_argument('--game', help='the game folder (default: <build>/game when there)')
    r.add_argument('--out', help='the folder for the run folders (default runs/)')
    r.add_argument('--arg', action='append', default=[], help='one more option for the game (repeatable)')
    r.add_argument('--server-arg', action='append', default=[], help='one more option for the server (repeatable)')
    r.set_defaults(func=run)
    rp = sub.add_parser('report', help='summary.txt again for a run')
    rp.add_argument('dir')
    rp.set_defaults(func=lambda a: report(a.dir))
    c = sub.add_parser('compare', help='one line per run')
    c.add_argument('dirs', nargs='+')
    c.set_defaults(func=lambda a: compare(a.dirs))
    a = p.parse_args()
    a.func(a)


if __name__ == '__main__':
    main()
