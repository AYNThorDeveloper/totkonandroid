#!/usr/bin/env python3
"""Convert the generated Apple arm64 NVN assembly to ELF syntax for Android.

Apple prefixes C symbols with '_' and addresses pages with sym@PAGE /
sym@PAGEOFF. ELF uses bare names and :lo12:. Data symbols reached with adrp
are marked hidden so a shared library resolves them locally."""
import re
import sys

source, target = sys.argv[1], sys.argv[2]
text = open(source).read()
adrp_targets = sorted(set(re.findall(r'\b_(\w+)@PAGE\b', text)))
text = re.sub(r'\b_(\w+)@PAGEOFF\b', r':lo12:\1', text)
text = re.sub(r'\b_(\w+)@PAGE\b', r'\1', text)
text = re.sub(r'(?<![\w.])_([A-Za-z]\w*)', r'\1', text)
text = re.sub(r'^(\s*)\.cstring\s*$', r'\1.section .rodata.str1.1,"aMS",@progbits,1', text, flags=re.M)
hidden = ''.join(f'    .hidden {name}\n' for name in adrp_targets)
with open(target, 'w') as out:
    out.write(f'// Generated from {source.split("/")[-1]} by port_macho_asm.py; do not edit.\n{hidden}{text}')
