"""Where BF's code builds PM4 type 3 packet headers (lis rD,0xC000|count then ori/addi with opcode << 8)."""
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guest_at  # noqa: E402

OPS = {0x22: 'DRAW_INDX', 0x36: 'DRAW_INDX_2', 0x3F: 'INDIRECT_BUFFER', 0x2D: 'SET_CONSTANT',
       0x2F: 'LOAD_ALU_CONSTANT', 0x30: 'SET_CONSTANT2', 0x3C: 'WAIT_REG_MEM', 0x46: 'EVENT_WRITE',
       0x3D: 'MEM_WRITE', 0x48: 'ME_INIT', 0x58: 'EVENT_WRITE_SHD', 0x5B: 'EVENT_WRITE_EXT',
       0x57: 'IM_LOAD_IMMEDIATE', 0x27: 'IM_LOAD', 0x37: 'VIZ_QUERY', 0x24: 'DRAW_INDX_BIN',
       0x10: 'NOP', 0x2E: 'COND_EXEC', 0x44: 'INVALIDATE_STATE', 0x4A: 'SET_SHADER_CONSTANTS'}
BASE = guest_at.BASE
img, funcs = guest_at.load()
starts = [f[0] for f in funcs]


def func_of(addr):
    i = bisect.bisect_right(starts, addr) - 1
    return funcs[i][0] if i >= 0 and funcs[i][0] <= addr < funcs[i][1] else None


def word(addr):
    o = addr - BASE
    return int.from_bytes(img[o:o + 4], 'big')


lo, hi = min(starts), max(f[1] for f in funcs)
by_op = collections.defaultdict(collections.Counter)
for addr in range(lo, hi, 4):
    w = word(addr)
    if w >> 26 != 15 or (w >> 16) & 31 != 0:  # addis rD,0,imm = lis
        continue
    imm = w & 0xFFFF
    if imm < 0xC000:
        continue
    rd = (w >> 21) & 31
    for k in range(1, 8):
        w2 = word(addr + 4 * k)
        op = w2 >> 26
        if op in (24, 14) and ((w2 >> 16) & 31) == rd:  # ori rA,rS,imm (rS=rd) / addi rD,rA,imm (rA=rd)
            low = w2 & 0xFFFF
            if op == 24 and ((w2 >> 21) & 31) != rd:
                continue
            code = (low >> 8) & 0x7F
            if low & 0xFF in (0, 1) and code in OPS:
                by_op[OPS[code]][func_of(addr)] += 1
            break
for name in sorted(by_op):
    fs = by_op[name]
    print('%-18s %3d sites in %3d functions: %s' % (name, sum(fs.values()), len(fs),
          ' '.join('%08X' % f if f else 'gap' for f in sorted(fs, key=lambda x: x or 0)[:24])))
