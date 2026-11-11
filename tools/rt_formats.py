import collections
import glob
import struct
import sys

import xxhash

NAMES = {0: '8_8_8_8', 1: '8_8_8_8_GAMMA', 2: '2_10_10_10', 3: '2_10_10_10_FLOAT', 4: '16_16', 5: '16_16_16_16',
         6: '16_16_FLOAT', 7: '16_16_16_16_FLOAT', 10: '2_10_10_10_AS_10_10_10_10',
         12: '2_10_10_10_FLOAT_AS_16_16_16_16', 14: '32_FLOAT', 15: '32_32_FLOAT'}

rt_count = collections.Counter()
depth_only = collections.Counter()
combos = collections.Counter()
for path in sys.argv[1:]:
    for f in glob.glob(path):
        d = open(f, 'rb').read()
        for at in range(12, len(d) - 71, 72):
            r = d[at:at + 72]
            if xxhash.xxh3_64_intdigest(r[8:]) != struct.unpack_from('<Q', r)[0]:
                continue
            desc = r[8:]
            vs, vsm, ps, psm = struct.unpack_from('<4Q', desc)
            w1, w2 = struct.unpack_from('<II', desc, 40)
            depth_format = (w1 >> 13) & 1
            depth_write = (w1 >> 17) & 1
            rts = struct.unpack_from('<4I', desc, 48)
            used = []
            for i, rt in enumerate(rts):
                if rt & 1:
                    fmt = (rt >> 1) & 0xF
                    used.append(NAMES.get(fmt, str(fmt)) + ('' if (rt >> 27) & 0xF else '(nowrite)'))
                    rt_count[NAMES.get(fmt, str(fmt))] += 1
            if not used:
                depth_only['ps=%s depth_write=%d D%s' % ('none' if ps == 0 else 'yes', depth_write,
                                                          '24FS8' if depth_format else '24S8')] += 1
            combos[' + '.join(used) or 'depth only'] += 1
print('render target formats (pipelines using each):')
for k, v in rt_count.most_common():
    print('  %-34s %d' % (k, v))
print('combinations:')
for k, v in combos.most_common():
    print('  %-60s %d' % (k, v))
print('depth only pipelines:')
for k, v in depth_only.most_common():
    print('  %-40s %d' % (k, v))
