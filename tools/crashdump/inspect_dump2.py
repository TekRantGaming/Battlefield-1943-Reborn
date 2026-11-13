"""Second pass: the exception stream's own context, a deep stack scan from it, the memory the exception parameter
points at (heap failure info), the memory segments inside ntdll, and every Vulkan layer / overlay string.
Usage: python -I inspect_dump2.py <dump path>
"""
import sys, struct, re
from minidump.minidumpfile import MinidumpFile
from minidump.streams.ContextStream import CONTEXT
import io, logging
logging.disable(logging.CRITICAL)

path = sys.argv[1]
raw = open(path, 'rb').read()
mf = MinidumpFile.parse(path)

mods = []
for d in mf.directories:
    if 'ModuleListStream' not in str(d.StreamType):
        continue
    off = d.Location.Rva
    (n,) = struct.unpack_from('<I', raw, off)
    off += 4
    for i in range(n):
        (base, size, chk, ts, name_rva) = struct.unpack_from('<QIIII', raw, off)
        off += 108
        (nlen,) = struct.unpack_from('<I', raw, name_rva)
        name = raw[name_rva + 4:name_rva + 4 + nlen].decode('utf-16-le', 'replace').split('\\')[-1]
        mods.append((base, size, name))
mods.sort()

def where(a):
    for (base, size, name) in mods:
        if base <= a < base + size:
            return '%s+0x%x' % (name, a - base)
    return None

segs = []
for s in mf.memory_segments.memory_segments:
    segs.append((s.start_virtual_address, s.size, s.start_file_address))
segs.sort()

def read_mem(addr, size):
    for (start, sz, foff) in segs:
        if start <= addr and addr + size <= start + sz:
            o = foff + (addr - start)
            return raw[o:o + size]
    return None

def seg_of(addr):
    for (start, sz, foff) in segs:
        if start <= addr < start + sz:
            return (start, sz, foff)
    return None

# ---- the exception stream's context ----
for d in mf.directories:
    if 'ExceptionStream' not in str(d.StreamType):
        continue
    off = d.Location.Rva
    (tid, align) = struct.unpack_from('<II', raw, off)
    # MINIDUMP_EXCEPTION is 152 bytes; then ThreadContext (DataSize, Rva)
    (ctx_size, ctx_rva) = struct.unpack_from('<II', raw, off + 8 + 152)
    print('== EXCEPTION CONTEXT (thread %d, %d bytes at rva 0x%x) ==' % (tid, ctx_size, ctx_rva))
    c = CONTEXT.parse(io.BytesIO(raw[ctx_rva:ctx_rva + ctx_size]))
    print(' rip=%016x %s' % (c.Rip, where(c.Rip)))
    print(' rsp=%016x rbp=%016x eflags=%08x' % (c.Rsp, c.Rbp, c.EFlags))
    print(' rax=%016x rbx=%016x rcx=%016x rdx=%016x' % (c.Rax, c.Rbx, c.Rcx, c.Rdx))
    print(' rsi=%016x rdi=%016x r8 =%016x r9 =%016x' % (c.Rsi, c.Rdi, c.R8, c.R9))
    print(' r10=%016x r11=%016x r12=%016x r13=%016x' % (c.R10, c.R11, c.R12, c.R13))
    print(' r14=%016x r15=%016x' % (c.R14, c.R15))
    for name in ('Rax', 'Rbx', 'Rcx', 'Rdx', 'Rsi', 'Rdi', 'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15', 'Rbp'):
        v = getattr(c, name)
        w = where(v)
        if w:
            print('  %s -> %s' % (name, w))
    s = seg_of(c.Rsp)
    if s:
        (start, sz, foff) = s
        end = start + sz
        print('== DEEP STACK SCAN from the exception rsp=%x to %x ==' % (c.Rsp, end))
        a = c.Rsp & ~7
        while a + 8 <= end:
            (v,) = struct.unpack_from('<Q', raw, foff + (a - start))
            w = where(v)
            if w:
                pre = read_mem(v - 7, 7)
                call = ''
                if pre:
                    if pre[2] == 0xE8: call = 'call rel32'
                    elif pre[1:3] == b'\xff\x15': call = 'call [rip+rel32]'
                    elif pre[5] == 0xFF and (pre[6] & 0xF8) == 0xD0: call = 'call reg'
                    elif pre[4] == 0x41 and pre[5] == 0xFF and (pre[6] & 0xF8) == 0xD0: call = 'call r8-15'
                    elif pre[4] == 0xFF and (pre[5] & 0xF8) == 0x50: call = 'call [reg+disp8]'
                    elif pre[1] == 0xFF and (pre[2] & 0xF8) == 0x90: call = 'call [reg+disp32]'
                    elif pre[3] == 0xFF and pre[4] == 0x14: call = 'call [sib]'
                    elif pre[0] == 0xFF and pre[1] == 0x94: call = 'call [sib+disp32]'
                    else: call = 'no call before (%s)' % pre.hex(' ')
                else:
                    call = '-'
                print(' [%016x] %016x %-40s %s' % (a, v, w, call))
            a += 8
    else:
        print(' exception rsp not in a dumped segment')

