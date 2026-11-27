#!/usr/bin/env python3
"""A player's bf1943.log in one screen (Settings > Diagnostics, bf/src/diagnostics*.cpp).

    python3 tools/bf_log_summary.py bf1943.log [--all]

Prints: the build, the run's summary lines (written when the game closed), a table of the scenes, the marks and the
black box's dumps with what their slow frames most likely were, the warnings and errors by kind, and the health,
ghost, movement and network lines worth a look. --all lists every scene instead of the slowest and the longest.
A log cut short (a crash, the process killed) has no summary lines: the rest is worked out from the log itself.
"""
import argparse
import collections
import re
import sys

LINE = re.compile(r"^\[(?P<time>[\d-]+ [\d:.]+)\] \[(?P<level>\w+)\] \[\w+\] \[t\d+\] (?P<msg>.*)$")
SCENE_END = re.compile(
    r"BF scene: (?P<scene>.+?) ended after (?P<dur>\S+)(?: \(the game closed\))?(?:; the last \S+)?: (?P<rest>.*)")
FRAMES = re.compile(r"(?P<frames>\d+) frames \((?P<fps>[\d.]+)/s\), (?P<over>\d+) over 50 ms \(longest (?P<longest>\d+) ms\)")
LOW = re.compile(r"1% low (?P<low1>\d+) FPS")
DUMP = re.compile(r"BF black box(?: \[(?P<scene>[^\]]+)\])?: (?P<why>.+?); the last [\d.]+ s")
CAUSE = re.compile(r"BF black box: (?P<at>[-+][\d.]+) s: game (?P<ms>[\d.]+) ms.*most likely: (?P<cause>.*)$")


def kind(msg):
    return re.sub(r"[0-9A-Fa-f]*\d[0-9A-Fa-f]*", "#", msg)[:100]


def seconds(text):
    m = re.match(r"(?:(\d+)h)?(?:(\d+)m)?(?:([\d.]+)s)?$", text)
    if not m:
        return 0.0
    h, mi, s = m.groups()
    return int(h or 0) * 3600 + int(mi or 0) * 60 + float(s or 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--all", action="store_true", help="every scene, not only the notable ones")
    args = ap.parse_args()

    build = None
    first = last = None
    summary, scenes, marks, dumps, causes = [], [], [], [], []
    warnings = collections.Counter()
    notable = collections.defaultdict(list)
    current_dump = None
    closed = False
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.match(raw.rstrip("\n"))
            if not m:
                continue
            t, level, msg = m.group("time"), m.group("level"), m.group("msg")
            first = first or t
            last = t
            if "(the game closed)" in msg or msg.startswith("Title terminated"):
                closed = True
            if level in ("warning", "error", "critical"):
                warnings[kind(msg)] += 1
            if msg.startswith("BF build: "):
                build = msg[len("BF build: "):]
            elif msg.startswith("BF summary: "):
                summary.append(msg[len("BF summary: "):])
            elif msg.startswith("BF scene: ") and " ended after " in msg:
                s = SCENE_END.match(msg)
                if s:
                    fr = FRAMES.search(s.group("rest"))
                    low = LOW.search(s.group("rest"))
                    scenes.append({
                        "time": t[11:19], "scene": s.group("scene"), "secs": seconds(s.group("dur")),
                        "fps": float(fr.group("fps")) if fr else 0.0, "over": int(fr.group("over")) if fr else 0,
                        "longest": int(fr.group("longest")) if fr else 0,
                        "low1": int(low.group("low1")) if low else None, "rest": s.group("rest")})
            elif msg.startswith("BF mark"):
                marks.append((t[11:19], msg))
            elif msg.startswith("BF black box") and "; the last " in msg and "dump " in msg:
                d = DUMP.match(msg)
                current_dump = {"time": t[11:19], "why": d.group("why") if d else msg, "frames": []}
                dumps.append(current_dump)
            elif msg.startswith("BF black box: ") and "most likely: " in msg and current_dump is not None:
                c = CAUSE.match(msg)
                if c:
                    current_dump["frames"].append((float(c.group("ms")), c.group("cause")))
                    causes.append(c.group("cause"))
            elif msg.startswith(("BF ghosts", "BF movement", "BF health")) and level != "info" or \
                    msg.startswith(("BF ghosts", "BF movement", "BF join", "BF hang", "BF crash report:")):
                notable[msg.split(":")[0].split(" [")[0]].append(f"{t[11:19]} {msg}")

    print(f"Log {args.log}: {first} to {last}")
    print(f"Build: {build or 'no BF build line (a log from before 27 Nov 2026)'}")
    if summary:
        print("\nThe run's summary (written at exit):")
        for s in summary:
            print(f"  {s}")
    elif not build:
        print("\nNo summary lines: a log from before 27 Nov 2026's summary.")
    elif closed:
        print("\nNo summary lines, though the game closed: the summary failed (a bug worth reporting).")
    else:
        print("\nNo summary lines: the game did not close normally (a crash, a hang, or the process was killed).")

    if scenes:
        print(f"\nScenes: {len(scenes)}")
        shown = scenes if args.all else sorted(scenes, key=lambda s: (-s["over"], -s["longest"]))[:8]
        if not args.all:
            print("  the 8 with the most frames over 50 ms (--all for every scene):")
        for s in sorted(shown, key=lambda s: s["time"]):
            low = f", 1% low {s['low1']} FPS" if s["low1"] is not None else ""
            print(f"  {s['time']} {s['scene'][:40]:40} {s['secs']:7.0f} s  {s['fps']:5.1f} FPS{low}, "
                  f"{s['over']} over 50 ms (longest {s['longest']} ms)")

    if marks:
        print(f"\nMarks (the player's F10): {len(marks)}")
        for t, m in marks:
            print(f"  {t} {m}")
    if dumps:
        print(f"\nBlack box dumps: {len(dumps)}")
        for d in dumps:
            worst = max(d["frames"], default=None)
            what = f"; its worst frame {worst[0]:.0f} ms: {worst[1]}" if worst else ""
            print(f"  {d['time']} {d['why']}{what}")
        if causes:
            print("  slow frames in all dumps by cause:")
            for c, n in collections.Counter(kind(c) for c in causes).most_common():
                print(f"    {n:4}x {c}")

    if warnings:
        print(f"\nWarnings and errors: {sum(warnings.values())} lines of {len(warnings)} kinds; the most:")
        for k, n in warnings.most_common(12):
            print(f"  {n:6}x {k}")

    for name, lines in notable.items():
        print(f"\n{name}: {len(lines)} line(s)")
        for line in lines[:6]:
            print(f"  {line[:220]}")
        if len(lines) > 6:
            print(f"  ... {len(lines) - 6} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
