"""Find code addresses reached only indirectly (pointers in data, lis/addi pairs in code) that codegen did not register
as functions, and write them as a [functions] TOML include. A Python port of King Kong Recompiled's
tools/FindMissingFuncs.cs (same author), with the same rules:

  - a candidate is referenced, is not a known function start or a jump label, follows a terminator (blr, bctr,
    b, or zero padding) and is not itself zero;
  - its end is the next known or candidate start, or the first zero padding word before it;
  - unlike the C# tool, a candidate inside a .pdata function is left out: those are exception funclets of their
    parent (playbook chapter 2), and seeding them as functions makes new unresolved branches.

Inputs: input/image.bin (VA minus 0x82000000), bf/generated/default (bf1943_init.cpp function table and the
loc_ labels of the recomp sources). Usage:

    python -I tools/find_missing_funcs.py [--out bf/missing_funcs.toml] [--explain 0x82XXXXXX]
"""
import argparse
import bisect
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guest_at  # noqa: E402

BASE = 0x82000000
CODE = (0x82160000, 0x82AAA1BC)  # .text, ctr and BINK, as the runtime logs "code=..."
DATA_SKIP = CODE


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(root, 'bf', 'missing_funcs.toml'))
    ap.add_argument('--explain')
    a = ap.parse_args()
    img = open(os.path.join(root, 'input', 'image.bin'), 'rb').read()
    gen = os.path.join(root, 'bf', 'generated', 'default')

    def word(va):
        return struct.unpack_from('>I', img, va - BASE)[0]

    def terminator(w):
        return w in (0x4E800020, 0x4E800420, 0) or (w >> 26 == 18 and (w & 3) == 0)

    known, thunks = set(), []
    for m in re.finditer(r'\{ 0x([0-9A-F]{8}), (\w+) \}', open(os.path.join(gen, 'bf1943_init.cpp')).read()):
        addr = int(m.group(1), 16)
        if m.group(2).startswith('__imp__'):
            thunks.append(addr)
        else:
            known.add(addr)
    labels = set()
    for f in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        labels.update(int(x, 16) for x in re.findall(r'^loc_([0-9A-F]{8}):', open(f).read(), re.M))
    thunk_lo, thunk_hi = (min(thunks), max(thunks) + 16) if thunks else (0, 0)

    def in_code(v):
        return CODE[0] <= v < CODE[1] and not (thunk_lo <= v < thunk_hi) and (v & 3) == 0

    refs = {}
    end_img = BASE + len(img) - 3
    for va in range(BASE, end_img, 4):
        if DATA_SKIP[0] <= va < DATA_SKIP[1]:
            continue
        v = word(va)
        if in_code(v):
            refs.setdefault(v, 'data ptr @0x%08X' % va)
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
            # A target just after the lis/addi is a switch jump table base, not a function.
            if in_code(v) and not (v > va and v - va < 0x40):
                refs.setdefault(v, 'lis/addi @0x%08X' % va)
        if terminator(w):
            lis = [None] * 32

    if a.explain:
        t = int(a.explain, 16)
        print('explain %08X: ref=%s known=%s label=%s prev terminator=%s (prev=%08X) nonzero=%s' % (
            t, refs.get(t), t in known, t in labels, terminator(word(t - 4)), word(t - 4), word(t) != 0))
    _, pdata = guest_at.load()
    pstarts = [f[0] for f in pdata]

    def inside_pdata(t):
        i = bisect.bisect_right(pstarts, t) - 1
        return i >= 0 and pdata[i][0] < t < pdata[i][1]

    cands = sorted(t for t in refs if t not in known and t not in labels and terminator(word(t - 4)) and word(t)
                   and not inside_pdata(t))
    starts = sorted(known | set(cands))
    with open(a.out, 'w', newline='\n') as out:
        out.write('# Written by tools/find_missing_funcs.py: functions only referenced indirectly.\n[functions]\n')
        for c in cands:
            i = bisect.bisect_right(starts, c)
            nxt = starts[i] if i < len(starts) else CODE[1]
            end = c
            while end < nxt and word(end) != 0:
                end += 4
            out.write('0x%08X = { end = 0x%08X }  # %s\n' % (c, end, refs[c]))
    print('known=%d labels=%d code refs=%d missing=%d -> %s' % (len(known), len(labels), len(refs), len(cands), a.out))


if __name__ == '__main__':
    main()
