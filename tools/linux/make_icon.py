"""The Linux AppImage's icon: the menus' dark backdrop with "1943" in the brand's stencil and khaki, BATTLEFIELD over
it and a red REBORN stamp under it (the README banner's look, make_banner.py).

    python -I make_icon.py <out.png> [size]
"""
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

STENCIL = r'C:\Windows\Fonts\STENCIL.TTF'
TYPED = r'C:\Windows\Fonts\courbd.ttf'
BACK = (34, 36, 27)
EDGE = (20, 20, 18)
KHAKI = (217, 207, 174)
KHAKI_DIM = (169, 154, 104)
PAPER = (236, 229, 209)
STAMP = (206, 62, 48)


def centred(d, y, text, font, fill, width, track=0):
    w = sum(d.textlength(c, font=font) for c in text) + track * (len(text) - 1)
    x = (width - w) / 2
    for c in text:
        d.text((x, y), c, font=font, fill=fill)
        x += d.textlength(c, font=font) + track


def main():
    out = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    S = 1024  # drawn large, then scaled down
    img = Image.new('RGB', (S, S), BACK)
    shade = Image.new('L', (S, S), 0)
    sd = ImageDraw.Draw(shade)
    for i in range(80):
        sd.rectangle([i * 3, i * 3, S - i * 3, S - i * 3], outline=int(i * 1.2))
    shade = shade.filter(ImageFilter.GaussianBlur(40))
    img = Image.composite(img, Image.new('RGB', (S, S), EDGE), shade)
    d = ImageDraw.Draw(img)
    centred(d, 170, 'BATTLEFIELD', ImageFont.truetype(STENCIL, 118), KHAKI_DIM, S, 4)
    centred(d, 300, '1943', ImageFont.truetype(STENCIL, 360), KHAKI, S, 10)
    # The stamp: a paper card in a red frame, turned a little.
    layer = Image.new('RGBA', (760, 230), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.rectangle([10, 10, 750, 220], fill=PAPER + (240,))
    for k in range(12):
        ld.rectangle([10 + k, 10 + k, 750 - k, 220 - k], outline=STAMP + (235,))
    f = ImageFont.truetype(STENCIL, 150)
    w = sum(ld.textlength(c, font=f) for c in 'REBORN') + 8 * 5
    x = (760 - w) / 2
    for c in 'REBORN':
        ld.text((x, 30), c, font=f, fill=STAMP + (250,))
        x += ld.textlength(c, font=f) + 8
    turned = layer.rotate(5, resample=Image.BICUBIC, expand=True)
    img.paste(turned, ((S - turned.size[0]) // 2, 700), turned)
    img = img.resize((size, size), Image.LANCZOS)
    img.save(out, optimize=True)
    print(out, img.size)


if __name__ == '__main__':
    main()
