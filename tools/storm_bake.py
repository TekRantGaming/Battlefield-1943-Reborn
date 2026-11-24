"""Bake a storm's radar into the game's storm pattern (D228; the fluid weather plan's step 2).

The user, 20 Nov 2026: the hurricane should "look and spin like a real hurricane. Use real recorded weather radar data
to base storms from." Input: NWS NEXRAD composite reflectivity frames from the Iowa Environmental Mesonet's archive
(the US composite, `n0q_YYYYMMDDHHMM.png`: 0.005 degrees a pixel from 126 W, 50 N; index i is -32 + 0.5 i dBZ, 0
none). Public domain US government data. The frames are not kept in git; the baked pattern is.

For each frame:
  1. the reflectivity in a box round the storm's expected place;
  2. the eye: the place in a search window where a clear disc stands inside the brightest ring (smoothed field: the
     ring's mean, 2 to 3.5 eye radii out, less the disc's);
  3. the frame re-centred on its eye, out to `--radius-km`, sampled to `--size` square, north up;
  4. what the radars did not reach (no echo joined to the disc's rim through weak or no echo: the composite shows the
     sectors past their reach as no echo) filled from the storm turned about its eye (half a turn, else a quarter), so
     its bands carry on round it instead of stopping at the radars' edge;
  5. the intensity 0 to 1 from 15 to 55 dBZ (light rain to the eyewall's heaviest), as a byte.

Output: a little header and the frames one after another,
  "BFSTORM1", width (u32), height (u32), frames (u32), the eye's radius in pixels (f32, the frames' mean), kilometres a
  pixel (f32), minutes between frames (f32), then frames x height x width bytes (row 0 the north).

  python tools/storm_bake.py <frames folder> bf/data/weather/storms/michael_2018.bin --centre 29.9 -85.4
"""
import argparse
import glob
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

LON0, LAT0, DEG = -126.0, 50.0, 0.005


def box_blur(a, r):
    """A box blur of radius r (twice: close to a Gaussian), edges held."""
    for _ in range(2):
        p = np.pad(a, r, mode='edge')
        c = np.cumsum(np.cumsum(p, 0), 1)
        c = np.pad(c, ((1, 0), (1, 0)))
        n = 2 * r + 1
        a = (c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]) / (n * n)
    return a


def read_dbz(path, lat, lon, half):
    """The frame's dBZ (no echo -40) in a box of `half` degrees round (lat, lon), and that box's top left corner."""
    x0 = int(round((lon - half - LON0) / DEG))
    y0 = int(round((LAT0 - (lat + half)) / DEG))
    size = int(round(2 * half / DEG))
    im = Image.open(path)
    a = np.array(im.crop((x0, y0, x0 + size, y0 + size)), dtype=np.float32)
    dbz = np.where(a > 0, -32.0 + 0.5 * a, -40.0)
    return dbz, x0, y0


