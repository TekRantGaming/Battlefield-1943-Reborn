"""Read a Windows minidump with the `minidump` package plus a hand parser for the module list (CV records).
Usage: python -I parse_dump.py <dump path> [--stack-depth N]
Prints: header, system info, misc info, modules (with version, timestamp, PDB GUID/age), exception, threads,
the crashing thread's context, and a scan of its stack for return addresses inside loaded modules.
"""
import sys, struct, datetime, uuid
from minidump.minidumpfile import MinidumpFile

path = sys.argv[1]
mf = MinidumpFile.parse(path)
fh = open(path, 'rb')
raw = fh.read()

def utc(ts):
    try:
        return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')
    except Exception:
        return '?'

# ---------- directories ----------
print('== HEADER ==')
h = mf.header
print(' flags=%s' % getattr(h, 'Flags', None), 'streams=%d' % len(mf.directories),
      'TimeDateStamp=%s' % utc(getattr(h, 'TimeDateStamp', 0)))
print(' streams:', ', '.join(str(getattr(d, 'StreamType', '?')).replace('MINIDUMP_STREAM_TYPE.', '') for d in mf.directories))

# ---------- system info ----------
print('== SYSTEM ==')
si = mf.sysinfo
if si:
    print(' arch=%s level=%s rev=0x%x procs=%s product=%s ver=%s.%s build=%s platform=%s csd=%r' % (
        si.ProcessorArchitecture, si.ProcessorLevel, si.ProcessorRevision or 0, si.NumberOfProcessors, si.ProductType,
        si.MajorVersion, si.MinorVersion, si.BuildNumber, si.PlatformId, si.CSDVersion))
    print(' vendor=%s versioninfo=%s feature=0x%x' % (si.VendorId, si.VersionInformation, si.FeatureInformation or 0))
mi = mf.misc_info
if mi:
    print('== MISC == pid=%s created=%s user=%s kernel=%s maxmhz=%s curmhz=%s' % (
        mi.ProcessId, utc(mi.ProcessCreateTime or 0), mi.ProcessUserTime, mi.ProcessKernelTime, mi.ProcessorMaxMhz,
        mi.ProcessorCurrentMhz))

# ---------- modules (hand parsed for the CV record) ----------
mods = []
for d in mf.directories:
    st = str(getattr(d, 'StreamType', ''))
    if 'ModuleListStream' not in st:
        continue
    loc = d.Location
    off = loc.Rva
    (n,) = struct.unpack_from('<I', raw, off)
    off += 4
    for i in range(n):
        (base, size, chk, ts, name_rva) = struct.unpack_from('<QIIII', raw, off)
        vi = struct.unpack_from('<13I', raw, off + 24)
        (cv_size, cv_rva, misc_size, misc_rva) = struct.unpack_from('<IIII', raw, off + 24 + 52)
        off += 108
        (nlen,) = struct.unpack_from('<I', raw, name_rva)
        name = raw[name_rva + 4:name_rva + 4 + nlen].decode('utf-16-le', 'replace')
        fv = '%d.%d.%d.%d' % (vi[2] >> 16, vi[2] & 0xffff, vi[3] >> 16, vi[3] & 0xffff) if vi[0] == 0xFEEF04BD else ''
        pv = '%d.%d.%d.%d' % (vi[4] >> 16, vi[4] & 0xffff, vi[5] >> 16, vi[5] & 0xffff) if vi[0] == 0xFEEF04BD else ''
        cv = ''
        if cv_size >= 24 and raw[cv_rva:cv_rva + 4] == b'RSDS':
            g = uuid.UUID(bytes_le=raw[cv_rva + 4:cv_rva + 20])
            (age,) = struct.unpack_from('<I', raw, cv_rva + 20)
            pdb = raw[cv_rva + 24:cv_rva + cv_size].split(b'\0')[0].decode('utf-8', 'replace')
            cv = '%s%x %s' % (g.hex.upper(), age, pdb)
        mods.append(dict(name=name, base=base, size=size, ts=ts, chk=chk, fv=fv, pv=pv, cv=cv))
