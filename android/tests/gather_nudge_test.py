#!/usr/bin/env python3
"""The gather nudge of the bridge (shader_bridge.py Spirv.nudge_gathers) on every reply with an OpImageGather in a
shader cache: every 2D / 2D array gather gets its coordinate through the 1/512-texel nudge, the result passes
spirv-val (unless the reply was invalid already), and nudging again changes nothing.
usage: gather_nudge_test.py <cache dir with .vks replies> [max replies]"""
import os
import struct
import sys
import tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import shader_bridge as sb   # noqa: E402


def gathers(words):
    """(coordinate id, producer of each id) of every OpImageGather."""
    made, coords, i = {}, [], 5
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        if n > 2:
            made[words[i + 2]] = words[i:i + n]
        if op == 96:
            coords.append(words[i + 4])
        i += n
    return coords, made


def main():
    cache = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    work = tempfile.mkdtemp(prefix='gather-nudge-')
    shaders = nudged = invalid_before = 0
    for name in sorted(os.listdir(cache)):
        if not name.endswith('.vks'):
            continue
        reply = open(os.path.join(cache, name), 'rb').read()
        if not reply.startswith(b'OK '):
            continue
        _, data = sb.split_reply(reply)
        if b'\x60\x00' not in data:
            continue
        spv = sb.Spirv(data)
        if not gathers(spv.words)[0]:
            continue
        shaders += 1
        n, skipped = spv.nudge_gathers()
        out = struct.pack('<%dI' % len(spv.words), *spv.words)
        coords, made = gathers(spv.words)
        for c in coords:
            add = made[c]
            div = made.get(add[4], [0])
            assert add[0] & 0xffff == 129 and div[0] & 0xffff == 136, '%s: a gather coordinate is not nudged' % name
        again = sb.Spirv(out)
        assert again.nudge_gathers()[0] == 0, '%s: nudged twice' % name
        try:
            sb.validate(work, 'nudged', out)
        except ValueError:
            try:
                sb.validate(work, 'plain', data)
            except ValueError:
                invalid_before += 1
            else:
                raise
        nudged += n
        if limit and shaders >= limit:
            break
    print('gather nudge: %d shaders, %d gathers nudged, %d invalid before the nudge' % (shaders, nudged, invalid_before))
    assert shaders, 'no gather in %s' % cache


if __name__ == '__main__':
    main()
