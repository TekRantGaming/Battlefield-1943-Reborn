"""Which draws change from one frame to the next, from a run's frame log (REX_DEV_FRAME_LOG=<s>,<frames> with our
SDK's "vsc" vertex constants hash on each draw line).

    python -I tools/frame_changes.py runs/<name>/bf1943.log

Draws are matched between consecutive frames by pixel shader, vertex shader, index count and how many times that
combination came before in the frame. For each matched draw the tool counts the frame to frame steps where its
vertex constants (transforms, bones) changed. A draw stepped by the game's 30 Hz ticks changes on about 30/FPS of
the steps; one updated every frame changes on all of them; a static one on none.
"""
import argparse
import collections
import re

ap = argparse.ArgumentParser()
ap.add_argument('log')
a = ap.parse_args()

frames, current = [], None
for line in open(a.log, encoding='utf-8', errors='replace'):
    if 'Frame log: frame start' in line or (current is None and 'Frame log: draw ' in line):
        current = []
    m = re.search(r'Frame log: draw \d+ ps (\w+) vs (\w+) .* count (\d+) vsc (\w+)', line)
    if m and current is not None:
        current.append(((m.group(1), m.group(2), m.group(3)), m.group(4)))
    if 'Frame log: frame end' in line and current is not None:
        frames.append(current)
        current = []
if len(frames) < 2:
    raise SystemExit('need two frames or more, found %d' % len(frames))


def keyed(frame):
    seen, out = collections.Counter(), {}
    for key, vsc in frame:
        out[key + (seen[key],)] = vsc
        seen[key] += 1
    return out


steps = collections.defaultdict(lambda: [0, 0])  # key: [steps seen, steps changed]
for prev, cur in zip(map(keyed, frames), map(keyed, frames[1:])):
    for key in prev.keys() & cur.keys():
        steps[key][0] += 1
        steps[key][1] += prev[key] != cur[key]
full = len(frames) - 1
tracked = [k for k, (n, _) in steps.items() if n == full]
always = sum(1 for k in tracked if steps[k][1] == full)
never = sum(1 for k in tracked if steps[k][1] == 0)
print('%d frames, %d draws a frame; %d draws in every frame: %d changed on every step, %d never, %d sometimes' % (
    len(frames), sum(map(len, frames)) // len(frames), len(tracked), always, never, len(tracked) - always - never))
some = sorted((steps[k][1], k) for k in tracked if 0 < steps[k][1] < full)
for changed, k in some[:12]:
    print('  sometimes: changed %d of %d steps  ps %s vs %s count %s #%d' % ((changed, full) + k))