mods.sort(key=lambda m: m['base'])
print('== MODULES (%d) ==' % len(mods))
for m in mods:
    print(' %016x %08x ts=%08x(%s) fv=%-16s pv=%-16s %s | pdb=%s' % (
        m['base'], m['size'], m['ts'], utc(m['ts']), m['fv'], m['pv'], m['name'], m['cv']))

def where(a):
    for m in mods:
        if m['base'] <= a < m['base'] + m['size']:
            return '%s+0x%x' % (m['name'].split('\\')[-1], a - m['base'])
    return None

# ---------- unloaded modules ----------
if mf.unloaded_modules:
    print('== UNLOADED ==')
    for u in mf.unloaded_modules.modules:
        print(' %016x %08x ts=%08x %s' % (u.baseaddress, u.size, u.timestamp or 0, u.name))

# ---------- memory segments ----------
segs = []
if mf.memory_segments:
    for s in mf.memory_segments.memory_segments:
        segs.append((s.start_virtual_address, s.size, s.start_file_address))
if mf.memory_segments_64:
    for s in mf.memory_segments_64.memory_segments:
        segs.append((s.start_virtual_address, s.size, s.start_file_address))
segs.sort()
print('== MEMORY SEGMENTS (%d, %d bytes) ==' % (len(segs), sum(s[1] for s in segs)))

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

# ---------- memory info (regions) ----------
def region_of(addr):
    if not mf.memory_info:
        return None
    for r in mf.memory_info.infos:
        if r.BaseAddress <= addr < r.BaseAddress + r.RegionSize:
            return r
    return None

# ---------- exception ----------
print('== EXCEPTION ==')
crash_tid = None
if mf.exception:
    for er in mf.exception.exception_records:
        ex = er.ExceptionRecord
        crash_tid = er.ThreadId
        code = ex.ExceptionCode_raw if ex.ExceptionCode_raw is not None else ex.ExceptionCode
        try:
            code = int(code)
        except Exception:
            pass
        print(' thread=%d code=%s (%s) flags=0x%x addr=0x%016x (%s) nparams=%d params=%s' % (
            er.ThreadId, hex(code) if isinstance(code, int) else code, ex.ExceptionCode, ex.ExceptionFlags,
            ex.ExceptionAddress, where(ex.ExceptionAddress), ex.NumberParameters,
            ['0x%x' % p for p in ex.ExceptionInformation[:ex.NumberParameters]]))
        r = region_of(ex.ExceptionAddress)
        if r:
            print('  region of the fault address: base=0x%x size=0x%x state=%s protect=%s type=%s' % (
                r.BaseAddress, r.RegionSize, r.State, r.Protect, r.Type))
        if ex.NumberParameters >= 2 and isinstance(code, int) and code in (0xC0000005, 0xC0000006):
            tgt = ex.ExceptionInformation[1]
            r = region_of(tgt)
            print('  access target 0x%x: %s; region: %s' % (
                tgt, where(tgt),
                'base=0x%x size=0x%x state=%s protect=%s type=%s' % (r.BaseAddress, r.RegionSize, r.State, r.Protect, r.Type) if r else 'none'))
else:
    print(' none')

# ---------- threads ----------
print('== THREADS (%d) ==' % len(mf.threads.threads))
infos = {}
if mf.thread_info:
    for ti in mf.thread_info.infos:
        infos[ti.ThreadId] = ti
ctxs = {}
for t in mf.threads.threads:
    c = getattr(t, 'ContextObject', None)
    ctxs[t.ThreadId] = c
    ti = infos.get(t.ThreadId)
    start = ('start=%s' % where(ti.StartAddress)) if ti and ti.StartAddress else ''
    rip = c.Rip if c else 0
    print(' tid=%-6d rip=%016x %-40s rsp=%016x teb=%x pri=%s susp=%s %s%s' % (
        t.ThreadId, rip, where(rip), c.Rsp if c else 0, t.Teb, t.Priority, t.SuspendCount, start,
        '  <== CRASHING' if t.ThreadId == crash_tid else ''))

