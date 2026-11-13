import sys, struct, logging, os
logging.disable(logging.CRITICAL)
from minidump.minidumpfile import MinidumpFile
def load(path):
    raw = open(path, 'rb').read(); mf = MinidumpFile.parse(path)
    mods = []
    for d in mf.directories:
        if 'ModuleListStream' not in str(d.StreamType): continue
        off = d.Location.Rva; (n,) = struct.unpack_from('<I', raw, off); off += 4
        for i in range(n):
            (base, size, chk, ts, name_rva) = struct.unpack_from('<QIIII', raw, off); off += 108
            (nlen,) = struct.unpack_from('<I', raw, name_rva)
            mods.append((base, size, os.path.basename(raw[name_rva + 4:name_rva + 4 + nlen].decode('utf-16-le', 'replace'))))
    segs = sorted((s.start_virtual_address, s.size, s.start_file_address) for s in mf.memory_segments.memory_segments)
    return raw, mf, mods, segs
def where(mods, a):
    for (base, size, name) in mods:
        if base <= a < base + size: return '%s+0x%x' % (name, a - base)
    return None
def seg_of(segs, addr):
    for (start, sz, foff) in segs:
        if start <= addr < start + sz: return (start, sz, foff)
    return None
