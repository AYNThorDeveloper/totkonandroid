#!/usr/bin/env python3
"""Create a small ELF with the same two leaf instructions and GOT layout.

This tests the Android loader mechanics without using a game dump. It is not a
substitute for running the probe against the actual TOTK 1.0.0 main.elf.
"""
from pathlib import Path
import struct

out = Path(__file__).resolve().parent / "build" / "synthetic-main.elf"
out.parent.mkdir(exist_ok=True)
blob = bytearray(0x2000)

struct.pack_into("<16sHHIQQQIHHHHHH", blob, 0,
                 b"\x7fELF\x02\x01\x01" + b"\0" * 9,
                 3, 183, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
struct.pack_into("<IIQQQQQQ", blob, 64, 1, 5, 0, 0, 0, 0x1000, 0x1000, 0x1000)
struct.pack_into("<IIQQQQQQ", blob, 120, 1, 6, 0x1000, 0x4540000, 0,
                 0x1000, 0x1000, 0x1000)
struct.pack_into("<IIQQQQQQ", blob, 176, 2, 6, 0x1100, 0x4540100, 0,
                 48, 48, 8)
blob[0x3f0:0x3fc] = bytes.fromhex("002a0290002c42f9c0035fd6")
blob[0x400:0x40c] = bytes.fromhex("002a0290003042f9c0035fd6")
struct.pack_into("<qQqQqQ", blob, 0x1100, 7, 0x4540200, 8, 48, 0, 0)
struct.pack_into("<QQqQQq", blob, 0x1200,
                 0x4540458, 1027, 0x3640ca0,
                 0x4540460, 1027, 0x37dd50a)
out.write_bytes(blob)
print(out)
