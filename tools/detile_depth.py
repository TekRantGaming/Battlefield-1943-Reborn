"""Detile a dumped 32 bpp Xenos texture (D24S8 depth resolved to 8888) and save it as an image; optionally print
the depth at given UVs.  python detile_depth.py dump.bin width height out.png [u,v ...]"""
import struct
import sys

from PIL import Image


def tiled_offset(x, y, width, log_bpp):
    aligned_width = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpp
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F)) >> log_bpp


path, width, height, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
data = open(path, 'rb').read()
n = len(data) // 4
words = struct.unpack('>%dI' % n, data[:n * 4])
depth = [[0.0] * width for _ in range(height)]
img = Image.new('L', (width, height))
px = img.load()
for y in range(height):
    for x in range(width):
        i = tiled_offset(x, y, width, 2)
        d = (words[i] >> 8) / 16777215.0 if i < n else 1.0
        depth[y][x] = d
        # stretch the 0.93..1.0 range of the casters to the full grey scale
        px[x, y] = max(0, min(255, int((d - 0.93) / 0.07 * 255)))
img.save(out)
for uv in sys.argv[5:]:
    u, v = (float(t) for t in uv.split(','))
    x, y = min(width - 1, max(0, int(u * width))), min(height - 1, max(0, int(v * height)))
    ring = [depth[yy][xx] for yy in range(max(0, y - 2), min(height, y + 3)) for xx in range(max(0, x - 2), min(width, x + 3))]
    print('uv %.4f,%.4f -> texel %d,%d depth %.5f (5x5 min %.5f max %.5f)' % (u, v, x, y, depth[y][x], min(ring), max(ring)))
