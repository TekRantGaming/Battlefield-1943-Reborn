"""The game's table of menu command and variable names (0x82020080 in the decrypted image): each entry a pointer to
the name and its number, as sub_8246D918 looks the menu's FSCommands up (docs/MENUS-PLAN.md). Prints number and name,
sorted by number.

    python -I tools/fscommand_table.py > build/fscommands.txt
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guest_at  # noqa: E402


def main():
    data, _ = guest_at.load()
    base = guest_at.BASE

    def word(a):
        return struct.unpack('>I', data[a - base:a - base + 4])[0]

    def text(a):
        if not 0x82000000 <= a < 0x83000000:
            return None
        o = a - base
        end = data.find(b'\0', o, o + 80)
        t = data[o:end]
        return t.decode('latin-1') if end > o and all(32 <= c < 127 for c in t) else None

    a = 0x82020200
    while text(word(a - 8)) and word(a - 4) < 0x400:
        a -= 8
    start, rows = a, []
    while text(word(a)) and word(a + 4) < 0x400:
        rows.append((word(a + 4), text(word(a))))
        a += 8
    print('table %08X to %08X, %d names' % (start, a, len(rows)))
    for number, name in sorted(rows):
        print('%3d 0x%02X %s' % (number, number, name))


if __name__ == '__main__':
    main()