# ---- the exception parameter's memory ----
for er in mf.exception.exception_records:
    ex = er.ExceptionRecord
    for p in ex.ExceptionInformation[:ex.NumberParameters]:
        print('== EXCEPTION PARAM 0x%x = %s ==' % (p, where(p)))
        s = seg_of(p)
        if not s:
            print(' not in the dump')
            # nearest segments
            for (start, sz, foff) in segs:
                if abs(start - p) < 0x200000:
                    print('  near: segment %x..%x' % (start, start + sz))
            continue
        (start, sz, foff) = s
        print(' in segment %x..%x' % (start, start + sz))
        avail = min(0x200, start + sz - p)
        b = read_mem(p, avail)
        print(' bytes:', b[:0x60].hex(' '))
        (ver, ssize, ftype) = struct.unpack_from('<IIQ', b, 0)
        print(' Version=%d StructureSize=%d FailureType=%d' % (ver, ssize, ftype))
        names = {1:'internal',2:'unknown',3:'generic',4:'entry_corruption',5:'multiple_entries_corruption',
                 6:'virtual_block_corruption',7:'buffer_overrun',8:'buffer_underrun',9:'block_not_busy',
                 10:'invalid_argument',11:'usage_after_free',12:'cross_heap_use',13:'freelists_corruption',
                 14:'listentry_corruption',15:'lfh_bitmap_mismatch',16:'segment_lfh_bitmap_corruption',
                 17:'segment_lfh_double_free',18:'vs_subsegment_corruption',19:'null_heap',20:'allocation_limit',
                 21:'commit_limit',22:'invalid_vs_block_entry'}
        print(' FailureType name (if the layout holds): heap_failure_%s' % names.get(ftype, '?'))
        q = struct.unpack_from('<%dQ' % (avail // 8), b, 0)
        for i, v in enumerate(q):
            w = where(v)
            print('  +0x%03x %016x %s' % (i * 8, v, w or ''))

# ---- segments inside ntdll (data captured) ----
for (base, size, name) in mods:
    if name.lower() == 'ntdll.dll':
        print('== segments inside ntdll (%x..%x) ==' % (base, base + size))
        for (start, sz, foff) in segs:
            if base <= start < base + size:
                print('  %x..%x (%d bytes) ntdll+0x%x' % (start, start + sz, sz, start - base))

# ---- strings: layers, overlays, env ----
print('== LAYER / OVERLAY / ENV STRINGS ==')
pats = [rb'VK_LAYER_[A-Za-z0-9_]+', rb'[A-Za-z]:\\[^\x00\r\n"]{0,120}\.json', rb'[A-Z_]{4,}VK_[A-Z_]+',
        rb'VK_[A-Z_]+=[^\x00\r\n]{0,80}', rb'ENABLE_[A-Z_0-9]+', rb'DISABLE_[A-Z_0-9]+', rb'VK_INSTANCE_LAYERS[^\x00]{0,100}',
        rb'VK_LOADER_[A-Z_]+[^\x00]{0,100}']
seen = set()
for pat in pats:
    for m in re.finditer(pat, raw):
        t = m.group(0).decode('ascii', 'replace')
        if t in seen: continue
        seen.add(t)
        print(' ascii @0x%x: %s' % (m.start(), t))
    wpat = b''.join(bytes([ch]) + b'\x00' if ch < 0x80 else bytes([ch]) for ch in b'')
for m in re.finditer(rb'(?:[A-Za-z0-9_:\\. \-]\x00){6,}', raw):
    t = m.group(0).decode('utf-16-le', 'replace')
    if ('json' in t.lower() or 'vk_layer' in t.lower() or 'overlay' in t.lower() or 'layer' in t.lower()) and t not in seen:
        seen.add(t)
        print(' utf16 @0x%x: %s' % (m.start(), t))
