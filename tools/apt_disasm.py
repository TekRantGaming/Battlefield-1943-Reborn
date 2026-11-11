"""Disassemble ActionScript functions in an EA APT movie (Battlefield 1943 menus, Xbox 360, big endian). Our own
tool; the opcode table and operand formats follow OpenSAGE's APT reader (format knowledge only, no code copied).

Files come from a BIGF archive inside an fbrb SwfMovie entry: <name>.apt (movie and bytecode) and <name>.const
(constant pool: header "Apt constant file", then at 0x14 the apt data offset, item count, item table offset;
items are (type, value) pairs: type 1 is a string at that offset in .const, type 4 a number).

Functions are found by name: a DefineFunction2 (0x8E) or DefineFunction (0x9B) whose 4 byte name operand points to
that name in the .apt file. Constant pool references (EA_PushConstantByte, ...) use the most recent ConstantPool.

    python -I tools/apt_disasm.py <file.apt> <function name> [more names] [--all]
"""
import argparse
import os
import re
import struct

ALIGNED = {0x9B, 0x8E, 0x88, 0x9D, 0x99, 0x96, 0x83, 0x8C, 0x87, 0x8B, 0x81, 0x9F, 0x94, 0xA1, 0xB8, 0xA4, 0xA6,
           0xA5, 0xA7}
NAMES = {
    0x00: 'End', 0x04: 'NextFrame', 0x05: 'PrevFrame', 0x06: 'Play', 0x07: 'Stop', 0x0A: 'Add', 0x0B: 'Subtract',
    0x0C: 'Multiply', 0x0D: 'Divide', 0x0E: 'Equals', 0x0F: 'LessThan', 0x10: 'And', 0x11: 'Or', 0x12: 'Not',
    0x13: 'StringEquals', 0x14: 'StringLength', 0x15: 'SubString', 0x17: 'Pop', 0x18: 'ToInteger',
    0x1C: 'GetVariable', 0x1D: 'SetVariable', 0x20: 'SetTarget2', 0x21: 'StringConcat', 0x22: 'GetProperty',
    0x23: 'SetProperty', 0x24: 'CloneSprite', 0x25: 'RemoveSprite', 0x26: 'Trace', 0x2A: 'Throw', 0x30: 'Random',
    0x34: 'GetTimer', 0x3A: 'Delete', 0x3B: 'Delete2', 0x3C: 'DefineLocal', 0x3D: 'CallFunction', 0x3E: 'Return',
    0x3F: 'Modulo', 0x40: 'NewObject', 0x41: 'Var', 0x42: 'InitArray', 0x43: 'InitObject', 0x44: 'TypeOf',
    0x45: 'TargetPath', 0x46: 'Enumerate', 0x47: 'Add2', 0x48: 'LessThan2', 0x49: 'Equals2', 0x4A: 'ToNumber',
    0x4B: 'ToString', 0x4C: 'PushDuplicate', 0x4D: 'StackSwap', 0x4E: 'GetMember', 0x4F: 'SetMember',
    0x50: 'Increment', 0x51: 'Decrement', 0x52: 'CallMethod', 0x53: 'NewMethod', 0x54: 'InstanceOf',
    0x55: 'Enumerate2', 0x56: 'PushThis', 0x58: 'PushGlobal', 0x59: 'PushZero', 0x5A: 'PushOne',
    0x5B: 'CallFuncPop', 0x5C: 'CallFunc', 0x5D: 'CallMethodPop', 0x5E: 'CallMethod', 0x60: 'BitAnd',
    0x61: 'BitOr', 0x62: 'BitXor', 0x63: 'ShiftLeft', 0x64: 'ShiftRight', 0x65: 'ShiftRight2',
    0x66: 'StrictEqual', 0x67: 'Greater', 0x68: 'StringGreater', 0x69: 'Extends', 0x70: 'PushThisVar',
    0x71: 'PushGlobalVar', 0x72: 'ZeroVar', 0x73: 'PushTrue', 0x74: 'PushFalse', 0x75: 'PushNull',
    0x76: 'PushUndefined', 0x77: 'TraceStart', 0x81: 'GotoFrame', 0x83: 'GetURL', 0x87: 'SetRegister',
    0x88: 'ConstantPool', 0x8B: 'SetTarget', 0x8C: 'GotoLabel', 0x8E: 'DefineFunction2', 0x94: 'With',
    0x96: 'PushData', 0x99: 'BranchAlways', 0x9A: 'GetURL2', 0x9B: 'DefineFunction', 0x9D: 'BranchIfTrue',
    0x9E: 'CallFrame', 0x9F: 'GotoFrame2', 0xA1: 'PushString', 0xA2: 'PushConstantByte',
    0xA3: 'PushConstantWord', 0xA4: 'GetStringVar', 0xA5: 'GetStringMember', 0xA6: 'SetStringVar',
    0xA7: 'SetStringMember', 0xAE: 'PushValueOfVar', 0xAF: 'GetNamedMember', 0xB0: 'CallNamedFuncPop',
    0xB1: 'CallNamedFunc', 0xB2: 'CallNamedMethodPop', 0xB3: 'CallNamedMethod', 0xB4: 'PushFloat',
    0xB5: 'PushByte', 0xB6: 'PushShort', 0xB7: 'PushLong', 0xB8: 'BranchIfFalse', 0xB9: 'PushRegister',
    0xFF: 'Padding',
}


