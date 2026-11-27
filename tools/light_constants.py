"""Where each of the game's shaders takes its outdoor light from: the float constant registers of the sun's direction
and colour and of the sky's and the ground's light, by shader hash. Ours: hashes and register numbers, no shader code.

    python -I tools/light_constants.py --game bf/assets --out bf/third_party/native-x-renderer/src/bf_lights.inc         --storage runs/*/user/cache/native_x_renderer/5841097E.pipelines

The game's shader resources (Levels/<level>/Shaders/Single.res) hold every shader as an Xbox 360 shader container
(magic 102A1100 pixel, 102A1101 vertex) with Direct3D's constant table in it: after the version word (FFFF0300 pixel,
FFFE0300 vertex; the table starts 8 bytes before it) a count and an offset to 20 byte entries {name offset, register
set (2: float), register, count, reserved, type, default}, all big endian, the offsets from the table's start. The
shaders are found through bf/data/shaders/bf1943-shaders.list (tools/shader_list.py): its hash is the runtime's, and
its offset is the microcode's place in the resource, whose container is the last one that starts before it.

14 Nov 2026: 1,111 shaders, 737 of them with the outdoor light's colours or direction, and the vertex shaders of what
receives the sun's shadows (outdoorLightShadowTransform0 to 2).

17 Nov 2026: a last column, 1 for the 71 pixel shaders that sample outdoorLightReflectionEnvmap (window glass, water,
shiny metal): a cube map of a day's sky, which they multiply by outdoorLightKeySpecularColorAndHemisphereVisibility's w
(their disassembly: tfetchCube from tf1, then times c6.w), so the renderer can dim the reflection at night.

18 Nov 2026 (D199): also every pixel shader in the game's archives, not only the list's (which has the shaders that
test runs had recorded): 105 more that take the outdoor light, the first person hands and weapon among them, which
had kept the day's light at night. A pixel shader's container gives its microcode's place (after the constant table's
part, past its literal constants) and size; the runtime names it by the microcode's XXH3 (tools/xxh3.py). A vertex
shader's microcode as the runtime has it differs from the files' in its vertex fetch fields, filled in at bind time,
so its name cannot be made from the files: --storage takes NativeXRenderer's shader storages from test runs ('S'
records: a hash and the microcode the runtime saw), and a vertex shader there that the list lacks gets the constants
of the files' containers of its size that differ from it in few dwords, when they all agree (14 skinned shaders, the
soldiers' and the arms' among them, whose shadow transforms the moved sun's turn had missed).
"""
import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbrb_extract  # noqa: E402
from xxh3 import xxh3_64  # noqa: E402

# The columns of the table, in its order.
NAMES = ('outdoorLightDir', 'outdoorLightHemisphereDir', 'outdoorLightTopColor', 'outdoorLightBottomColor',
         'outdoorLightKeyColor', 'outdoorLightKeySpecularColorAndHemisphereVisibility', 'cameraPos',
         'outdoorLightShadowTransform0')
# How many registers each takes (the shadow transform's three rows; the second and third cascades' follow at +4, +8).
COUNTS = {'outdoorLightShadowTransform0': 3}


def constant_table(blob, at, pixel):
    """{name: (register set, register, count)} of the container that holds the microcode at `at`, or None."""
    start = blob.rfind(b'\x10\x2a\x11\x00' if pixel else b'\x10\x2a\x11\x01', 0, at)
    if start < 0:
        return None
    v = blob.find(b'\xff\xff\x03\x00' if pixel else b'\xff\xfe\x03\x00', start, at)
    if v < 8:
        return None
    t = v - 8
    size, _, _, count, info = struct.unpack_from('>5I', blob, t)
    if size != 0x1c or count > 300:
        return None
    out = {}
    for i in range(count):
        n, rset, reg, cnt = struct.unpack_from('>IHHH', blob, t + info + i * 20)
        out[blob[t + n:blob.find(b'\0', t + n)].decode('latin-1')] = (rset, reg, cnt)
    return out


