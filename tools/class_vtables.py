"""Battlefield 1943's data classes (Frostbite's reflection): for every class registered with the type system, its
name, static entry, runtime class object, size and the vtable its constructor writes at +0 of each instance. Data
objects loaded from the game's assets start with that vtable, so a memory scan for it finds every instance
(bf/src/modern_mode.cpp).

    python -I tools/class_vtables.py [--name HaloAimerData] > build/class_vtables.txt

How: each class is registered by a call to sub_8261A830(r3 = class object, r4 = static entry, r5 = parent class
object, r6 = create function, r7 = field table) in the static initializers (0x82A00000 on). The create function
allocates the size and calls the constructor, which stores the vtable (lis/addi into a register, stw to 0(r31)).
"""
import argparse
import os
import struct

BASE = 0x82000000
REGISTER = 0x8261A830


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', default='')
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    img = open(os.path.join(root, 'input', 'image.bin'), 'rb').read()

    def u32(addr):
        return struct.unpack_from('>I', img, addr - BASE)[0]

    def s16(x):
        return x - 0x10000 if x & 0x8000 else x

    def cstr(addr):
        if not (BASE <= addr < BASE + len(img)):
            return ''
        end = img.index(b'\0', addr - BASE)
        s = img[addr - BASE:end].decode('latin1')
        return s if s.isprintable() else ''

    def branch_target(addr):
        w = u32(addr)
        if (w >> 26) != 18 or (w & 3) != 1:
            return None
        off = w & 0x03FFFFFC
        if off & 0x02000000:
            off -= 0x04000000
        return (addr + off) & 0xFFFFFFFF

    def constant_regs(start, end):
        regs, his = {}, {}
        for b in range(start, end, 4):
            x = u32(b)
            op, rd, ra, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
            if op == 15 and ra == 0:
                his[rd] = imm << 16
            elif op == 14 and ra in his:
                regs[rd] = (his[ra] + s16(imm)) & 0xFFFFFFFF
        return regs

    def vtable_of(create):
        # The create function: "li r3,size; bl alloc ... li r4,0; bl constructor".
        size, ctor = None, None
        for b in range(create, create + 0x60, 4):
            x = u32(b)
            if (x >> 26) == 14 and ((x >> 16) & 31) == 0 and ((x >> 21) & 31) == 3:
                size = x & 0xFFFF
            t = branch_target(b)
            if t and t != 0x8260B6F8 and u32(b - 4) == 0x38800000:  # li r4,0 just before: the constructor
                ctor = t
                break
        if not ctor:
            return size, None, None
        # The constructor: the first "stw rX,0(r31)" (or r3) whose rX holds a constant pointer into the image.
        his, regs = {}, {}
        for b in range(ctor, ctor + 0x400, 4):
            x = u32(b)
            op, rd, ra, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
            if op == 15 and ra == 0:
                his[rd] = imm << 16
            elif op == 14 and ra in his:
                regs[rd] = (his[ra] + s16(imm)) & 0xFFFFFFFF
            elif op == 36 and imm == 0 and ra in (31, 30, 3) and rd in regs and 0x82000000 <= regs[rd] < 0x82100000:
                return size, ctor, regs[rd]
            if x == 0x4E800020:
                break
        return size, ctor, None

    for addr in range(0x82A00000, 0x82AE0000, 4):
        if branch_target(addr) != REGISTER:
            continue
        regs = constant_regs(addr - 4 * 24, addr)
        entry = regs.get(4)
        if not entry:
            continue
        name = cstr(u32(entry))
        if not name or (a.name and name != a.name):
            continue
        create = regs.get(6, 0)
        size, ctor, vtable = vtable_of(create) if create else (None, None, None)
        print('%-44s entry %08X class %08X parent %08X fields %08X create %08X size %s vtable %s' % (
            name, entry, regs.get(3, 0), regs.get(5, 0), regs.get(7, 0), create,
            '%#x' % size if size is not None else '?', '%08X' % vtable if vtable else '?'))


if __name__ == '__main__':
    main()
