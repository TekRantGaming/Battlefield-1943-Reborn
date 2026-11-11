"""Turn rates from runs with BF_DEV_LOOK (bf/src/look.cpp): the mouse's angle per count and the stick's turn speed,
and how many frames showed a new view yaw, for comparing frame rates.

    python -I tools/look_rates.py runs/lr30/bf1943.log runs/lr60/bf1943.log ...

A run's mouse phase is its seconds with test mouse counts and no stick turn (BF_DEV_MOUSE), its stick phase the
seconds with the game's own turn and no mouse (BF_WALK_TEST with BF_WALK_STICK=rx). The first and last second of each
phase are left out (they are partly outside it).
"""
import re
import sys

LINE = re.compile(r'BF look: ([\d.]+) FPS, (\d+) aim updates, (\d+) frames with a new view yaw; yaw ([-\d.]+) rad/s '
                  r"\(game's own ([-\d.]+)\), pitch ([-\d.]+) rad/s \(game's own ([-\d.]+)\), mouse ([-\d.]+) ([-\d.]+) "
                  r'counts, ([-\d.]+) rad per count')


def phases(rows):
    mouse = [r for r in rows if r['counts'] > 0 and abs(r['game']) < 1e-3]
    stick = [r for r in rows if r['counts'] == 0 and abs(r['game']) > 0.1]
    return mouse[1:-1], stick[1:-1]


print(f"{'run':28} {'FPS':>6} {'new yaw':>8} {'rad/count':>11} {'stick rad/s':>12} {'game rad/s':>11} {'secs':>8}")
for path in sys.argv[1:]:
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if m and int(m.group(2)) > 0:
            rows.append({'fps': float(m.group(1)), 'ticks': int(m.group(2)), 'new': int(m.group(3)),
                         'yaw': float(m.group(4)), 'game': float(m.group(5)), 'counts': float(m.group(8)), 'rpc': float(m.group(10))})
    mouse, stick = phases(rows)
    both = mouse + stick
    fps = sum(r['fps'] for r in both) / max(len(both), 1)
    new = sum(r['new'] for r in both) / max(sum(r['fps'] for r in both), 1)
    # The logged angle per count (its row's turn over its row's counts), weighted by counts.
    per_count = sum(r['rpc'] * r['counts'] for r in mouse) / max(sum(r['counts'] for r in mouse), 1)
    stick_rate = sum(r['yaw'] for r in stick) / max(len(stick), 1)
    game_rate = sum(r['game'] for r in stick) / max(len(stick), 1)
    print(f"{path:28} {fps:6.1f} {new:8.1%} {per_count:11.7f} {stick_rate:12.4f} {game_rate:11.4f} "
          f"{len(mouse):3d} {len(stick):3d}")
