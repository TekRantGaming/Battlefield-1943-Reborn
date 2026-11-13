# The capture point letters' masks (bf/src/capture_points.cpp): A to H in Oswald Bold, drawn large, leaned a little to
# sit with the flag's slant, and reduced to the flag sprite's size. Writes bf/src/capture_letters.inc (two 16 x 14
# coverage masks a letter: the letter and its outline). With a decoded copy of the game's icon atlas
# (menu/minimap/icons0, 512 x 256; never kept in the repo) it also draws a preview on the game's own flags.
#   python -I tools/make_flag_letters.py [<decoded icons0.png> <preview.png>]
# Needs Pillow.
import os
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace(os.sep, '/')
atlas_png, preview = (sys.argv[1], sys.argv[2]) if len(sys.argv) >= 3 else (None, None)
write = True
LETTERS = 'ABCDEFGH'
W, H = 16, 14          # the mask's size in sprite texels
OX, OY = 8, 8          # where the mask sits in the 32 x 32 flag sprite
CAP = 10.4             # the capitals' height in texels
SS = 16                # drawn this many times larger, then reduced
SHEAR = 0.10           # the flag leans to the right toward its top

font = ImageFont.truetype(root + '/bf/fonts/Oswald-Bold.ttf', 400)
# the font size whose capitals are CAP * SS tall
box = font.getbbox('E')
size = int(round(400 * CAP * SS / (box[3] - box[1])))
font = ImageFont.truetype(root + '/bf/fonts/Oswald-Bold.ttf', size)


def masks(ch):
    big = Image.new('L', (W * SS, H * SS), 0)
    d = ImageDraw.Draw(big)
    b = font.getbbox(ch)
    cap = font.getbbox('E')
    x = (W * SS - (b[2] - b[0])) / 2 - b[0]
    y = (H * SS - (cap[3] - cap[1])) / 2 - cap[1]
    d.text((x, y), ch, font=font, fill=255)
    # the shear: rows nearer the top move right
    big = big.transform(big.size, Image.AFFINE, (1, SHEAR, -SHEAR * H * SS / 2, 0, 1, 0), resample=Image.BICUBIC)
    letter = big.resize((W, H), Image.LANCZOS)
    # the outline: the letter grown by about 1.1 texels, softly
    grown = big.filter(ImageFilter.MaxFilter(2 * int(1.1 * SS) + 1)).filter(ImageFilter.GaussianBlur(SS * 0.35))
    outline = grown.resize((W, H), Image.LANCZOS)
    return letter, outline


all_masks = {ch: masks(ch) for ch in LETTERS}

# ---- the preview: each letter on the three flags, at 1x and 6x
if not atlas_png:
    atlas = None
else:
    atlas = Image.open(atlas_png).convert('RGBA')
flags = {'neutral': (288, 130), 'friendly': (328, 32), 'enemy': (241, 212)}
INK = {'neutral': ((28, 28, 26), None), 'friendly': ((250, 248, 240), (8, 20, 44)), 'enemy': ((250, 248, 240), (52, 10, 6))}


def lettered(kind, ch):
    fx, fy = flags[kind]
    s = atlas.crop((fx, fy, fx + 32, fy + 32)).copy()
    letter, outline = all_masks[ch]
    ink, edge = INK[kind]
    px = s.load()
    for y in range(H):
        for x in range(W):
            sx, sy = OX + x, OY + y
            r, g, b, a = px[sx, sy]
            if a == 0:
                continue  # never outside the flag
            if edge is not None:
                o = outline.getpixel((x, y)) / 255.0 * 0.55
                r, g, b = [int(round(c * (1 - o) + e * o)) for c, e in zip((r, g, b), edge)]
            l = letter.getpixel((x, y)) / 255.0
            r, g, b = [int(round(c * (1 - l) + i * l)) for c, i in zip((r, g, b), ink)]
            px[sx, sy] = (r, g, b, a)
    return s


sheet = Image.new('RGBA', (len(LETTERS) * 200 + 10, 3 * 250 + 10), (86, 128, 164, 255))
for row, kind in enumerate(('neutral', 'friendly', 'enemy') if atlas else ()):
    for col, ch in enumerate(LETTERS):
        s = lettered(kind, ch)
        sheet.alpha_composite(s.resize((192, 192), Image.BILINEAR), (10 + col * 200, 10 + row * 250))
        sheet.alpha_composite(s, (10 + col * 200, 205 + row * 250))
        # as the minimap draws it: the 30 x 21 part around the flag at 20 x 14
        small = s.crop((1, 5, 31, 26)).resize((20, 14), Image.BILINEAR)
        sheet.alpha_composite(small, (60 + col * 200, 212 + row * 250))
        sheet.alpha_composite(small.resize((60, 42), Image.BILINEAR), (100 + col * 200, 200 + row * 250))
if atlas:
    sheet.save(preview)
    print('preview', preview)

if write:
    out = ['// The capture point letters (capture_points.cpp): A to H in Oswald Bold (bf/fonts, SIL Open Font License),',
           '// drawn large, leaned with the flag and reduced; a %d x %d coverage mask a letter and one of its outline,' % (W, H),
           '// placed at %d, %d in a 32 x 32 flag sprite. Made by tools/make_flag_letters.py: change it there.' % (OX, OY),
           'inline constexpr int kLetterW = %d, kLetterH = %d, kLetterX = %d, kLetterY = %d, kLetterCount = %d;' % (W, H, OX, OY, len(LETTERS)),
           'inline constexpr unsigned char kLetterMask[kLetterCount][2][kLetterW * kLetterH] = {']
    for ch in LETTERS:
        out.append('    {  // %s' % ch)
        for m in all_masks[ch]:
            data = list(m.getdata())
            rows = [', '.join('%3d' % v for v in data[y * W:(y + 1) * W]) for y in range(H)]
            out.append('     {' + ',\n      '.join(rows) + '},')
        out.append('    },')
    out.append('};')
    open(root + '/bf/src/capture_letters.inc', 'w', newline='\n').write('\n'.join(out) + '\n')
    print('wrote bf/src/capture_letters.inc')
