#!/usr/bin/env python3
"""Generate a neutral PNG icon using only the Python standard library."""
import struct,zlib,sys
from pathlib import Path
p=Path(sys.argv[1]);p.parent.mkdir(parents=True,exist_ok=True)
def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
w=192
raw=b''.join(b'\0'+b''.join(bytes((35,190,160,255) if 40<x<152 and (40<y<64 or 84<y<108 or 128<y<152) else (16,30,40,255)) for x in range(w)) for y in range(w))
p.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,w,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
