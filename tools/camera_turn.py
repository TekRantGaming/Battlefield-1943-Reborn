"""How evenly the drawn camera turns, from a run's camera log (REX_DEV_CAMERA_LOG, our SDK's D3D12 Xenos plugin) while
the stick turns the soldier (BF_WALK_TEST with BF_WALK_STICK=rx).

    python -I tools/camera_turn.py runs/<name>/bf1943.log [--skip-end 100] [--frames 1400]

The view projection's W column turns with the camera; its angle in the x, z plane is taken per frame. Prints the
frames that repeat the last frame's angle (the 30 Hz tick shows through), the frames that step backwards, and the
median turn per second.
"""
import argparse
import datetime
import math
import re
import statistics

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--skip-end', type=int, default=100, help='frames left out at the end (the run being stopped)')
ap.add_argument('--frames', type=int, default=1400)
a = ap.parse_args()

rows = []
for line in open(a.log, encoding='utf-8', errors='replace'):
    m = re.match(r'\[(\S+ \S+)\].*Camera log: frame (\d+) view w (\S+) (\S+) (\S+) (\S+)', line)
    if m:
        t = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f').timestamp()
        rows.append((t, int(m.group(2)), float(m.group(3)), float(m.group(5))))
rows = rows[-(a.frames + a.skip_end):-a.skip_end]
steps = []
for p, q in zip(rows, rows[1:]):
    d = math.atan2(q[3], q[2]) - math.atan2(p[3], p[2])
    steps.append(((d + math.pi) % (2 * math.pi) - math.pi, q[0] - p[0], q[1]))
secs = rows[-1][0] - rows[0][0]
repeats = sum(1 for d, _, _ in steps if abs(d) < 1e-6)
back = [(f, round(d, 4)) for d, _, f in steps if d < -1e-6]
rates = [d / dt for d, dt, _ in steps if dt > 0.002]
print(f'{len(rows)} frames over {secs:.1f} s ({len(rows) / secs:.1f} FPS): {repeats} repeated, {len(back)} backward, '
      f'median {statistics.median(rates):.3f} rad/s')
if back:
    print('backward:', back[:12])
