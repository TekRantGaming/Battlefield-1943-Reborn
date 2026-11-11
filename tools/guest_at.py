"""Show what is at guest addresses in the decrypted image: the .pdata function that holds it (or the gap it sits in),
and the raw words around it, so a missing function's start and end can be read off.

    python -I tools/guest_at.py 0x82599398 [more addresses] [--words 24]
"""
import argparse
import bisect
import os
import struct

BASE = 0x82000000
PDATA = (0x82112E00, 0x82153C70)  # section range from the Ghidra import log


def load():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    img = open(os.path.join(root, 'input', 'image.bin'), 'rb').read()
    funcs = []
    for va in range(PDATA[0], PDATA[1] - 7, 8):
        begin, packed = struct.unpack_from('>II', img, va - BASE)
        if begin:
            funcs.append((begin, begin + ((packed >> 8) & 0x3FFFFF) * 4))
    funcs.sort()
    return img, funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addresses', nargs='+')
    ap.add_argument('--words', type=int, default=24)
    a = ap.parse_args()
    img, funcs = load()
    starts = [f[0] for f in funcs]
    for text in a.addresses:
        addr = int(text, 16)
        i = bisect.bisect_right(starts, addr) - 1
        prev, nxt = funcs[i], funcs[i + 1] if i + 1 < len(funcs) else (0, 0)
        if prev[0] <= addr < prev[1]:
            print('%08X is inside .pdata function %08X..%08X' % (addr, prev[0], prev[1]))
        else:
            print('%08X is in a gap: previous function ends %08X, next starts %08X' % (addr, prev[1], nxt[0]))
        for va in range(addr - 8, addr + a.words * 4, 4):
            w = struct.unpack_from('>I', img, va - BASE)[0]
            note = {0x4E800020: 'blr', 0x4E800420: 'bctr', 0x00000000: 'zero'}.get(w, '')
            if w >> 26 == 18:
                off = w & 0x3FFFFFC
                off = off - 0x4000000 if off & 0x2000000 else off
                note = ('bl' if w & 1 else 'b') + ' %08X' % (va + off)
            print('  %s%08X %08X %s' % ('>' if va == addr else ' ', va, w, note))


if __name__ == '__main__':
    main()