def find_eye(dbz, guess, window, eye_px):
    """The eye near `guess` (row, col): the clear disc inside the brightest ring."""
    v = np.clip((dbz - 15.0) / 40.0, 0.0, 1.0)
    s = box_blur(v, max(2, eye_px // 4))
    h, w = s.shape
    yy, xx = np.mgrid[-int(eye_px * 3.5):int(eye_px * 3.5) + 1, -int(eye_px * 3.5):int(eye_px * 3.5) + 1]
    rr = np.sqrt(yy * yy + xx * xx)
    disc = rr <= eye_px * 0.8
    ring = (rr >= eye_px * 2.0) & (rr <= eye_px * 3.5)
    best, at = -1e9, guess
    m = int(eye_px * 3.5)
    for cy in range(max(m, guess[0] - window), min(h - m, guess[0] + window + 1), 2):
        for cx in range(max(m, guess[1] - window), min(w - m, guess[1] + window + 1), 2):
            patch = s[cy - m:cy + m + 1, cx - m:cx + m + 1]
            score = patch[ring].mean() - patch[disc].mean()
            if score > best:
                best, at = score, (cy, cx)
    return at, best


def recentre(dbz, eye, radius_px, size):
    """The frame round its eye, `size` square over a disc of `radius_px`, by bilinear sampling (no echo outside)."""
    out = np.full((size, size), -40.0, dtype=np.float32)
    g = (np.arange(size) + 0.5) / size * 2.0 - 1.0
    gy, gx = np.meshgrid(g, g, indexing='ij')
    sy = eye[0] + gy * radius_px
    sx = eye[1] + gx * radius_px
    y0 = np.floor(sy).astype(int)
    x0 = np.floor(sx).astype(int)
    fy, fx = sy - y0, sx - x0
    h, w = dbz.shape
    ok = (y0 >= 0) & (x0 >= 0) & (y0 + 1 < h) & (x0 + 1 < w)
    y0c, x0c = np.clip(y0, 0, h - 2), np.clip(x0, 0, w - 2)
    a = dbz[y0c, x0c] * (1 - fx) * (1 - fy) + dbz[y0c, x0c + 1] * fx * (1 - fy) + \
        dbz[y0c + 1, x0c] * (1 - fx) * fy + dbz[y0c + 1, x0c + 1] * fx * fy
    out[ok] = a[ok]
    return out, ok


def rim_gaps(img, inside):
    """No echo joined to the disc's rim through no echo: the sectors past the radars' reach (the composite shows them
    as no echo, as clear air). Clear air enclosed by echo (the eye, the moats between bands) is kept."""
    size = img.shape[0]
    g = (np.arange(size) + 0.5) - size / 2.0
    gy, gx = np.meshgrid(g, g, indexing='ij')
    r = np.sqrt(gx * gx + gy * gy) / (size / 2.0)
    none = (img < 10.0) & inside  # (weak echoes too: the composite's faint arcs past the radars broke it up)
    reach = none & (r >= 0.97)
    while True:
        grown = reach.copy()
        grown[1:, :] |= reach[:-1, :]
        grown[:-1, :] |= reach[1:, :]
        grown[:, 1:] |= reach[:, :-1]
        grown[:, :-1] |= reach[:, 1:]
        grown &= none
        if (grown == reach).all():
            break
        reach = grown
    # Widened by 4 pixels: the gap's own weak border otherwise stayed as a dark seam beside the fill (run michael_bake3).
    for _ in range(4):
        grown = reach.copy()
        grown[1:, :] |= reach[:-1, :]
        grown[:-1, :] |= reach[1:, :]
        grown[:, 1:] |= reach[:, :-1]
        grown[:, :-1] |= reach[:, 1:]
        reach = grown & inside
    return reach


def fill_sectors(img, covered):
    """Pixels without data: the storm turned about its eye, half a turn first (its two main bands lie on each other
    that way), else a quarter turn either way, from the first turned place that has data. (The nearest bearing with
    data was the gap's own dark edge: the sectors stayed black, run michael_bake2.)"""
    size = img.shape[0]
    out = img.copy()
    miss = ~covered
    c = (size - 1) / 2.0
    yy, xx = np.nonzero(miss)
    dy, dx = yy - c, xx - c
    left = np.ones(len(yy), dtype=bool)
    for ry, rx in ((-dy, -dx), (dx, -dy), (-dx, dy)):
        ty = np.clip(np.round(c + ry).astype(int), 0, size - 1)
        tx = np.clip(np.round(c + rx).astype(int), 0, size - 1)
        ok = left & covered[ty, tx]
        out[yy[ok], xx[ok]] = img[ty[ok], tx[ok]]
        left &= ~ok
    out[yy[left], xx[left]] = -40.0
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('frames', help='folder of n0q_*.png frames, in time order by name')
    ap.add_argument('out', help='the baked pattern (.bin)')
    ap.add_argument('--centre', type=float, nargs=2, required=True, metavar=('LAT', 'LON'), help='the storm near here')
    ap.add_argument('--radius-km', type=float, default=150.0)
    ap.add_argument('--size', type=int, default=256)
    ap.add_argument('--eye-km', type=float, default=10.0, help='the eye radius looked for')
    ap.add_argument('--minutes', type=float, default=5.0)
    ap.add_argument('--preview', help='a PNG of the frames side by side, for a look')
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.frames, 'n0q_*.png')))
    if not paths:
        sys.exit('no n0q_*.png frames in ' + args.frames)
    lat, lon = args.centre
    km_lat = 111.2
    km_lon = 111.2 * math.cos(math.radians(lat))
    km_px = DEG * (km_lat + km_lon) / 2.0  # (near enough square at these sizes: within 7% at 30 N)
    radius_px = args.radius_km / km_px
    eye_px = max(3, int(round(args.eye_km / km_px)))
    half = (args.radius_km * 2.2) / km_lat
    frames, eyes = [], []
    guess = None
    for p in paths:
        dbz, x0, y0 = read_dbz(p, lat, lon, half)
        h, w = dbz.shape
        g = guess or (h // 2, w // 2)
        eye, score = find_eye(dbz, g, 90 if guess is None else 25, eye_px)
        guess = eye
        e_lat = LAT0 - (y0 + eye[0]) * DEG
        e_lon = LON0 + (x0 + eye[1]) * DEG
        img, inside = recentre(dbz, eye, radius_px, args.size)
        covered = inside & ~rim_gaps(img, inside)
        filled = fill_sectors(img, covered)
        v = np.clip((filled - 15.0) / 40.0, 0.0, 1.0)
        frames.append((v * 255.0 + 0.5).astype(np.uint8))
        eyes.append(eye_px * args.size / (2 * radius_px))
        print('%s: eye at %.3f N %.3f E (score %.2f), %.0f%% of the disc filled from other bearings, mean %.3f' %
              (os.path.basename(p), e_lat, e_lon, score, 100.0 * ((~covered) & inside).mean() / max(inside.mean(), 1e-6),
               v.mean()))
    eye_r = float(np.mean(eyes))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(b'BFSTORM1')
        f.write(struct.pack('<IIIfff', args.size, args.size, len(frames), eye_r, 2 * args.radius_km / args.size,
                            args.minutes))
        for fr in frames:
            f.write(fr.tobytes())
    print('wrote %s: %d frames of %dx%d, the eye %.1f px across in radius, %.3f km a pixel (%d bytes)' %
          (args.out, len(frames), args.size, args.size, eye_r, 2 * args.radius_km / args.size,
           os.path.getsize(args.out)))
    if args.preview:
        cols = min(6, len(frames))
        rows = (len(frames) + cols - 1) // cols
        sheet = Image.new('L', (cols * args.size, rows * args.size))
        for k, fr in enumerate(frames):
            sheet.paste(Image.fromarray(fr), ((k % cols) * args.size, (k // cols) * args.size))
        sheet.save(args.preview)


if __name__ == '__main__':
    main()