def pixel_shaders(blob):
    """Every pixel shader container in an archive entry: (microcode's place, its XXH3)."""
    out = []
    s = blob.find(b'\x10\x2a\x11\x00')
    while s >= 0:
        try:
            # The header: the constant table's part's size, the data part's, ..., at +20 the program's information,
            # whose words 10 and 11 are the literal constants' size and the microcode's (both in bytes).
            head, info = struct.unpack_from('>I12xI', blob, s + 4)
            literals, size = struct.unpack_from('>2I', blob, s + info + 40)
            at = s + head + literals
            if 17 <= size <= 0x40000 and size % 4 == 0 and at + size <= len(blob):
                out.append((at, xxh3_64(blob[at:at + size])))
        except struct.error:
            pass
        s = blob.find(b'\x10\x2a\x11\x00', s + 4)
    return out


def vertex_shaders(blob):
    """Every vertex shader container in an archive entry: (microcode's place, its size in bytes)."""
    out = []
    s = blob.find(b'\x10\x2a\x11\x01')
    while s >= 0:
        try:
            head, info = struct.unpack_from('>I12xI', blob, s + 4)
            literals, size = struct.unpack_from('>2I', blob, s + info + 40)
            at = s + head + literals
            if 17 <= size <= 0x40000 and size % 4 == 0 and at + size <= len(blob):
                out.append((at, size))
        except struct.error:
            pass
        s = blob.find(b'\x10\x2a\x11\x01', s + 4)
    return out


