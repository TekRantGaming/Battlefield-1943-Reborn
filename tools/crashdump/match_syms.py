"""Name 0.3.0 bf1943.exe RVAs with the 0.3.1-test2 PDB by matching the function's code bytes.
Usage: python -I match_syms.py rva [rva...]"""
import pefile, re, subprocess, struct, bisect, sys
old = pefile.PE(r"F:\BF-1942-MP-Decomp\runs\release\0.3.0\BF1943-Reborn-0.3.0-windows-x64\bf1943.exe", fast_load=True)
new = pefile.PE(r"F:\BF-1942-MP-Decomp\bf\out\build\bf-relwithdebinfo\bf1943.exe", fast_load=True)
od = old.get_memory_mapped_image(); nd = new.get_memory_mapped_image()
nbase = new.OPTIONAL_HEADER.ImageBase
sym = "llvm-symbolizer"  # LLVM's symbolizer, on the PATH
funcs = []
for s in old.sections:
    if s.Name.rstrip(b'\0') == b'.pdata':
        d = old.get_data(s.VirtualAddress, s.Misc_VirtualSize)
        for i in range(0, len(d) - 11, 12):
            (b, e, u) = struct.unpack_from('<III', d, i)
            if b == 0: break
            funcs.append((b, e))
funcs.sort(); starts = [f[0] for f in funcs]
def func_of(rva):
    i = bisect.bisect_right(starts, rva) - 1
    return funcs[i] if i >= 0 and funcs[i][0] <= rva < funcs[i][1] else None
def symbolize(rvas):
    args = [sym, "--obj=" + r"F:\BF-1942-MP-Decomp\bf\out\build\bf-relwithdebinfo\bf1943.exe", "--inlines", "--demangle", "--no-demangle"] 
    args = [sym, "--obj=" + r"F:\BF-1942-MP-Decomp\bf\out\build\bf-relwithdebinfo\bf1943.exe", "--inlines"] + ["0x%x" % (nbase + r) for r in rvas]
    out = subprocess.run(args, capture_output=True, text=True).stdout
    return ' | '.join(l.strip() for l in out.splitlines() if l.strip() and 'Microsoft Visual Studio' not in l)[:400]
def clean(w):
    return not (any(c in (0xE8, 0xE9) for c in w) or b'\xff\x15' in w or b'\x8d\x05' in w or b'\x8b\x05' in w or b'\x8d\x0d' in w or b'\x8d\x15' in w or b'\x8b\x0d' in w or b'\x8b\x15' in w or b'\x8d\x1d' in w or b'\x8d\x35' in w or b'\x8d\x3d' in w)
for arg in sys.argv[1:]:
    rva = int(arg, 16)
    f = func_of(rva)
    fs = f[0] if f else rva - 0x80
    hit = None
    for wstart in list(range(rva - 60, rva + 20, 4)) + list(range(fs, min(fs + 0x200, rva), 4)):
        w = od[wstart:wstart + 40]
        if not clean(w): continue
        hits = [m.start() for m in re.finditer(re.escape(w), nd)]
        if len(hits) == 1:
            hit = (wstart, hits[0]); break
    if not hit:
        print('0x%x (fn 0x%x+0x%x): no unique match' % (rva, fs, rva - fs)); continue
    delta = hit[1] - hit[0]
    print('0x%x (fn 0x%x+0x%x) -> 0.3.1 0x%x: %s' % (rva, fs, rva - fs, rva + delta, symbolize([rva + delta - 1])))
