"""Build our shader list from recorded shader storage: for every shader the runtime saw, where its Xenos microcode
sits in the player's own game files, so setup on the player's PC (bf/src/shader_prep.cpp) can write the runtime's
shader storage without us shipping any of the game's shader code.

    python -I tools/shader_list.py --game input/pkg --out bf/assets/shaders runs/sh1/user/cache/shaders/shareable ...

Inputs are folders with the runtime's storage files from test runs: <title>.xsh (shaders) and <title>.*.xpso
(pipeline descriptions). Outputs in --out:
  bf1943-shaders.list   one line per shader: hash, type, dword count, source, patches (text, ours)
  <title>.*.xpso        the merged pipeline descriptions (shader hashes and render state; no shader code)

Sources: "fbrb <archive> <entry> <offset>" (a byte offset into that archive entry) or "xex <guest address>" (the
shader is built into default.xex). Vertex shaders differ from the files in a few dwords, the vertex fetch fields the
game's D3D fills in at bind time; those are kept as "<dword index>:<xor>" pairs.

NativeXRenderer (our Vulkan renderer, what players run) keeps its own storage, <cache>/native_x_renderer/<title>.pipelines:
'XNPS', u32 version, u32 PipelineState size, u32 0, then tagged records: 'S' u8 type, u32 dwords, u64 XXH3, microcode;
'T' u8 type, u64 shader hash, u64 modification (a translation); 'P' u64 vertex hash, u64 vertex modification, u64
pixel hash, u64 pixel modification, PipelineState (a pipeline, render state only). Its shaders join the list; its
translations and pipelines go to --out as <title>.nxr (the same file with no 'S' records: no shader code), which
shader_prep.cpp turns into the player's storage with the shaders from their game files.

Storage formats (host byte order): .xsh 'XESH', u32 version, then {u64 XXH3 of the microcode, u32 dword count |
type << 31 (1 pixel, 0 vertex), microcode}; .xpso 'XEPS', u32 kind, u32 version, then 72 byte records whose first
u64 is the XXH3 of the other 64 bytes.
"""
import argparse
import glob
import os
import struct
import sys

import xxhash

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbrb_extract  # noqa: E402

SHADER_TYPES = ('Dx9ShaderDatabase', 'XenonPixelShader', 'XenonVertexShader')


def read_xsh(path):
    d = open(path, 'rb').read()
    out, at = [], 8
    while at + 12 <= len(d):
        h, w = struct.unpack_from('<QI', d, at)
        n = w & 0x7FFFFFFF
        u = d[at + 12:at + 12 + n * 4]
        if len(u) < n * 4 or xxhash.xxh3_64_intdigest(u) != h:
            break
        out.append((h, w >> 31, u))
        at += 12 + n * 4
    return d[:8], out


def read_xpso(path):
    d = open(path, 'rb').read()
    recs = []
    for at in range(12, len(d) - 71, 72):
        r = d[at:at + 72]
        if xxhash.xxh3_64_intdigest(r[8:]) == struct.unpack_from('<Q', r)[0]:
            recs.append(r)
    return d[:12], recs


NXR_MAGIC = 0x53504E58


def read_nxr(path):
    """A NativeXRenderer storage file: (header, {(type, hash): ucode}, [translation records], [pipeline records])."""
    d = open(path, 'rb').read()
    if len(d) < 16 or struct.unpack_from('<I', d)[0] != NXR_MAGIC:
        return None
    state = struct.unpack_from('<I', d, 8)[0]
    shaders, trans, pipes, at = {}, [], [], 16
    while at < len(d):
        tag = d[at]
        if tag == ord('S'):
            if at + 14 > len(d):
                break
            t, n, h = struct.unpack_from('<BIQ', d, at + 1)
            u = d[at + 14:at + 14 + n * 4]
            if len(u) < n * 4:
                break
            shaders[(t, h)] = u
            at += 14 + n * 4
        elif tag == ord('T'):
            if at + 18 > len(d):
                break
            trans.append(d[at:at + 18])
            at += 18
        elif tag == ord('P'):
            if at + 33 + state > len(d):
                break
            pipes.append(d[at:at + 33 + state])
            at += 33 + state
        else:
            break
    return d[:16], shaders, trans, pipes


def game_sources(game):
    """Every shader resource in the game's archives: (archive path relative to the game folder, entry, bytes)."""
    for path in sorted(glob.glob(os.path.join(game, 'dist', 'xenon', '**', '*.fbrb'), recursive=True)):
        rel = os.path.relpath(path, game).replace(os.sep, '/')
        if '/vo/' in rel or 'sounds' in rel:
            continue
        _, entries, data = fbrb_extract.read(path)
        for e in entries:
            if e['type'] in SHADER_TYPES:
                yield rel, e['name'], data[e['offset']:e['offset'] + e['size']]


