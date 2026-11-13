"""The key prompt pictures of the mouse's side buttons, MB4 and MB5 (issue 14, 13 Nov 2026).

Xelu's prompts (CC0, bf/glyphs/LICENSE.txt) have the mouse's left, middle and right buttons but no side buttons. These
are drawn from its plain mouse (bf/glyphs/keyboard/Mouse_Simple.png): the side button lit in white on the mouse's left
edge as Xelu lights a pressed button (MB4 the back one, lower; MB5 the forward one, higher), and the button's number
on the body in the menus' font (bf/fonts/Oswald-Bold.ttf, SIL Open Font Licence).

    python -I tools/make_mouse_side_glyphs.py        (writes bf/glyphs/keyboard/MB4.png and MB5.png)
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYS = os.path.join(ROOT, 'bf', 'glyphs', 'keyboard')
FONT = os.path.join(ROOT, 'bf', 'fonts', 'Oswald-Bold.ttf')


def lit_colour(lmb):
    """The white Xelu gives a pressed button: the brightest opaque colour of LMB.png."""
    best = (255, 255, 255, 255)
    top = -1
    pixels = lmb.load()
    for y in range(lmb.height):
        for x in range(lmb.width):
            px = pixels[x, y]
            if px[3] > 200 and px[0] + px[1] + px[2] > top:
                top, best = px[0] + px[1] + px[2], px
    return best


def left_edge(img, y):
    """The first opaque pixel of row y from the left (the mouse's outline), or None."""
    for x in range(img.width):
        if img.getpixel((x, y))[3] > 128:
            return x
    return None


def make(base, lit, number, y0, y1):
    img = base.copy()
    d = ImageDraw.Draw(img)
    # The side button: a capsule just inside the outline on the left, from y0 to y1.
    xs = [e for e in (left_edge(img, y) for y in range(y0, y1 + 1)) if e is not None]
    x0 = max(xs) + 2
    d.rounded_rectangle((x0, y0, x0 + 4, y1), radius=2, fill=lit)
    # The number on the body, under the buttons.
    font = ImageFont.truetype(FONT, 17)
    text = str(number)
    l, t, r, b = d.textbbox((0, 0), text, font=font)
    cx, cy = img.width / 2 + 2, 47
    d.text((cx - (r - l) / 2 - l, cy - (b - t) / 2 - t), text, font=font, fill=(230, 230, 230, 255))
    return img


def main():
    base = Image.open(os.path.join(KEYS, 'Mouse_Simple.png')).convert('RGBA')
    lit = lit_colour(Image.open(os.path.join(KEYS, 'LMB.png')).convert('RGBA'))
    h = base.height
    make(base, lit, 4, int(h * 0.50), int(h * 0.66)).save(os.path.join(KEYS, 'MB4.png'))
    make(base, lit, 5, int(h * 0.30), int(h * 0.46)).save(os.path.join(KEYS, 'MB5.png'))
    print('wrote MB4.png and MB5.png in', KEYS)


if __name__ == '__main__':
    sys.exit(main())
