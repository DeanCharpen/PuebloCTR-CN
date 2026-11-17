#!/usr/bin/env python3
"""PuebloCTR's own neutral icon, banner and banner sound (nothing of the game's - no logo, no art, no music) in the
style of the bottom screen (a dark gradient with a soft vignette, blood-red hairlines, Noto Serif).

  icon.png      48x48   the HOME Menu / Homebrew Launcher icon (SMDH, also for PuebloCTR.3dsx)
  banner.png    256x128 the HOME Menu's banner (the top screen when the title is chosen)
  banner.wav    the banner's sound: 2.5 s, a low soft chord fading out (synthesised here)

The files are committed (overlay/port/whole/3ds/cia/): run this only to change them. From the repository's root (Python 3
with tools/requirements-assets.txt; the fonts, SIL OFL: tools/fetch_fonts.sh):
  python3 tools/cia_assets.py [OUT_DIR] [--preview DIR]   (default overlay/port/whole/3ds/cia/)
"""
import os
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # (the repository: tools/..)
FONTS = os.path.join(ROOT, 'build/fonts')  # (tools/fetch_fonts.sh)
ARGS = [a for i, a in enumerate(sys.argv[1:], 1) if a != '--preview' and sys.argv[i - 1] != '--preview']
OUT = ARGS[0] if ARGS else os.path.join(ROOT, 'overlay/port/whole/3ds/cia')
SHOTS = sys.argv[sys.argv.index('--preview') + 1] if '--preview' in sys.argv else None  # (enlarged, for a look)
SERIF_B = os.path.join(FONTS, 'NotoSerifJP-Black.otf')
SERIF = os.path.join(FONTS, 'NotoSerifJP-SemiBold.otf')
SERIF_R = os.path.join(FONTS, 'NotoSerifJP-Regular.otf')
BLOOD = (150, 18, 22)
BONE = (226, 218, 204)


def dark(w, h, scale=4):
    """The background, drawn at `scale` times and brought down: a vertical gradient with a vignette."""
    W, H = w * scale, h * scale
    y = np.linspace(0, 1, H)[:, None]
    x = np.linspace(0, 1, W)[None, :]
    top, bottom = np.array([30, 24, 26]), np.array([8, 7, 9])
    img = top * (1 - y[..., None]) + bottom * y[..., None]
    img = np.broadcast_to(img, (H, W, 3)).copy()
    d = np.sqrt(((x - 0.5) / 0.62) ** 2 + ((y - 0.42) / 0.75) ** 2)
    img *= np.clip(1.15 - 0.55 * d, 0.35, 1.0)[..., None]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB'), scale


def spaced(draw, xy, text, font, fill, spacing, anchor_center=True):
    widths = [draw.textlength(c, font=font) for c in text]
    total = sum(widths) + spacing * (len(text) - 1)
    x, y = xy
    if anchor_center:
        x -= total / 2
    for c, w in zip(text, widths):
        draw.text((x, y), c, font=font, fill=fill, anchor='ls')
        x += w + spacing
    return total


def icon():
    img, s = dark(48, 48)
    dr = ImageDraw.Draw(img)
    W = 48 * s
    # a thin blood-red frame inside the edge
    dr.rectangle([2 * s, 2 * s, W - 2 * s - 1, W - 2 * s - 1], outline=BLOOD, width=s)
    # the letter P, bone-white, with a red shadow
    f = ImageFont.truetype(SERIF_B, 38 * s)
    glow = Image.new('L', img.size, 0)
    ImageDraw.Draw(glow).text((W / 2 + 1 * s, 37 * s), 'P', font=f, fill=255, anchor='ms')
    glow = glow.filter(ImageFilter.GaussianBlur(2.2 * s))
    img.paste(Image.new('RGB', img.size, BLOOD), (0, 0), glow.point(lambda v: int(v * 0.85)))
    dr = ImageDraw.Draw(img)
    dr.text((W / 2, 36 * s), 'P', font=f, fill=BONE, anchor='ms')
    # a hairline under it
    dr.line([12 * s, 41 * s, W - 12 * s, 41 * s], fill=BLOOD, width=s)
    return img.resize((48, 48), Image.LANCZOS)


def banner():
    img, s = dark(256, 128)
    W, H = 256 * s, 128 * s
    f1 = ImageFont.truetype(SERIF_B, 34 * s)
    f2 = ImageFont.truetype(SERIF, 13 * s)
    glow = Image.new('L', img.size, 0)
    spaced(ImageDraw.Draw(glow), (W / 2, 68 * s), 'PUEBLO', f1, 255, 6 * s)
    glow = glow.filter(ImageFilter.GaussianBlur(4 * s))
    img.paste(Image.new('RGB', img.size, BLOOD), (0, 0), glow.point(lambda v: int(v * 0.7)))
    dr = ImageDraw.Draw(img)
    spaced(dr, (W / 2, 68 * s), 'PUEBLO', f1, BONE, 6 * s)
    dr.line([46 * s, 78 * s, W - 46 * s, 78 * s], fill=BLOOD, width=s)
    spaced(dr, (W / 2, 100 * s), 'CTR', f2, (200, 60, 60), 10 * s)
    return img.resize((256, 128), Image.LANCZOS)


def sound(path, seconds=2.5, rate=32000):
    """A low soft minor chord (A2, C3, E3 and an octave), each partial decaying, a slow swell and fade: 16-bit stereo."""
    t = np.arange(int(seconds * rate)) / rate
    out = np.zeros_like(t)
    for f, a in ((110.0, 1.0), (130.81, 0.6), (164.81, 0.5), (220.0, 0.25)):
        out += a * np.sin(2 * np.pi * f * t) * np.exp(-t * 1.1)
        out += 0.15 * a * np.sin(2 * np.pi * 2 * f * t) * np.exp(-t * 2.5)
    env = np.minimum(1, t / 0.25) * np.minimum(1, (seconds - t) / 0.8)
    out = out * env
    out = out / np.max(np.abs(out)) * 0.45
    pcm = (out * 32767).astype('<i2')
    stereo = np.stack([pcm, pcm], axis=1).tobytes()
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(stereo)


def main():
    os.makedirs(OUT, exist_ok=True)
    i, b = icon(), banner()
    i.save(os.path.join(OUT, 'icon.png'))
    b.save(os.path.join(OUT, 'banner.png'))
    sound(os.path.join(OUT, 'banner.wav'))
    if SHOTS:
        os.makedirs(SHOTS, exist_ok=True)
        i.resize((192, 192), Image.NEAREST).save(os.path.join(SHOTS, 'icon-x4.png'))
        b.resize((512, 256), Image.LANCZOS).save(os.path.join(SHOTS, 'banner-x2.png'))
    print('ok:', OUT, '(icon.png, banner.png, banner.wav)')


if __name__ == '__main__':
    main()
