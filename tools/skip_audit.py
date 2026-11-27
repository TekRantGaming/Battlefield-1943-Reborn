"""The draws NativeXRenderer's own rules leave out (D249), each named by its pixel shader's constant table in the game's
files: its samplers and constants, and a flag where they are not the sky's. Ours: names and hashes only, no shader code.

    python -I tools/skip_audit.py --game bf/assets runs/<run>/bf1943.log [more logs]
    python -I tools/skip_audit.py --game bf/assets --ps 0365AC9437C0240E,8ABF56776060E64C

The renderer logs each distinct skipped draw once ("rule skip: <rule>: PS <hash> VS <hash>, <n> vertices, target WxH").
D243 is why: the own sky's rule had taken every map's tyre-track pass, whose constant table names the outdoor light, the
sun's shadow map and the heightmap; a sky's draw samples sky textures, not those. The tables are read as
tools/light_constants.py reads them (the game's .fbrb archives under <game>/dist).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbrb_extract  # noqa: E402
import light_constants  # noqa: E402

LINE = re.compile(r'rule skip: (?P<rule>.*?): PS (?P<ps>[0-9A-F]{16}) VS (?P<vs>[0-9A-F]{16}), (?P<n>\d+) vertices, '
                  r'target (?P<w>\d+)x(?P<h>\d+)(?:, (?P<dome>-?\d+) draws after the sky dome)?')
# What a sky's draw does not take: the ground, the sun's shadows, surfaces' maps.
NOT_SKY = re.compile(r'heightmap|shadow|diffuse|normal|detail|mask|terrain|tread|occlusion|specular|bump|lightmap',
                     re.IGNORECASE)
NOT_SKY_CONSTANTS = ('outdoorLightShadowTransform0', 'outdoorLightDir')


def tables(game, wanted):
    """{pixel shader hash: {name: (register set, register, count)}} for the hashes in `wanted`."""
    found = {}
    for root, _, files in os.walk(os.path.join(game, 'dist')):
        for name in sorted(files):
            if not name.endswith('.fbrb') or len(found) == len(wanted):
                continue
            try:
                _, entries, data = fbrb_extract.read(os.path.join(root, name))
            except (ValueError, OSError, EOFError) as e:
                print('not read:', name, e, file=sys.stderr)
                continue
            for e in entries:
                blob = data[e['offset']:e['offset'] + e['size']]
                for at, h in light_constants.pixel_shaders(blob):
                    if h in wanted and h not in found:
                        table = light_constants.constant_table(blob, at, True)
                        if table is not None:
                            found[h] = table
    return found


def verdict(table):
    samplers = sorted(k for k, v in table.items() if v[0] == 3)
    constants = sorted(k for k, v in table.items() if v[0] != 3)
    bad = [s for s in samplers if NOT_SKY.search(s)] + [c for c in constants if c in NOT_SKY_CONSTANTS]
    generic = samplers and all(re.match(r'^sampler_Texture\d*$', s) for s in samplers)
    if bad:
        return 'NOT SKY? ' + ', '.join(bad), samplers, constants
    if generic:
        return 'generic samplers only: look at it', samplers, constants
    return 'sky-like', samplers, constants


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True, help='the game folder (its dist folder inside)')
    ap.add_argument('--ps', help='pixel shader hashes, comma separated, instead of logs')
    ap.add_argument('logs', nargs='*')
    a = ap.parse_args()
    skips = {}  # ps -> [(rule, vs, vertices, target)]
    if a.ps:
        for h in a.ps.split(','):
            skips.setdefault(int(h, 16), []).append(('(given)', '', '', ''))
    for path in a.logs:
        for line in open(path, encoding='utf-8', errors='replace'):
            m = LINE.search(line)
            if m:
                dome = f", {m['dome']} after the dome" if m['dome'] is not None else ''
                entry = (m['rule'], m['vs'], m['n'], f"{m['w']}x{m['h']}{dome}")
                lst = skips.setdefault(int(m['ps'], 16), [])
                if entry not in lst:
                    lst.append(entry)
    if not skips:
        sys.exit('no rule skips in the logs (and no --ps)')
    found = tables(a.game, set(skips))
    flagged = 0
    for ps in sorted(skips):
        if ps in found:
            v, samplers, constants = verdict(found[ps])
        else:
            v, samplers, constants = 'not in the game files (a vertex shader hash? another game?)', [], []
        flagged += v.startswith('NOT SKY')
        print(f'{ps:016X}  {v}')
        for rule, vs, n, target in skips[ps]:
            print(f'    {rule}  VS {vs} {n} vertices {target}'.rstrip())
        if samplers:
            print('    samplers: ' + ', '.join(samplers))
        if constants:
            print('    constants: ' + ', '.join(constants))
    print(f'\n{len(skips)} pixel shaders, {len(found)} found in the game files, {flagged} flagged NOT SKY')


if __name__ == '__main__':
    main()