def best_match(ucode, sources):
    """The source block closest to ucode: (source, offset, differing dword indexes). Vertex shaders are found by
    their first 16 bytes, which the game's patching leaves alone."""
    n = len(ucode) // 4
    best = None
    for rel, name, blob in sources:
        k = blob.find(ucode[:16])
        while k != -1 and k + len(ucode) <= len(blob):
            cand = blob[k:k + len(ucode)]
            diff = [i for i in range(n) if cand[i * 4:i * 4 + 4] != ucode[i * 4:i * 4 + 4]]
            if best is None or len(diff) < len(best[3]):
                best = (rel, name, k, diff)
                if not diff:
                    return best
            k = blob.find(ucode[:16], k + 1)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('storage', nargs='*', help='folders holding recorded .xsh, .xpso and .pipelines files')
    ap.add_argument('--scan', action='append', default=[], help='also every storage folder under this folder')
    ap.add_argument('--game', required=True, help="the game folder (default.xex and dist)")
    ap.add_argument('--image', default='input/image.bin', help='the decrypted default.xex image (base 0x82000000)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--unfound', action='store_true', help='write unfound.txt in --out: shaders not located')
    ap.add_argument('--max-patches', type=int, default=32, help='leave out shaders that need more patched dwords')
    a = ap.parse_args()

    for root in a.scan:
        for base, _, files in os.walk(root):
            if any(f.endswith(('.xsh', '.pipelines')) for f in files):
                a.storage.append(base)
    shaders, pipelines, xsh_head = {}, {}, None
    nxr_head, nxr_trans, nxr_pipes = None, {}, {}
    for folder in a.storage:
        for p in glob.glob(os.path.join(folder, '*.pipelines')):
            r = read_nxr(p)
            if not r:
                continue
            if nxr_head and r[0] != nxr_head:
                print('skipped %s: another storage version' % p)
                continue
            nxr_head = r[0]
            for (t, h), u in r[1].items():
                if xxhash.xxh3_64_intdigest(u) == h:
                    shaders[h] = (t, u)
            for x in r[2]:
                nxr_trans[x] = True
            for x in r[3]:
                nxr_pipes[x] = True
        for p in glob.glob(os.path.join(folder, '*.xsh')):
            xsh_head, recs = read_xsh(p)
            for h, t, u in recs:
                shaders[h] = (t, u)
        for p in glob.glob(os.path.join(folder, '*.xpso')):
            head, recs = read_xpso(p)
            kind = os.path.basename(p)
            entry = pipelines.setdefault(kind, [head, {}])
            for r in recs:
                entry[1][r[:8]] = r
    sources = list(game_sources(a.game))
    image = open(a.image, 'rb').read() if os.path.exists(a.image) else b''

    lines, missing = [], 0
    unfound = []
    for h, (t, u) in sorted(shaders.items()):
        n = len(u) // 4
        k = image.find(u) if image else -1
        if k >= 0 and k % 4 == 0:
            lines.append('%016x %s %d xex %08x' % (h, 'p' if t else 'v', n, 0x82000000 + k))
            continue
        m = best_match(u, sources)
        if not m or len(m[3]) > a.max_patches:
            missing += 1
            unfound.append((h, t, len(u) // 4, m and len(m[3])))
            continue
        rel, name, off, diff = m
        blob = next(b for r, e, b in sources if r == rel and e == name)
        patches = ['%d:%08x' % (i, struct.unpack_from('<I', blob, off + i * 4)[0] ^ struct.unpack_from('<I', u, i * 4)[0])
                   for i in diff]
        lines.append(' '.join(['%016x %s %d fbrb %s %s %d' % (h, 'p' if t else 'v', n, rel, name.replace(' ', '%20'),
                                                                 off)] + patches))
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'bf1943-shaders.list'), 'w', newline='\n') as f:
        f.write('# BF1943 shader list v1: hash type dwords source [dword:xor ...]; header %s\n' % xsh_head.hex())
        f.write('\n'.join(lines) + '\n')
    if nxr_head:
        # Only the translations and pipelines whose shaders are all listed (the others could not be made).
        listed = set(int(l.split()[0], 16) for l in lines)
        trans = [x for x in nxr_trans if struct.unpack_from('<Q', x, 2)[0] in listed]
        pipes = []
        for x in nxr_pipes:
            vh, _, ph, _ = struct.unpack_from('<QQQQ', x, 1)
            # A pixel hash of all ones is an object motion pipeline (NXR D137: its pixel shader is the renderer's).
            if vh in listed and (ph == 0 or ph == 0xFFFFFFFFFFFFFFFF or ph in listed):
                pipes.append(x)
        with open(os.path.join(a.out, '5841097E.nxr'), 'wb') as f:
            f.write(nxr_head + b''.join(sorted(trans)) + b''.join(sorted(pipes)))
        print('NativeXRenderer: %d of %d translations, %d of %d pipelines' % (len(trans), len(nxr_trans), len(pipes),
                                                                             len(nxr_pipes)))
    for kind, (head, recs) in pipelines.items():
        with open(os.path.join(a.out, kind), 'wb') as f:
            f.write(head + b''.join(recs[k] for k in sorted(recs)))
    if unfound:
        with open(os.path.join(a.out, 'unfound.txt') if a.unfound else os.devnull, 'w') as f:
            for x in unfound:
                f.write('%016x %s %d %s\n' % (x[0], 'p' if x[1] else 'v', x[2], x[3]))
    print('%d shaders listed, %d not found in the game files; pipelines: %s' %
          (len(lines), missing, ', '.join('%s %d' % (k, len(v[1])) for k, v in pipelines.items())))


if __name__ == '__main__':
    main()
