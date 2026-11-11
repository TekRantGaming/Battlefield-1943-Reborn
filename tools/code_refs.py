"""Find code that builds a given address with lis/addi (or lis/ori), the way callbacks and tables are passed.

    python -I tools/code_refs.py 0x823BE7C8 [more]
"""
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guest_at  # noqa: E402

BASE = 0x82000000
CODE = (0x82160000, 0x82AAA1BC)


def main():
    targets = {int(a, 16) for a in sys.argv[1:]}
    img, funcs = guest_at.load()
    starts = [f[0] for f in funcs]

    def word(va):
        return struct.unpack_from('>I', img, va - BASE)[0]

    lis = [None] * 32
    for va in range(CODE[0], CODE[1], 4):
        w = word(va)
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:
            lis[rd] = (w & 0xFFFF) << 16
            continue
        if op in (14, 24) and lis[ra] is not None:
            lo = w & 0xFFFF
            v = ((lis[ra] + (lo - 0x10000 if lo & 0x8000 else lo)) if op == 14 else (lis[ra] | lo)) & 0xFFFFFFFF
            if v in targets:
                i = bisect.bisect_right(starts, va) - 1
                print('%08X built at %08X in function %08X' % (v, va, funcs[i][0] if i >= 0 else 0))
        if w in (0x4E800020, 0x4E800420):
            lis = [None] * 32


if __name__ == '__main__':
    main()