def storage_shaders(path):
    """NativeXRenderer's shader storage: {hash: (type, microcode)} of its 'S' records."""
    data = open(path, 'rb').read()
    out = {}
    if data[:4] != b'XNPS':
        return out
    state_size = struct.unpack_from('<I', data, 8)[0]
    p = 16
    while p < len(data):
        tag = data[p:p + 1]
        if tag == b'S':
            kind, dwords, h = struct.unpack_from('<BIQ', data, p + 1)
            out[h] = (kind, data[p + 14:p + 14 + dwords * 4])
            p += 14 + dwords * 4
        elif tag == b'T':
            p += 18
        elif tag == b'P':
            p += 33 + state_size
        else:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True, help='the game folder (dist)')
    ap.add_argument('--list', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bf', 'data',
                                                   'shaders', 'bf1943-shaders.list'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--storage', nargs='*', default=[], help="NativeXRenderer's shader storages (globs) from test runs")
    a = ap.parse_args()
    archives = {}
    rows = []
    shaders = 0
    named = set()  # every shader looked at

    def row(table, h, pixel):
        regs = []
        for n in NAMES:
            rset, reg, cnt = table.get(n, (2, 255, 1))
            regs.append(reg if rset == 2 and cnt == COUNTS.get(n, 1) and reg < 255 else 255)
        if 'outdoorLightShadowTransform0' in table and (table.get('outdoorLightShadowTransform1', (0, 0, 0))[1] != regs[7] + 4 or
                                                        table.get('outdoorLightShadowTransform2', (0, 0, 0))[1] != regs[7] + 8):
            print('shadow transforms not at +4, +8: %016x' % h, file=sys.stderr)
            regs[7] = 255
        if any(r != 255 for r in regs[:6]) or regs[7] != 255:
            envmap = table.get('outdoorLightReflectionEnvmap', (0, 0, 0))[0] == 3  # a sampler
            return (h, 1 if pixel else 0, regs + [1 if envmap else 0])
        return None

    for line in open(a.list):
        p = line.split()
        if len(p) < 7 or p[0].startswith('#') or p[3] != 'fbrb':
            continue
        if p[4] not in archives:
            _, entries, data = fbrb_extract.read(os.path.join(a.game, p[4]))
            archives[p[4]] = ({e['name']: e for e in entries}, data)
        entries, data = archives[p[4]]
        e = entries[p[5]]
        table = constant_table(data[e['offset']:e['offset'] + e['size']], int(p[6]), p[1] == 'p')
        if table is None:
            print('no constant table:', line.strip(), file=sys.stderr)
            continue
        shaders += 1
        named.add(int(p[0], 16))
        r = row(table, int(p[0], 16), p[1] == 'p')
        if r:
            rows.append(r)
    # Every pixel shader in the game's archives (the list has only those test runs recorded).
    listed = shaders
    vertex_containers = {}  # microcode size: [(entry, place)]
    for root, _, files in os.walk(os.path.join(a.game, 'dist')):
        for name in sorted(files):
            if not name.endswith('.fbrb'):
                continue
            try:
                _, entries, data = fbrb_extract.read(os.path.join(root, name))
            except (ValueError, OSError, EOFError) as e:
                print('not read:', name, e, file=sys.stderr)
                continue
            for e in entries:
                blob = data[e['offset']:e['offset'] + e['size']]
                for at, h in pixel_shaders(blob):
                    if h in named:
                        continue
                    named.add(h)
                    table = constant_table(blob, at, True)
                    if table is None:
                        continue
                    shaders += 1
                    r = row(table, h, True)
                    if r:
                        rows.append(r)
            if a.storage:
                for e in entries:
                    blob = data[e['offset']:e['offset'] + e['size']]
                    for at, size in vertex_shaders(blob):
                        vertex_containers.setdefault(size, []).append((blob, at))
    print('%d shaders from the list, %d more pixel shaders from the archives' % (listed, shaders - listed))
    # Vertex shaders the runtime saw that the list lacks.
    seen = {}
    for pattern in a.storage:
        for path in glob.glob(pattern, recursive=True):
            for h, (kind, code) in storage_shaders(path).items():
                if kind == 0 and h not in named:
                    seen.setdefault(h, code)
    added = 0
    for h, code in sorted(seen.items()):
        found = set()
        for blob, at in vertex_containers.get(len(code), []):
            mine = blob[at:at + len(code)]
            differ = sum(1 for i in range(0, len(code), 4) if mine[i:i + 4] != code[i:i + 4])
            if differ <= max(8, len(code) // 32):  # an eighth of the dwords at most: the fetch fields
                table = constant_table(blob, at, False)
                if table is not None:
                    r = row(table, h, False)
                    found.add(r and (r[0], r[1], tuple(r[2])))
        if len(found) != 1:
            print('vertex shader %016x: %d different readings of the files' % (h, len(found)), file=sys.stderr)
            continue
        named.add(h)
        shaders += 1
        r = found.pop()
        if r:
            rows.append(r)
            added += 1
    print('%d vertex shaders from the storages, %d of them with the outdoor light' % (len(seen), added))
    rows.sort()
    with open(a.out, 'w', newline='\n') as f:
        f.write('// Generated by tools/light_constants.py from the game\'s shader constant tables: do not edit.\n')
        f.write('// {shader hash, 1 pixel / 0 vertex, the float constant of: outdoorLightDir, outdoorLightHemisphereDir,\n')
        f.write('// outdoorLightTopColor, outdoorLightBottomColor, outdoorLightKeyColor,\n')
        f.write('// outdoorLightKeySpecularColorAndHemisphereVisibility, cameraPos, outdoorLightShadowTransform0 (three rows;\n')
        f.write('// the second and the third cascades\' at +4 and +8) (255: the shader has none), then 1 if it samples\n')
        f.write('// outdoorLightReflectionEnvmap (a day sky\'s cube map, times the specular constant\'s w)}. Sorted by hash.\n')
        for h, pixel, regs in rows:
            f.write('{0x%016Xull, %d, %s},\n' % (h, pixel, ', '.join(str(r) for r in regs)))
    print('%d shaders read, %d with the outdoor light -> %s' % (shaders, len(rows), a.out))


if __name__ == '__main__':
    main()
