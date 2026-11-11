"""Camera speed and repeated frames from a run's camera log (REX_DEV_CAMERA_LOG in our SDK's GPU plugin).

    python -I tools/camera_speed.py runs/<name>/bf1943.log [--from <s>] [--seconds <s>] [--walk]

Each "Camera log: frame N view w a b c d" line is one presented frame's view projection W column (it moves with
the camera). Prints, over the window: frames, frames per second, the camera's speed in units per second (path
length of the W column over time) and how many frames repeated the previous frame's camera exactly. With --walk the
window starts --from seconds after the walk test began (BF_WALK_TEST's "BF walk test" line), so runs at different
frame rates are measured over the same part of the walk.
"""
import argparse
import datetime
import math
import re

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--from', dest='start', type=float, default=-8.0, help='window start, seconds (negative: from the end)')
ap.add_argument('--seconds', type=float, default=6.0)
ap.add_argument('--walk', action='store_true', help='--from counts from the start of the walk test')
a = ap.parse_args()

rows = []
walk = None
for line in open(a.log, encoding='utf-8', errors='replace'):
    # The walk's start: auto join's walk test, or a dev pad script holding the left stick (lsr lsl lsu lsd).
    w = re.match(r'\[(\S+ \S+)\].*BF walk test:', line)
    d = re.match(r'\[(\S+ \S+)\].*BF dev pad: \S+ s buttons ([0-9A-F]+)', line)
    if d and int(d.group(2), 16) & 0x3C0000:
        w = d
    if w and walk is None:
        walk = datetime.datetime.strptime(w.group(1), '%Y-%m-%d %H:%M:%S.%f').timestamp()
    m = re.match(r'\[(\S+ \S+)\].*Camera log: frame (\d+) view w (\S+) (\S+) (\S+) (\S+)', line)
    if m:
        t = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f').timestamp()
        rows.append((t, int(m.group(2)), [float(m.group(i)) for i in range(3, 7)]))
if not rows:
    raise SystemExit('no camera log lines')
if a.walk:
    if walk is None:
        raise SystemExit('no walk test line')
    begin = walk + a.start
else:
    begin = rows[0][0] + a.start if a.start >= 0 else rows[-1][0] + a.start
window = [r for r in rows if begin <= r[0] < begin + a.seconds]
if len(window) < 3:
    raise SystemExit('window too short')
path, repeats = 0.0, 0
for (_, _, p), (_, _, q) in zip(window, window[1:]):
    step = math.dist(p[:3], q[:3])
    path += step
    if step == 0.0:
        repeats += 1
span = window[-1][0] - window[0][0]
# The straight line from the first frame to the last: the path length grows with the frame rate when the camera
# bobs (finer sampling of the same curve), the net distance does not.
net = math.dist(window[0][2][:3], window[-1][2][:3])
# Jumps: steps over four times the median step, what a server correction (rubber banding) looks like; the view's bob
# alone stays well under that. (Per frame steps, not per second: the log's millisecond times are too coarse.)
rates = sorted(math.dist(p[:3], q[:3]) for (_, _, p), (_, _, q) in zip(window, window[1:]))
median = rates[len(rates) // 2]
jumps = sum(1 for r in rates if r > 4 * median)
print('frames %d over %.2f s: %.1f FPS, camera %.3f units/s (net %.3f), %d repeated frames (%.1f%%), %d jumps' % (
    len(window), span, (len(window) - 1) / span, path / span, net / span, repeats, 100.0 * repeats / (len(window) - 1),
    jumps))
