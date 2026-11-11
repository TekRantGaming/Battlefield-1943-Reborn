"""Find strings in the decrypted image and the code that builds their address (lis/addi pairs, like the recompiler's
generated code shows them), plus 32 bit pointers to them in data. Prints the function holding each reference.

    python -I tools/string_refs.py DIALOG_CONNECTION_FAILED [more substrings] [--exact]
"""
import argparse
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guest_at  # noqa: E402

BASE = 0x82000000
CODE = (0x82160000, 0x82AAA1BC)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('needles', nargs='+')
    ap.add_argument('--exact', action='store_true', help='whole string must match')
    a = ap.parse_args()
    img, funcs = guest_at.load()
    starts = [f[0] for f in funcs]

    def word(va):
        return struct.unpack_from('>I', img, va - BASE)[0]

    def holder(va):
        i = bisect.bisect_right(starts, va) - 1
        return '%08X' % funcs[i][0] if i >= 0 and funcs[i][0] <= va < funcs[i][1] else 'gap'

    targets = {}
    for n in a.needles:
        pat = re.escape(n.encode())
        for m in re.finditer(pat, img):
            s = img.rfind(b'\0', 0, m.start()) + 1
            e = img.find(b'\0', m.end())
            text = img[s:e]
            if a.exact and text != n.encode():
                continue
            targets[BASE + s] = text.decode('latin-1')
    refs = {t: [] for t in targets}
    lis = [None] * 32
    for va in range(CODE[0], CODE[1], 4):
        w = word(va)
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:
            lis[rd] = (w & 0xFFFF) << 16
            continue
        if op == 14 and lis[ra] is not None:
            lo = w & 0xFFFF
            v = (lis[ra] + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
            if v in refs:
                refs[v].append('code %08X in %s' % (va, holder(va)))
        if w in (0x4E800020, 0x4E800420):
            lis = [None] * 32
    for va in range(BASE, BASE + len(img) - 3, 4):
        v = word(va)
        if v in refs and not (CODE[0] <= va < CODE[1]):
            refs[v].append('data ptr @%08X' % va)
    for t in sorted(targets):
        print('%08X %s' % (t, targets[t]))
        for r in refs[t][:12]:
            print('    ' + r)


if __name__ == '__main__':
    main()
