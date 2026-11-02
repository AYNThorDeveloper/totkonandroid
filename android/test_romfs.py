#!/usr/bin/env python3
"""Host regressions of the Android backend. No device or game files required."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def digest(data):
    return hashlib.sha256(data).hexdigest()

def index(rows):
    names = bytearray()
    records = bytearray()
    for path, offset, size in rows:
        name = path.encode() + b'\0'
        records += struct.pack('<QQII', offset, size, len(names), len(name))
        names += name
    return struct.pack('<8sQQ', b'TOTKRFS1', len(rows), 24 + len(records)) + records + names


def main():
    image = b'helloBOUNDARY'
    rows = [('Sound/test', 0, 5), ('empty', len(image), 0), ('tail', 5, 8)]
    good = index(rows)
    checks = 0
    with tempfile.TemporaryDirectory(prefix='totk-romfs-') as tmp:
        folder = Path(tmp)
        env = dict(os.environ, TOTK_ROMFS=tmp)
        exe = folder / 'romfs_test'
        config = folder / 'romfs_config.h'

        def build(data, configured=True):
            if configured:
                config.write_text(f'#define ROMFS_IMAGE_SIZE {len(image)}ULL\n'
                                  f'#define ROMFS_IMAGE_SHA256 "{digest(image)}"\n'
                                  f'#define ROMFS_INDEX_SHA256 "{digest(data)}"\n')
            elif config.exists():
                config.unlink()
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-g', '-fsanitize=' + os.environ.get('TOTK_TEST_SANITIZERS', 'address,undefined'), '-fno-omit-frame-pointer', '-pthread',
                            '-I' + tmp, '-I' + str(ROOT / 'games/totk/runtime'),
                            str(ROOT / 'android/tests/romfs_test.c'), '-o', str(exe)], check=True)

        def run(data=good, img=image, expect=None, mode='mount-only', run_env=env, write=True):
            nonlocal checks
            (folder / 'runtime-index.bin').write_bytes(data)
            if write:
                (folder / 'romfs.bin').write_bytes(img)
            result = subprocess.run([str(exe), mode], env=run_env, capture_output=True, text=True)
            if expect:
                assert result.returncode == 90 and expect in result.stderr, result
            else:
                assert result.returncode == 0, result
            checks += 1
            return result.stderr

        build(good, configured=False)
        run(mode='init-only')
        run(expect='metadata missing')
        build(good)
        run(mode='all')
        run(mode='all', run_env=dict(env, TOTK_ROMFS_PARALLEL='0'))
        run(good[:-1], expect='index SHA-256 mismatch')
        run(img=b'j' + image[1:], expect='SHA-256 mismatch')
        run(img=image[:-1], expect='wrong size')
        # The verified-image cache skips a rehash only for the identical file.
        with tempfile.TemporaryDirectory(prefix='totk-romfs-cache-') as cache_dir:
            stamp = Path(cache_dir) / 'romfs-verified'
            cached = dict(env, TOTK_ROMFS_VERIFIED_CACHE=str(stamp))
            assert 'verifying game image' in run(run_env=cached)
            assert stamp.read_text().endswith(digest(image) + '\n')
            assert 'skipping rehash' in run(run_env=cached, write=False)
            os.utime(folder / 'romfs.bin', ns=(1, 1))
            assert 'verifying game image' in run(run_env=cached, write=False)
            run(img=b'j' + image[1:], expect='SHA-256 mismatch', run_env=cached)
            stamp.write_text(stamp.read_text().replace(digest(image), '0' * 64))
            assert 'verifying game image' in run(run_env=cached)
        # These malformed indexes have matching build hashes: structural checks
        # must reject them independently of the integrity check.
        cases = [
            (b'BADMAGIC' + good[8:], 'index magic'),
            (good[:8] + struct.pack('<QQ', 2**64 - 1, 24) + good[24:], 'index layout'),
            (index([('escape', len(image), 1)]), 'file bounds'),
            (index([('escape', 0, 2**64 - 1)]), 'file bounds'),
            (index([('../escape', 0, 1)]), 'invalid or unsorted path'),
            (index([('a//b', 0, 1)]), 'invalid or unsorted path'),
            (index([('a/', 0, 1)]), 'invalid or unsorted path'),
            (index([('z', 0, 1), ('a', 0, 1)]), 'invalid or unsorted path'),
            (index([('a', 0, 1), ('a', 0, 1)]), 'invalid or unsorted path'),
            (good[:40] + struct.pack('<II', 0xffffffff, 2) + good[48:], 'name bounds'),
            (good[:-1] + b'x', 'invalid or unsorted path'),
        ]
        for data, error in cases:
            build(data)
            run(data, expect=error)
    print(f'PASS: {checks} scenarios, including concurrent reads, close during read and 400 read/open/close cycles per functional run; sanitizers: {os.environ.get("TOTK_TEST_SANITIZERS", "address,undefined")}')

if __name__ == '__main__':
    main()
