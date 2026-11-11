"""Unpack a Frostbite 1 .fbrb archive (Battlefield 1943, Xbox 360) into a folder. Our own reader.

Layout: "FbRB", big endian header size, the header (gzip), then the data (one gzip stream, or stored). Header:
version, string table size, string table, entry count, then per entry six big endian words: name offset, flags,
data offset, size, size again, type name offset (offsets into the string table and the unpacked data).

    python -I tools/fbrb_extract.py <archive.fbrb> <output folder> [--list] [--type SwfMovie]
"""
import argparse
import gzip
import os
import struct


def read(path):
    d = open(path, 'rb').read()
    if d[:4] != b'FbRB':
        raise ValueError('not an fbrb archive: %s' % path)
    hs = struct.unpack('>I', d[4:8])[0]
    h = gzip.decompress(d[8:8 + hs])
    raw = d[8 + hs:]
    data = gzip.decompress(raw) if raw[:2] == b'\x1f\x8b' else raw
    version, strsize = struct.unpack('>II', h[:8])
    strs = h[8:8 + strsize]
    rest = h[8 + strsize:]
    count = struct.unpack('>I', rest[:4])[0]

    def name(off):
        return strs[off:strs.find(b'\0', off)].decode('latin-1')

    entries = []
    for i in range(count):
        n, flags, off, size, size2, t = struct.unpack_from('>6I', rest, 4 + i * 24)
        entries.append({'name': name(n), 'type': name(t), 'flags': flags, 'offset': off, 'size': size,
                        'size2': size2})
    return version, entries, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('archive')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--type', help='only entries of this type name')
    a = ap.parse_args()
    version, entries, data = read(a.archive)
    for e in entries:
        if a.type and e['type'] != a.type:
            continue
        if a.list or not a.out:
            print('%-12s %9d %08X %s' % (e['type'], e['size'], e['flags'], e['name']))
            continue
        path = os.path.join(a.out, e['name'].replace('/', os.sep))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data[e['offset']:e['offset'] + e['size']])
    if a.out and not a.list:
        print('%d entries -> %s' % (len(entries), a.out))


if __name__ == '__main__':
    main()
