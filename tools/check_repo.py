"""Fail when a file that must never be in git is staged (START-HERE.md: no game files, no generated code).
Checks: forbidden extensions, generated or input folders, and any file over 1 MB.

    python -I tools/check_repo.py        (exit 1 and a list when something is wrong)
"""
import subprocess
import sys

FORBIDDEN_EXT = ('.xex', '.fbrb', '.zz', '.bin', '.pdb', '.png', '.jpg', '.dds', '.xma', '.wav', '.res', '.dbx',
                 '.exe', '.dll', '.obj', '.lib', '.map', '.pyc', '.key')  # .key: official.key (SERVERS-PLAN.md 7)
FORBIDDEN_DIRS = ('input/', 'Game Files/', 'bf/generated/', 'bf/assets', 'bf/out/', 'generated/', 'ghidra/',
                  'build/', 'runs/', 'sdk/')
LIMIT = 1 << 20


def main():
    out = subprocess.run(['git', 'diff', '--cached', '--name-only', '-z'], capture_output=True, check=True).stdout
    bad = []
    for name in filter(None, out.decode('utf-8').split('\0')):
        lower = name.lower()
        # The launcher's button glyphs: CC0 prompt pictures (Xelu's and Kenney's, bf/glyphs/LICENSE.txt), never the
        # game's; the only pictures allowed.
        if lower.startswith('bf/glyphs/') and lower.endswith('.png'):
            pass
        # TekRantGaming's own TRG Studios logo (his studio's, the startup card's ink only; MakeTrgCard lays it on the
        # player's own card paper).
        elif lower == 'bf/data/startup/trg_studios.png':
            pass
        # The Linux AppImage's icon: drawn from text by tools/linux/make_icon.py (no game art).
        elif lower == 'bf/data/linux/bf1943-reborn.png':
            pass
        elif lower.endswith(FORBIDDEN_EXT) or lower.startswith(tuple(d.lower() for d in FORBIDDEN_DIRS)):
            bad.append(name)
            continue
        size = len(subprocess.run(['git', 'show', ':' + name], capture_output=True).stdout)
        if size > LIMIT:
            bad.append('%s (%d bytes)' % (name, size))
    for b in bad:
        print('not allowed in git:', b)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
