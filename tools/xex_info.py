"""Print the plain (unencrypted) header fields of an Xbox 360 XEX2 file: module flags, optional headers,
execution info (title ID, media ID, versions), import libraries, and a few other common fields.

    python -I tools/xex_info.py <default.xex>
"""
import struct
import sys

NAMES = {
    0x000002FF: 'resource info', 0x000003FF: 'base file format', 0x00010001: 'base address',
    0x00010100: 'entry point', 0x00010201: 'image base address', 0x000103FF: 'import libraries',
    0x00018002: 'checksum timestamp', 0x00018102: 'enabled for callcap', 0x00018200: 'enabled for fastcap',
    0x000183FF: 'original PE name', 0x000200FF: 'static libraries', 0x00020104: 'TLS info',
    0x00020200: 'default stack size', 0x00020301: 'default filesystem cache size',
    0x00020401: 'default heap size', 0x00028002: 'page heap size and flags', 0x00030000: 'system flags',
    0x00040006: 'execution info', 0x00040201: 'title workspace size', 0x00040310: 'game ratings',
    0x00040404: 'LAN key', 0x000405FF: 'Xbox 360 logo', 0x000406FF: 'multidisc media IDs',
    0x000407FF: 'alternate title IDs', 0x00040801: 'additional title memory', 0x00E10402: 'exports by name',
}


def main(path):
    d = open(path, 'rb').read()
    magic, flags, pe_off, _, sec_off, count = struct.unpack_from('>4sIIIII', d, 0)
    print('magic %s  module flags %08X  PE data offset %X  security info %X  %d optional headers' %
          (magic.decode(), flags, pe_off, sec_off, count))
    image_size = struct.unpack_from('>I', d, sec_off + 4)[0]
    load_addr = struct.unpack_from('>I', d, sec_off + 0x110)[0]
    print('image size %X  load address %08X' % (image_size, load_addr))
    for i in range(count):
        key, val = struct.unpack_from('>II', d, 0x18 + i * 8)
        name = NAMES.get(key, '?')
        size = key & 0xFF
        if size <= 1:
            print('  %08X %-28s value %08X' % (key, name, val))
            continue
        print('  %08X %-28s at %X' % (key, name, val))
        if key == 0x00040006:
            media, ver, base_ver, tid, plat, etype, disc, ndisc, savegame = struct.unpack_from(
                '>IIIIBBBBI', d, val)
            v = lambda x: '%d.%d.%d.%d' % (x >> 28, (x >> 24) & 0xF, (x >> 8) & 0xFFFF, x & 0xFF)
            print('      media ID %08X  version %s  base version %s  title ID %08X  disc %d/%d  savegame %08X'
                  % (media, v(ver), v(base_ver), tid, disc, ndisc, savegame))
        elif key == 0x000103FF:
            total, str_size, nlibs = struct.unpack_from('>III', d, val)
            names = d[val + 12:val + 12 + str_size].split(b'\0')
            names = [n.decode() for n in names if n]
            p = val + 12 + str_size
            for _ in range(nlibs):
                lsize = struct.unpack_from('>I', d, p)[0]
                ver, minver, name_idx, nimp = struct.unpack_from('>IIHH', d, p + 0x1C)
                v = lambda x: '%d.%d.%d.%d' % (x >> 28, (x >> 24) & 0xF, (x >> 8) & 0xFFFF, x & 0xFF)
                print('      %-12s version %s (min %s)  %d imports' % (names[name_idx], v(ver), v(minver), nimp))
                p += lsize
        elif key == 0x000200FF:
            n = (struct.unpack_from('>I', d, val)[0] - 4) // 16
            for j in range(n):
                nm, a, b, c, q = struct.unpack_from('>8sHHHH', d, val + 4 + j * 16)
                print('      %-8s %d.%d.%d.%d' % (nm.rstrip(b'\0').decode(), a, b, c, q & 0xFF))
        elif key == 0x000183FF:
            n = struct.unpack_from('>I', d, val)[0]
            print('      %s' % d[val + 4:val + n].split(b'\0')[0].decode())
        elif key == 0x000002FF:
            n = (struct.unpack_from('>I', d, val)[0] - 4) // 16
            for j in range(n):
                nm, addr, sz = struct.unpack_from('>8sII', d, val + 4 + j * 16)
                print('      resource %-8s at %08X size %X' % (nm.rstrip(b'\0').decode(), addr, sz))
        elif key == 0x000003FF:
            info_size, enc, comp = struct.unpack_from('>IHH', d, val)
            print('      encryption %d  compression %d' % (enc, comp))
        elif key == 0x00030000:
            pass
        elif size == 4 or (key & 0xFF) not in (0xFF,):
            print('      raw %s' % d[val:val + min(size * 4, 32)].hex())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
