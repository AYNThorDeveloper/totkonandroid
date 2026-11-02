#!/usr/bin/env python3
"""Adaptive launcher icon (Android 8+) from the game icon, so the launcher shows
the artwork edge to edge instead of on its white legacy-icon plate.
Layer: the icon blurred to cover the 108 dp canvas, the sharp icon on top sized
so the logo stays inside the 72 dp visible area. Needs Pillow.
usage: make_adaptive_icon.py <res dir> (icon-source.jpg beside it)"""
import sys
from pathlib import Path
from PIL import Image, ImageFilter

DENSITIES = {'mdpi': 1, 'hdpi': 1.5, 'xhdpi': 2, 'xxhdpi': 3, 'xxxhdpi': 4}


def main():
    res = Path(sys.argv[1])
    source = Image.open(res.parent / 'icon-source.jpg').convert('RGB')
    for density, scale in DENSITIES.items():
        canvas = round(108 * scale)
        art = round(74 * scale)   # about the 72 dp viewport: the whole logo stays inside round masks
        back = source.resize((canvas, canvas), Image.LANCZOS).filter(ImageFilter.GaussianBlur(radius=6 * scale))
        front = source.resize((art, art), Image.LANCZOS)
        back.paste(front, ((canvas - art) // 2, (canvas - art) // 2))
        folder = res / f'mipmap-{density}'
        folder.mkdir(parents=True, exist_ok=True)
        back.save(folder / 'ic_launcher_background.png')
        Image.new('RGBA', (canvas, canvas), (0, 0, 0, 0)).save(folder / 'ic_launcher_foreground.png')
    anydpi = res / 'mipmap-anydpi-v26'
    anydpi.mkdir(parents=True, exist_ok=True)
    (anydpi / 'ic_launcher.xml').write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@mipmap/ic_launcher_background" />\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
        '</adaptive-icon>\n')
    print(anydpi)


if __name__ == '__main__':
    main()
