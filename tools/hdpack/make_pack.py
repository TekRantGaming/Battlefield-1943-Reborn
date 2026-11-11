"""The HD texture pack (TekRantGaming, 9 Nov 2026: an HD pack made here, handed out by him, imported in the launcher and
toggled in the settings). Never in git: it is made from the game's own textures.

  1. The game exports its world textures (a test build with BF_DEV_HD_EXPORT=<folder>: shader_prep.cpp HdExport):
     one PNG each and manifest.tsv (key, guest format, width, height, .res format, PNG, name).
  2. This script upscales them 4x with Real-ESRGAN (realesrgan-ncnn-vulkan, model realesrgan-x4plus), brings them to
     2x the original with a Lanczos filter (sharper than the model's own 2x), takes the alpha from the original (scaled
     the same way: the model's alpha is not clean), makes every mip, encodes them in the textures' own block formats
     (BC1 for the game's DXT1, BC3 for its DXT3 and DXT5) and writes the pack.

Pack (little endian): "BFHD", version 1, count, 0; per texture 56 bytes: key (u64), guest format, width, height, HD
width, HD height, mips, block format (1 or 3), 0 (u32 each), data offset, data size (u64); then each texture's mips,
the largest first, blocks row by row. NativeXRenderer (guest_textures.cpp, D149) finds a texture by its key: XXH3 of
its first 64 KB in guest memory, xor its guest format << 56, width << 32 and height << 16.

Usage: python tools/hdpack/make_pack.py <export folder> <work folder> <pack file> --esrgan <realesrgan-ncnn-vulkan.exe>
"""
import argparse
import os
import struct
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageFilter

MAX_SIDE = 4096


# ---- block encoding (range fit along the colours' principal axis)

def to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return ((c[..., 0] * 31 + 127) // 255) << 11 | ((c[..., 1] * 63 + 127) // 255) << 5 | ((c[..., 2] * 31 + 127) // 255)


def from565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31], axis=-1).astype(np.float32)


def blocks_of(img):
    """An image (h, w, c) with h and w multiples of 4 as blocks (n, 16, c), row by row."""
    h, w, c = img.shape
    b = img.reshape(h // 4, 4, w // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)
    return b


def encode_colour(px):
    """BC1 colour blocks (n, 8 bytes) for pixels (n, 16, 3), four colour mode."""
    px = px.astype(np.float32)
    mean = px.mean(axis=1, keepdims=True)
    centred = px - mean
    cov = np.einsum('nki,nkj->nij', centred, centred)
    axis = np.ones((px.shape[0], 3), np.float32)
    for _ in range(6):  # the principal axis by power iteration
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    t = np.einsum('nki,ni->nk', centred, axis)
    lo = mean[:, 0] + axis * t.min(axis=1, keepdims=True)
    hi = mean[:, 0] + axis * t.max(axis=1, keepdims=True)
    c0, c1 = to565(hi), to565(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    e0, e1 = from565(c0), from565(c1)
    pal = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], axis=1)  # (n, 4, 3)
    d = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(axis=-1)  # (n, 16, 4)
    idx = d.argmin(axis=-1).astype(np.uint32)
    same = c0 == c1
    idx[same] = 0
    bits = np.zeros(px.shape[0], np.uint32)
    for k in range(16):
        bits |= idx[:, k] << (2 * k)
    out = np.zeros((px.shape[0], 8), np.uint8)
    out[:, 0] = c0 & 255
    out[:, 1] = c0 >> 8
    out[:, 2] = c1 & 255
    out[:, 3] = c1 >> 8
    for k in range(4):
        out[:, 4 + k] = (bits >> (8 * k)) & 255
    return out


