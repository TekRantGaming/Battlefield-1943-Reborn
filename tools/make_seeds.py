"""Build the Ghidra seed files from the recompiler's output, as King Kong's decomp does (scripts/make_seeds.py there).

Outputs (build/seeds/):
  functions.tsv  addr, end (exclusive, empty if unknown), source
  names.tsv      addr, name, source, evidence

Function starts are every entry of bf/generated/default/bf1943_init.cpp's table (what the recompiler translated).
Ends come from .pdata, then from bf/overrides.toml and bf/missing_funcs.toml. Names: the import thunks, and the
functions identified so far in docs and STATE.md (NAMES below, with their evidence), and the recompiler's own
names (register save and restore helpers, which SeedBF1943.java marks inline).

    python -I tools/make_seeds.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guest_at  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'seeds')

# Functions named from evidence so far: address -> (name, evidence)
NAMES = {
    0x829C34D0: ('setjmp', 'saves f14..f31, r13..r31, v64..v127 into the buffer at r3; declared to codegen'),
    0x829C3090: ('longjmp', 'restores the setjmp buffer; declared to codegen'),
    0x8299A440: ('luaD_throw', 'longjmp to L->errorJmp, else panic and exit(1); first boot trace'),
    0x827C4578: ('exit', 'CRT exit, KeBugCheck(0) at its end; first boot trace'),
    0x827D57D8: ('XUserGetSigninState', 'thin wrapper around XamUserGetSigninState'),
    0x827D7528: ('XUserGetSigninInfo', 'thin wrapper around XamUserGetSigninInfo'),
    0x827D58F8: ('XUserGetXUID', 'thin wrapper around XamUserGetXUID'),
    0x824836B0: ('XNetGetEthernetLinkStatus', 'thin wrapper around NetDll_XNetGetEthernetLinkStatus'),
    0x824836A0: ('XNetGetTitleXnAddr', 'thin wrapper around NetDll_XNetGetTitleXnAddr'),
    0x824835C0: ('XNetGetConnectStatus', 'thin wrapper around NetDll_XNetGetConnectStatus'),
    0x827D74C0: ('XShowSigninUI', 'thin wrapper around XamShowSigninUI'),
    0x8249A010: ('OnlineManager_Reset', 'clears the online manager at [0x82AF67EC], calls XNetUnregisterKey; '
                                        'runs when Multiplayer fails to connect (runs\\live3)'),
}


def main():
    os.makedirs(OUT, exist_ok=True)
    gen = os.path.join(ROOT, 'bf', 'generated', 'default')
    table = re.findall(r'\{ 0x([0-9A-F]{8}), (\w+) \}', open(os.path.join(gen, 'bf1943_init.cpp')).read())
    _, pdata = guest_at.load()
    ends = {b: e for b, e in pdata}
    pdata_starts = set(ends)
    for toml in ('overrides.toml', 'missing_funcs.toml'):
        for a, e in re.findall(r'0x([0-9A-F]{8}) = \{ end = 0x([0-9A-F]{8}) \}', open(os.path.join(ROOT, 'bf', toml)).read()):
            ends.setdefault(int(a, 16), int(e, 16))
    functions, names = [], []
    for addr_text, sym in table:
        addr = int(addr_text, 16)
        if sym.startswith('__imp__'):
            names.append((addr, sym[len('__imp__'):], 'import', 'import thunk ' + sym))
            source = 'import thunk'
        elif not sym.startswith('sub_'):
            names.append((addr, sym, 'recompiler', 'named by the recompiler (register save and restore helpers)'))
            source = 'recompiler helper'
        else:
            source = 'pdata' if addr in pdata_starts else 'recompiler'
        end = ends.get(addr)
        functions.append((addr, end, source))
    for addr, (name, evidence) in NAMES.items():
        names.append((addr, name, 'bf1943', evidence))
    functions.sort()
    with open(os.path.join(OUT, 'functions.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('addr\tend\tsource\n')
        for addr, end, source in functions:
            f.write('%08X\t%s\t%s\n' % (addr, '%08X' % end if end else '', source))
    with open(os.path.join(OUT, 'names.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('addr\tname\tsource\tevidence\n')
        for row in sorted(names):
            f.write('%08X\t%s\t%s\t%s\n' % row)
    print('functions %d (with ends %d), names %d -> %s' % (
        len(functions), sum(1 for x in functions if x[1]), len(names), OUT))


if __name__ == '__main__':
    main()
