"""Minimal reader for Xbox 360 STFS packages (CON, LIVE, PIRS).

Written for this project from the public format description (Free60 wiki). It walks the file table, follows
each file's block chain through the level 0 hash tables, checks every data block's SHA1 against its hash
entry, and writes the files out. Usage:

    python -I tools/stfs_extract.py <package> <output dir> [--manifest manifest.tsv]
"""
import argparse
import hashlib
import os
import struct
import sys

BLOCK = 0x1000
PER_TABLE = 0xAA        # hash entries per hash table block
L1_SPAN = 0xAA * 0xAA   # 0x70E4 blocks covered by one level 1 table


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def u24be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


class Stfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(0xB000)
        self.magic = h[0:4]
        if self.magic not in (b'CON ', b'LIVE', b'PIRS'):
            raise ValueError('not an STFS package: %r' % self.magic)
        self.header_size = struct.unpack_from('>I', h, 0x340)[0]
        self.content_type = struct.unpack_from('>I', h, 0x344)[0]
        self.title_id = struct.unpack_from('>I', h, 0x360)[0]
        vd = 0x379
        self.block_sep = h[vd + 2]
        self.ft_blocks = struct.unpack_from('<H', h, vd + 3)[0]
        self.ft_start = u24le(h, vd + 5)
        self.top_hash = h[vd + 8:vd + 28]
        self.alloc_blocks = struct.unpack_from('>I', h, vd + 28)[0]
        self.display_name = h[0x411:0x411 + 0x80].decode('utf-16-be').split('\0')[0]
        # Bit 0 of block separation set: one hash table per level ("female"), else two.
        self.shift = 0 if (self.block_sep & 1) else 1
        self.first_table = (self.header_size + 0xFFF) & ~0xFFF
        self.step0 = 0xAB if self.shift == 0 else 0xAC

    def backing_block(self, n):
        r = (((n + PER_TABLE) // PER_TABLE) << self.shift) + n
        if n < PER_TABLE:
            return r
        if n < L1_SPAN:
            return r + (((n + L1_SPAN) // L1_SPAN) << self.shift)
        return (1 << self.shift) + r + (((n + L1_SPAN) // L1_SPAN) << self.shift)

    def l0_hash_block(self, n):
        if n < PER_TABLE:
            return 0
        r = (n // PER_TABLE) * self.step0
        r += ((n // L1_SPAN) + 1) << self.shift
        if n // L1_SPAN == 0:
            return r
        return r + (1 << self.shift)

    def read_block(self, n):
        self.f.seek(self.first_table + self.backing_block(n) * BLOCK)
        return self.f.read(BLOCK)

    def hash_entry(self, n):
        # Two table packages may keep the live copy in the second table; this reader only handles the first.
        off = self.first_table + self.l0_hash_block(n) * BLOCK + (n % PER_TABLE) * 0x18
        self.f.seek(off)
        e = self.f.read(0x18)
        return e[:20], e[20], u24be(e, 21)

    def chain(self, start, count, consecutive):
        n = start
        for i in range(count):
            yield n
            if consecutive:
                n += 1
            else:
                n = self.hash_entry(n)[2]

    def files(self):
        raw = b''.join(self.read_block(b) for b in self.chain(self.ft_start, self.ft_blocks, False))
        out = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            if e[0] == 0:
                break
            flags = e[0x28]
            out.append({
                'index': i // 0x40,
                'name': e[:flags & 0x3F].decode('ascii', 'replace'),
                'dir': bool(flags & 0x80),
                'consecutive': bool(flags & 0x40),
                'blocks': u24le(e, 0x29),
                'start': u24le(e, 0x2F),
                'parent': struct.unpack_from('>h', e, 0x32)[0],
                'size': struct.unpack_from('>I', e, 0x34)[0],
            })
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('package')
    ap.add_argument('outdir')
    ap.add_argument('--manifest')
    a = ap.parse_args()
    s = Stfs(a.package)
    print('magic %s  content type %08X  title id %08X  name %r' %
          (s.magic.decode(), s.content_type, s.title_id, s.display_name))
    print('header size %X  block sep %d  file table %d blocks at %d  allocated blocks %d' %
          (s.header_size, s.block_sep, s.ft_blocks, s.ft_start, s.alloc_blocks))
    entries = s.files()
    paths = {}
    for e in entries:
        parent = paths.get(e['parent'], '') if e['parent'] >= 0 else ''
        paths[e['index']] = os.path.join(parent, e['name']) if parent else e['name']
    rows = []
    bad_total = 0
    for e in entries:
        p = os.path.join(a.outdir, paths[e['index']])
        if e['dir']:
            os.makedirs(p, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(p) or a.outdir, exist_ok=True)
        sha = hashlib.sha256()
        left = e['size']
        bad = 0
        with open(p, 'wb') as o:
            for n in s.chain(e['start'], e['blocks'], e['consecutive']):
                data = s.read_block(n)
                if hashlib.sha1(data).digest() != s.hash_entry(n)[0]:
                    bad += 1
                chunk = data[:min(BLOCK, left)]
                o.write(chunk)
                sha.update(chunk)
                left -= len(chunk)
        bad_total += bad
        rows.append((paths[e['index']].replace('\\', '/'), e['size'], sha.hexdigest(), bad))
    for r in rows:
        print('%-60s %12d %s%s' % (r[0], r[1], r[2], '  BAD BLOCKS %d' % r[3] if r[3] else ''))
    print('%d files, %d bytes, %d blocks failed SHA1' % (len(rows), sum(r[1] for r in rows), bad_total))
    if a.manifest:
        with open(a.manifest, 'w', encoding='utf-8', newline='\n') as m:
            m.write('path\tsize\tsha256\n')
            for r in rows:
                m.write('%s\t%d\t%s\n' % r[:3])
    return 1 if bad_total else 0


if __name__ == '__main__':
    sys.exit(main())