class Const:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.data_offset, count, table = struct.unpack_from('>III', d, 0x14)
        self.items = []
        for i in range(count):
            t, v = struct.unpack_from('>II', d, table + i * 8)
            if t == 1:
                self.items.append(d[v:d.find(b'\0', v)].decode('latin-1'))
            elif t == 4:
                self.items.append(struct.unpack('>f', struct.pack('>I', v))[0])
            else:
                self.items.append('<type %d: %08X>' % (t, v))


class Disasm:
    def __init__(self, apt, const):
        self.d = open(apt, 'rb').read()
        self.const = const
        self.pool = []

    def s(self, off):
        if not 0 < off < len(self.d):
            return '<%08X>' % off
        return self.d[off:self.d.find(b'\0', off)].decode('latin-1')

    def u32(self, o):
        return struct.unpack_from('>I', self.d, o)[0]

    def c(self, idx):
        if idx < len(self.pool):
            item = self.pool[idx]
            return repr(self.const.items[item]) if item < len(self.const.items) else '<const %d>' % item
        return '<pool %d>' % idx

    def run(self, start, end, depth=0, out=None):
        out = out if out is not None else []
        o = start
        pad = '  ' * depth
        while o < end:
            at = o
            op = self.d[o]
            o += 1
            if op in ALIGNED:
                o = (o + 3) & ~3
            name = NAMES.get(op, 'op_%02X' % op)
            arg = ''
            if op in (0x99, 0x9D, 0xB8):
                rel = struct.unpack_from('>i', self.d, o)[0]
                o += 4
                arg = '-> %06X' % (o + rel)
            elif op == 0x88:
                n, p = self.u32(o), self.u32(o + 4)
                o += 8
                self.pool = [self.u32(p + 4 * i) for i in range(n)]
                arg = '%d constants' % n
            elif op == 0x96:
                n, p = self.u32(o), self.u32(o + 4)
                o += 8
                arg = ', '.join(repr(self.const.items[self.u32(p + 4 * i)]) if self.u32(p + 4 * i) <
                                len(self.const.items) else '?' for i in range(n))
            elif op in (0xA1, 0xA4, 0xA5, 0xA6, 0xA7, 0x8C):
                arg = repr(self.s(self.u32(o)))
                o += 4
            elif op == 0x83:
                arg = '%r %r' % (self.s(self.u32(o)), self.s(self.u32(o + 4)))
                o += 8
            elif op in (0x87, 0x81, 0x9F, 0x8B, 0x94):
                arg = str(struct.unpack_from('>i', self.d, o)[0])
                o += 4
            elif op in (0xA2, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3):
                arg = self.c(self.d[o])
                o += 1
            elif op == 0xA3:
                arg = self.c(struct.unpack_from('>H', self.d, o)[0])
                o += 2
            elif op in (0xB5, 0xB9):
                arg = str(self.d[o])
                o += 1
            elif op == 0xB6:
                arg = str(struct.unpack_from('>H', self.d, o)[0])
                o += 2
            elif op == 0xB7:
                arg = str(struct.unpack_from('>i', self.d, o)[0])
                o += 4
            elif op == 0xB4:
                arg = str(struct.unpack_from('>f', self.d, o)[0])
                o += 4
            elif op == 0x8E:
                fname = self.s(self.u32(o))
                nparams = self.u32(o + 4)
                nregs, flags = self.d[o + 8], int.from_bytes(self.d[o + 9:o + 12], 'big')
                plist = self.u32(o + 12)
                params = [self.s(self.u32(plist + 8 * i + 4)) for i in range(nparams)]
                size = self.u32(o + 16)
                o += 28
                out.append('%s%06X DefineFunction2 %s(%s) regs %d flags %06X size %d' % (pad, at, fname,
                                                                                       ', '.join(params), nregs,
                                                                                       flags, size))
                saved = self.pool
                self.run(o, o + size, depth + 1, out)
                self.pool = saved
                o += size
                continue
            elif op == 0x9B:
                fname = self.s(self.u32(o))
                nparams, plist = self.u32(o + 4), self.u32(o + 8)
                params = [self.s(self.u32(plist + 4 * i)) for i in range(nparams)]
                size = self.u32(o + 12)
                o += 24
                out.append('%s%06X DefineFunction %s(%s) size %d' % (pad, at, fname, ', '.join(params), size))
                self.run(o, o + size, depth + 1, out)
                o += size
                continue
            out.append('%s%06X %s %s' % (pad, at, name, arg))
            if op == 0x00 and depth == 0:
                break
        return out

    def find_function(self, name):
        target = self.d.find(name.encode() + b'\0')
        hits = []
        while target >= 0:
            if target == 0 or self.d[target - 1] == 0:
                for m in re.finditer(re.escape(struct.pack('>I', target)), self.d):
                    p = m.start()
                    for op_at in range(p - 4, p):
                        if op_at >= 0 and self.d[op_at] in (0x8E, 0x9B) and ((op_at + 4) & ~3) == p:
                            hits.append(op_at)
            target = self.d.find(name.encode() + b'\0', target + 1)
        return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('apt')
    ap.add_argument('names', nargs='*')
    ap.add_argument('--all', action='store_true', help='disassemble every action block (each starts with a '
                    'ConstantPool) to stdout')
    a = ap.parse_args()
    const = Const(os.path.splitext(a.apt)[0] + '.const')
    dis = Disasm(a.apt, const)
    if a.all:
        d = dis.d
        # ConstantPool: opcode, padding to 4, count, pointer to the count ids (inside the file).
        for o in range(0, len(d) - 12):
            if d[o] != 0x88:
                continue
            q = (o + 4) & ~3
            n, ptr = dis.u32(q), dis.u32(q + 4)
            if not (0 < n < 5000 and 0 < ptr < len(d) and ptr + 4 * n <= len(d)):
                continue
            if any(dis.u32(ptr + 4 * i) >= len(const.items) for i in range(n)):
                continue
            print('==== action block at %06X' % o)
            try:
                print('\n'.join(dis.run(o, len(d))))
            except (struct.error, IndexError) as e:
                print('  (stopped: %s)' % e)
        return
    for name in a.names:
        hits = dis.find_function(name)
        if not hits:
            print('%s: not found' % name)
        for h in hits:
            print('\n'.join(dis.run(h, h + 1 + 64 * 1024)[:400]))


if __name__ == '__main__':
    main()