def encode_alpha(a):
    """BC3 alpha blocks (n, 8 bytes) for alphas (n, 16), eight value mode."""
    a = a.astype(np.float32)
    a0 = a.max(axis=1)
    a1 = a.min(axis=1)
    a0i, a1i = np.rint(a0).astype(np.int32), np.rint(a1).astype(np.int32)
    flat = a0i == a1i
    a0i = np.where(flat & (a0i < 255), a0i + 1, a0i)
    a1i = np.where(flat & (a0i == 255) & (a1i == 255), 254, a1i)
    vals = np.stack([a0i, a1i] + [((7 - i) * a0i + i * a1i) / 7 for i in range(1, 7)], axis=1).astype(np.float32)
    d = np.abs(a[:, :, None] - vals[:, None, :])
    idx = d.argmin(axis=-1).astype(np.uint64)
    bits = np.zeros(a.shape[0], np.uint64)
    for k in range(16):
        bits |= idx[:, k] << np.uint64(3 * k)
    out = np.zeros((a.shape[0], 8), np.uint8)
    out[:, 0] = a0i
    out[:, 1] = a1i
    for k in range(6):
        out[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return out


def encode(img, bc):
    """An RGBA image (h, w, 4) as BC1 or BC3 blocks (bytes)."""
    h, w, _ = img.shape
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (ph, pw) != (h, w):
        img = np.pad(img, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    b = blocks_of(img)
    colour = encode_colour(b[:, :, :3])
    if bc == 1:
        return colour.tobytes()
    alpha = encode_alpha(b[:, :, 3])
    return np.concatenate([alpha, colour], axis=1).tobytes()


def mips_of(img):
    """Every level of an RGBA image down to 1 x 1 (each half the last, a box filter with alpha weighted colour)."""
    out = [img]
    cur = img.astype(np.float32)
    while cur.shape[0] > 1 or cur.shape[1] > 1:
        h, w = cur.shape[:2]
        nh, nw = max(1, h // 2), max(1, w // 2)
        if h % 2 or w % 2:
            cur = cur[: nh * 2 if h > 1 else 1, : nw * 2 if w > 1 else 1]
        a = cur.reshape(nh, cur.shape[0] // nh, nw, cur.shape[1] // nw, 4)
        alpha = a[..., 3:4]
        wsum = alpha.sum(axis=(1, 3))
        rgb = (a[..., :3] * alpha).sum(axis=(1, 3)) / np.maximum(wsum, 1e-3)
        plain = a[..., :3].mean(axis=(1, 3))
        rgb = np.where(wsum > 1e-3, rgb, plain)
        cur = np.concatenate([rgb, alpha.mean(axis=(1, 3))], axis=-1)
        out.append(np.clip(np.rint(cur), 0, 255).astype(np.uint8))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('export')
    ap.add_argument('work')
    ap.add_argument('pack')
    ap.add_argument('--esrgan', required=True)
    ap.add_argument('--model', default='realesrgan-x4plus')
    ap.add_argument('--limit', type=int, default=0, help='only the first N textures (a test)')
    args = ap.parse_args()

    rows = []
    with open(os.path.join(args.export, 'manifest.tsv'), encoding='utf-8') as f:
        next(f)
        for line in f:
            key, guest_format, w, h, res_format, png, name = line.rstrip('\n').split('\t')
            rows.append((int(key, 16), int(guest_format), int(w), int(h), int(res_format), png, name))
    if args.limit:
        rows = rows[: args.limit]
    up = os.path.join(args.work, 'x4')
    os.makedirs(up, exist_ok=True)

    # 1. Real-ESRGAN, 4x, on the textures not yet upscaled (a folder at a time; RGB: the alpha comes from the original).
    todo = [r for r in rows if not os.path.exists(os.path.join(up, r[5]))]
    if todo:
        src = os.path.join(args.work, 'rgb')
        os.makedirs(src, exist_ok=True)
        for r in todo:
            Image.open(os.path.join(args.export, r[5])).convert('RGB').save(os.path.join(src, r[5]))
        t0 = time.time()
        subprocess.run([args.esrgan, '-i', src, '-o', up, '-n', args.model, '-s', '4', '-f', 'png'], check=True,
                       cwd=os.path.dirname(os.path.abspath(args.esrgan)))
        print('upscaled %d textures in %.0f s' % (len(todo), time.time() - t0), flush=True)

    # 2. To 2x, the original alpha, mips, blocks; the pack.
    entries, blobs, offset = [], [], 0
    t0 = time.time()
    for i, (key, guest_format, w, h, res_format, png, name) in enumerate(rows):
        src = Image.open(os.path.join(args.export, png)).convert('RGBA')
        hw, hh = w * 2, h * 2
        while max(hw, hh) > MAX_SIDE:
            hw, hh = hw // 2, hh // 2
        try:
            rgb = Image.open(os.path.join(up, png)).convert('RGB').resize((hw, hh), Image.LANCZOS)
        except FileNotFoundError:
            print('no upscaled picture for', name, flush=True)
            continue
        alpha = src.getchannel('A').resize((hw, hh), Image.LANCZOS)
        # The model's detail, the original's colour: Real-ESRGAN shifts colours (a warm gravel came out grey), so the
        # broad tones (a blur of 3 pixels at the new size) are taken from the original scaled up, the fine detail
        # from the model.
        base = src.convert('RGB').resize((hw, hh), Image.BICUBIC)
        blur = lambda im: np.asarray(im.filter(ImageFilter.GaussianBlur(3)), np.float32)
        fixed = np.asarray(rgb, np.float32) + blur(base) - blur(rgb)
        img = np.dstack([np.clip(np.rint(fixed), 0, 255).astype(np.uint8), np.asarray(alpha)])
        bc = 1 if guest_format == 18 else 3
        levels = mips_of(img)
        data = b''.join(encode(lv, bc) for lv in levels)
        entries.append((key, guest_format, w, h, hw, hh, len(levels), bc, offset, len(data)))
        blobs.append(data)
        offset += len(data)
        if i % 25 == 0:
            print('%d of %d (%s, %dx%d to %dx%d) %.0f s' % (i + 1, len(rows), name, w, h, hw, hh, time.time() - t0),
                  flush=True)
    header = struct.pack('<4sIII', b'BFHD', 1, len(entries), 0)
    index_size = len(header) + 56 * len(entries)
    with open(args.pack + '.tmp', 'wb') as f:
        f.write(header)
        for e in entries:
            f.write(struct.pack('<QIIIIIIIIQQ', e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], 0, index_size + e[8],
                                e[9]))
        for b in blobs:
            f.write(b)
    os.replace(args.pack + '.tmp', args.pack)
    print('pack: %d textures, %.1f MB' % (len(entries), os.path.getsize(args.pack) / 1e6), flush=True)


if __name__ == '__main__':
    sys.exit(main())
