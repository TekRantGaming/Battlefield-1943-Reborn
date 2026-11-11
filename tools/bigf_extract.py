"""Extract a BIGF archive (the menus' .res files in Battlefield 1943's fbrb archives: an APT movie's .apt, .const and
.geo files). Our own tool, from the header as read here: "BIGF", the archive size (little endian), the file count
and the header size (big endian), then per file its offset and size (big endian) and its name (NUL terminated).

    python -I tools/bigf_extract.py <file.res> <out dir>
"""
import os
import struct
import sys


def extract(path, out):
    d = open(path, 'rb').read()
    if d[:4] != b'BIGF':
        sys.exit(f'{path}: not a BIGF archive')
    count, = struct.unpack('>I', d[8:12])
    pos = 16
    os.makedirs(out, exist_ok=True)
    names = []
    for _ in range(count):
        off, size = struct.unpack('>II', d[pos:pos + 8])
        end = d.index(b'\0', pos + 8)
        name = d[pos + 8:end].decode('latin-1')
        pos = end + 1
        safe = os.path.basename(name.replace('\\', '/'))
        with open(os.path.join(out, safe), 'wb') as f:
            f.write(d[off:off + size])
        names.append((safe, size))
    return names


if __name__ == '__main__':
    for name, size in extract(sys.argv[1], sys.argv[2]):
        print(f'{name} {size}')