# ---------- the crashing thread's context and stack scan ----------
def scan_stack(tid, depth):
    c = ctxs.get(tid)
    if not c:
        print(' no context for thread', tid)
        return
    print('== CONTEXT of thread %d ==' % tid)
    print(' rip=%016x rsp=%016x rbp=%016x eflags=%08x' % (c.Rip, c.Rsp, c.Rbp, c.EFlags))
    print(' rax=%016x rbx=%016x rcx=%016x rdx=%016x' % (c.Rax, c.Rbx, c.Rcx, c.Rdx))
    print(' rsi=%016x rdi=%016x r8 =%016x r9 =%016x' % (c.Rsi, c.Rdi, c.R8, c.R9))
    print(' r10=%016x r11=%016x r12=%016x r13=%016x' % (c.R10, c.R11, c.R12, c.R13))
    print(' r14=%016x r15=%016x' % (c.R14, c.R15))
    for name in ('Rip', 'Rax', 'Rbx', 'Rcx', 'Rdx', 'Rsi', 'Rdi', 'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15', 'Rbp'):
        v = getattr(c, name)
        w = where(v)
        if w:
            print('  %s -> %s' % (name, w))
    s = seg_of(c.Rsp)
    if not s:
        print(' stack memory at rsp not in the dump')
        return
    (start, sz, foff) = s
    end = start + sz
    print('== STACK SCAN thread %d: rsp=%x, segment %x..%x (%d bytes) ==' % (tid, c.Rsp, start, end, sz))
    # code bytes at rip
    cb = read_mem(c.Rip - 16, 32)
    if cb:
        print(' bytes at rip-16..rip+16: %s | %s' % (cb[:16].hex(' '), cb[16:].hex(' ')))
    else:
        print(' code bytes at rip not in the dump')
    n = 0
    a = c.Rsp & ~7
    while a + 8 <= end and n < depth:
        (v,) = struct.unpack_from('<Q', raw, foff + (a - start))
        w = where(v)
        if w:
            # does a call precede it? look at code bytes if the dump has them
            pre = read_mem(v - 7, 7)
            call = ''
            if pre:
                if pre[2] == 0xE8:
                    call = 'call rel32'
                elif pre[1:3] == b'\xff\x15':
                    call = 'call [rip+rel32]'
                elif pre[5] == 0xFF and (pre[6] & 0xF8) == 0xD0:
                    call = 'call reg'
                elif pre[4] == 0x41 and pre[5] == 0xFF and (pre[6] & 0xF8) == 0xD0:
                    call = 'call r8-15'
                elif pre[4] == 0xFF and (pre[5] & 0xF8) == 0x50:
                    call = 'call [reg+disp8]'
                elif pre[1] == 0xFF and (pre[2] & 0xF8) == 0x90:
                    call = 'call [reg+disp32]'
                elif pre[3] == 0xFF and pre[4] == 0x14:
                    call = 'call [sib]'
                elif pre[0] == 0xFF and pre[1] == 0x94:
                    call = 'call [sib+disp32]'
                else:
                    call = 'no call before (%s)' % pre.hex(' ')
            else:
                call = 'code not in dump'
            print(' [%016x] %016x %-44s %s' % (a, v, w, call))
            n += 1
        a += 8
    print(' (scanned up to %x)' % a)

depth = 80
if '--stack-depth' in sys.argv:
    depth = int(sys.argv[sys.argv.index('--stack-depth') + 1])
if crash_tid is not None:
    scan_stack(crash_tid, depth)
if '--all-threads' in sys.argv:
    for t in mf.threads.threads:
        if t.ThreadId != crash_tid:
            scan_stack(t.ThreadId, 25)
