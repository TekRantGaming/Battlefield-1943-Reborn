"""Search a minidump's raw bytes for strings of interest (the crash reporter's text, overlay and layer names).
Usage: python -I grep_dump.py <dump path>
"""
import sys, re
raw = open(sys.argv[1], 'rb').read()
print('size', len(raw))
# ASCII and UTF-16 strings of 6+ characters that mention anything of interest
keys = [b'Battlefield 1943', b'Exception: 0x', b'Third party', b'Overlays hooked', b'Game thread', b'Game call stack',
        b'Minidump:', b'Reason:', b'Access:', b'RTSS', b'RivaTuner', b'VK_LAYER', b'VK_INSTANCE', b'overlay', b'Overlay',
        b'OBS', b'graphics-hook', b'Discord', b'EOSOVH', b'GameOverlay', b'Nahimic', b'nvspcap', b'amdxc', b'vulkan',
        b'Vulkan', b'vkQueuePresent', b'vkCreateSwapchain', b'DLSS', b'FidelityFX', b'Diligent', b'bf1943', b'rexruntime',
        b'BF ', b'0.3.0', b'sub_8', b'read of guest', b'write of guest']
found = {}
for k in keys:
    for enc in ('ascii', 'utf16'):
        pat = k if enc == 'ascii' else k.decode('ascii').encode('utf-16-le')
        for m in re.finditer(re.escape(pat), raw):
            o = m.start()
            if enc == 'ascii':
                s = o
                while s > 0 and 0x20 <= raw[s - 1] < 0x7f:
                    s -= 1
                e = m.end()
                while e < len(raw) and (0x20 <= raw[e] < 0x7f or raw[e] in (0x0a, 0x0d, 0x09)):
                    e += 1
                text = raw[s:e].decode('ascii', 'replace')
            else:
                s = o
                while s > 1 and 0x20 <= raw[s - 2] < 0x7f and raw[s - 1] == 0:
                    s -= 2
                e = m.end()
                while e + 1 < len(raw) and 0x20 <= raw[e] < 0x7f and raw[e + 1] == 0:
                    e += 2
                text = raw[s:e].decode('utf-16-le', 'replace')
            text = text.strip()
            if len(text) < 6:
                continue
            key = (enc, text[:400])
            if key in found:
                continue
            found[key] = o
limit = 400
for (enc, text), o in sorted(found.items(), key=lambda kv: kv[1]):
    if '\n' in text:
        print('--- %s @0x%x (multi-line) ---' % (enc, o))
        for line in text.split('\n')[:40]:
            print('   ', line.rstrip())
    else:
        print('%s @0x%x: %s' % (enc, o, text[:limit]))
