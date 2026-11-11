"""Source swizzles of texture fetches in a dumped Xenos microcode file (all three components, also for 2D)."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
for order in ('<', '>'):
    words = struct.unpack('%s%dI' % (order, len(d) // 4), d[:len(d) // 4 * 4])
    hits = []
    for i in range(0, len(words) - 2, 3):
        w0, w1, w2 = words[i:i + 3]
        opcode = w0 & 0x1F
        if opcode != 1:  # kTextureFetch
            continue
        src = (w0 >> 5) & 0x3F
        const = (w0 >> 20) & 0x1F
        swiz = (w0 >> 26) & 0x3F
        dim = (w2 >> 14) & 3
        ox = ((w2 >> 16) & 0x1F)
        comps = 'xyzw'
        hits.append('r%d.%s%s%s tf%d dim %d offset_x %d' % (src, comps[swiz & 3], comps[(swiz >> 2) & 3],
                                                          comps[(swiz >> 4) & 3], const, dim, ox))
    print(order, len(hits), 'fetches')
    for h in hits:
        print('  ', h)
