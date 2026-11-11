"""List the generated functions that call a function or import, with how many call sites each has, and the size of
each caller in instructions (small callers are usually the XDK's thin wrappers).

    python -I tools/callers.py XamUserGetSigninState sub_82599398 ...
"""
import glob
import os
import re
import sys


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    names = sys.argv[1:]
    found = {n: {} for n in names}
    sizes = {}
    for f in glob.glob(os.path.join(root, 'bf', 'generated', 'default', '*_recomp.*.cpp')):
        fn = None
        for line in open(f):
            m = re.match(r'DEFINE_REX_FUNC\((\w+)\)', line)
            if m:
                fn = m.group(1)
                sizes[fn] = 0
                continue
            if fn and line.startswith('\t// '):
                sizes[fn] += 1
            for n in names:
                if (f'__imp__{n}(ctx, base)' in line or f'\t{n}(ctx, base)' in line) and fn:
                    found[n][fn] = found[n].get(fn, 0) + 1
    for n in names:
        print(n)
        for fn, count in sorted(found[n].items()):
            print('  %-14s %2d call%s, %d instructions' % (fn, count, '' if count == 1 else 's', sizes.get(fn, 0)))


if __name__ == '__main__':
    main()
