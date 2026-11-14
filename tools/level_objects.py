"""The objects a Battlefield 1943 level places (its main partition, levels/<map>.dbx in reduced-level-00.fbrb): each
instance's blueprint and transform, as bf/src/night_lamps.cpp reads them (the same binary dbx reader, in Python).

    python -I tools/level_objects.py <game root> <level folder> [--csv out.csv] [--match text]

Prints the blueprints with their counts; --csv writes every placed object (blueprint, position, the up and forward
axes); --match keeps the blueprints containing that text.
"""
import argparse
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbrb_extract  # noqa: E402


class Reader:
    def __init__(self, d):
        self.d, self.n, self.i, self.out = d, len(d), 0, []
        self.in_instance, self.has_transform, self.placed = False, False, None

    def varint(self):
        v = 0
        for shift in range(0, 35, 7):
            c = self.d[self.i]
            self.i += 1
            v |= (c & 0x7F) << shift
            if not c & 0x80:
                return v
        raise ValueError('varint')

    def s(self, index):
        return self.strings[index - 1] if 0 < index <= len(self.strings) else ''

    def read(self):
        d = self.d
        if d[:8] != b'{binary}':
            raise ValueError('not a binary dbx')
        start, count = struct.unpack('>I', d[8:12])[0], struct.unpack('>I', d[20:24])[0]
        base = 28 + (count - 1) * 4
        self.strings = []
        for k in range(count - 1):
            at = base + struct.unpack('>I', d[28 + k * 4:32 + k * 4])[0]
            self.strings.append(d[at:d.index(b'\0', at)].decode('latin-1'))
        self.i = start
        while self.i < self.n and self.node(0):
            pass
        return self.out

    def node(self, depth):
        tag = self.varint()
        if tag == 0:
            return False
        flags = self.d[self.i]
        self.i += 1
        name = self.s(tag)
        attrs = {}
        for _ in range(flags & 0x0F):
            k = self.s(self.varint())
            attrs[k] = self.s(self.varint())
        kind = (flags >> 4) & 7
        value = None
        if kind == 2:
            self.varint()
        elif kind == 6:
            c = self.varint()  # (not self.i += self.varint(): self.i would be read before the varint moves it)
            self.i += c
        elif kind == 7:
            c = self.varint()
            size = self.d[self.i]
            self.i += 1
            value = (c, size, self.d[self.i:self.i + c * size])
            self.i += c * size
        elif kind != 0:
            raise ValueError('value kind %d' % kind)
        if depth == 2 and self.in_instance:
            if name == 'complex' and attrs.get('name') == 'Transform' and value and value[0] == 16 and value[1] == 4:
                self.placed['m'] = struct.unpack('>16f', value[2])
                self.has_transform = True
            elif name == 'field' and attrs.get('name') == 'ReferencedObject' and attrs.get('ref') not in (None, '', 'null'):
                self.placed['blueprint'] = attrs['ref']
        instance = depth == 1 and name == 'instance'
        if instance:
            self.in_instance, self.has_transform, self.placed = True, False, {'blueprint': ''}
        if flags & 0x80:
            while self.node(depth + 1):
                pass
        if instance:
            if self.has_transform and self.placed['blueprint']:
                self.out.append(self.placed)
            self.in_instance = False
        return True


def placed_objects(game_root, folder):
    archive = os.path.join(game_root, 'dist', 'xenon', 'levels', folder, 'reduced-level-00.fbrb')
    _, entries, data = fbrb_extract.read(archive)
    want = [e for e in entries if e['name'].lower() == 'levels/%s.dbx' % folder.lower()]
    if not want:
        raise ValueError('no levels/<map>.dbx in %s' % archive)
    e = want[0]
    return e['name'], Reader(data[e['offset']:e['offset'] + e['size']]).read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_root')
    ap.add_argument('folder')
    ap.add_argument('--csv')
    ap.add_argument('--match', default='')
    a = ap.parse_args()
    name, objs = placed_objects(a.game_root, a.folder)
    objs = [o for o in objs if a.match.lower() in o['blueprint'].lower()]
    print('%s: %d placed objects' % (name, len(objs)))
    counts = collections.Counter(o['blueprint'].rsplit('/', 1)[0] for o in objs)
    for bp, c in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print('%5d  %s' % (c, bp))
    if a.csv:
        with open(a.csv, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['blueprint', 'x', 'y', 'z', 'up_x', 'up_y', 'up_z', 'fwd_x', 'fwd_y', 'fwd_z'])
            for o in objs:
                m = o['m']
                w.writerow([o['blueprint']] + ['%.2f' % v for v in (m[12], m[13], m[14], m[4], m[5], m[6], m[8], m[9], m[10])])


if __name__ == '__main__':
    main()
