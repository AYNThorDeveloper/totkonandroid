#!/usr/bin/env python3
"""Android launcher icons from the game's own control data (icon_<language>.dat,
a 256x256 JPEG in the control NCA), like games/totk/scripts/extract_app_icon.py
does for macOS. Writes <out>/mipmap-*/ic_launcher.png (sips, part of macOS) and
<out>/../icon-source.jpg, from which make_adaptive_icon.py builds the adaptive icon.
usage: extract_android_icon.py <game.nsp|game.xci> --keys <prod.keys> --out <res dir>"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'games' / 'totk' / 'scripts'))
from extract_app_icon import LANGUAGES, control_nca  # noqa: E402
from extract_exefs import read_at, require  # noqa: E402
from extract_romfs import index_romfs  # noqa: E402
from extract_shared_fonts import Slice, load_keys, nca_romfs  # noqa: E402

DENSITIES = {'mdpi': 48, 'hdpi': 72, 'xhdpi': 96, 'xxhdpi': 144, 'xxxhdpi': 192}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--title', default='0100f2c0115b6000')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    keys = load_keys(a.keys)
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        image = t / 'control.romfs'
        offset, tickets = control_nca(a.game, keys, a.title)
        nca_romfs(Slice(a.game, offset), keys, image, tickets)
        entries = {e['path']: e for e in index_romfs(image)}
        icon = next((entries[f'icon_{lang}.dat'] for lang in LANGUAGES if f'icon_{lang}.dat' in entries), None)
        require(icon is not None, 'no icon in the control data')
        with image.open('rb') as f:
            (t / 'icon.jpg').write_bytes(read_at(f, icon['offset'], icon['size']))
        a.out.mkdir(parents=True, exist_ok=True)
        (a.out.parent / 'icon-source.jpg').write_bytes((t / 'icon.jpg').read_bytes())
        for density, px in DENSITIES.items():
            folder = a.out / f'mipmap-{density}'
            folder.mkdir(parents=True, exist_ok=True)
            subprocess.run(['sips', '-s', 'format', 'png', '-z', str(px), str(px), t / 'icon.jpg', '--out', folder / 'ic_launcher.png'],
                           check=True, capture_output=True)
    print(a.out)


if __name__ == '__main__':
    main()
