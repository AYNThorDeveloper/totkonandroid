#!/usr/bin/env python3
"""Reject private inputs, generated binaries and common secret markers before publication."""
from pathlib import Path
import re
import hashlib
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
FORBIDDEN={'.apk','.aab','.apks','.nsp','.nsz','.xci','.xcz','.nca','.nso','.nro','.romfs','.keys','.tik','.cert','.bfttf','.ttf','.otf','.sav','.spv','.vks','.so','.dylib','.dll','.a','.o','.elf','.bin','.keystore','.jks','.png','.jpg','.jpeg','.webp','.mp4','.mov','.zip','.gz','.7z','.tar','.pyc'}
PRIVATE={'build','bin','obj','__pycache__','Uitgepakt','Bronbestanden','Sleutels','publication-work','extracted','rewritten','user-data','shader-cache','shader-seed','prebuilt'}
SECRETS=re.compile(rb'gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----')
# The sole binary documentation exception was rendered from project HTML and visually reviewed.
REVIEWED_UI_IMAGES = {'docs/images/video-settings.png': '76fba52b2ef8c1258836385d29df2838ae7aac5e67cf7bcf756d8846165f62f6'}
def main():
    result=subprocess.run(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=ROOT,check=True,capture_output=True)
    paths=sorted(set(x for x in result.stdout.decode().split('\0') if x));errors=[]
    for rel in paths:
        p=ROOT/rel
        if p.is_symlink():errors.append(rel+': symlink');continue
        if not p.is_file():errors.append(rel+': missing file');continue
        if rel in REVIEWED_UI_IMAGES:
            if hashlib.sha256(p.read_bytes()).hexdigest()!=REVIEWED_UI_IMAGES[rel]:errors.append(rel+': UI preview changed; review and update its digest')
            continue
        if p.suffix.lower() in FORBIDDEN or set(p.relative_to(ROOT).parts)&PRIVATE:errors.append(rel+': private/generated file')
        if p.name in {'.env','local.properties','prod.keys','title.keys'} or p.name.startswith('.env.'):errors.append(rel+': local configuration')
        data=p.read_bytes()
        if b'\0' in data:errors.append(rel+': binary content')
        if SECRETS.search(data):errors.append(rel+': possible secret')
        if re.search(rb'data:image/[^;]+;base64|iVBORw0KGgo[A-Za-z0-9+/]{70}|/9j/[A-Za-z0-9+/]{100}|android_reloc_[0-9]+\[|android_symbol_[0-9]+\[',data):errors.append(rel+': embedded image or generated game table')
    if errors:
        print('\n'.join(errors),file=sys.stderr);return 1
    print(f'PASS: {len(paths)} selected files; no blocked inputs, unreviewed binaries or common secret markers.')
    print('This check does not establish copyright provenance or gameplay compatibility.')
    return 0
if __name__=='__main__':sys.exit(main())
