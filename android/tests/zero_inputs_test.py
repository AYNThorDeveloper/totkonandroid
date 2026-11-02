#!/usr/bin/env python3
"""The device's C port of input stripping against the bridge's (shader_bridge.py): every reply with inputs in a
shader cache, for each single input and for all inputs at once, must come out byte-identical and pass spirv-val.
usage: zero_inputs_test.py <zero_inputs_test binary> <cache dir with .vks replies> [max replies]"""
import os
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import shader_bridge as sb   # noqa: E402


def expected(reply, mask):
    lines, data = sb.split_reply(reply)
    head = lines[0].split()
    spv = sb.Spirv(data)
    spv.zero_inputs(mask)
    data = struct.pack('<%dI' % len(spv.words), *spv.words)
    kept = [l for l in lines[1:] if not (l.startswith('I ') and (mask >> int(l.split()[1])) & 1)]
    head[1] = str(len(data))
    head[3] = str(sum(1 for l in kept if l.startswith('I ')))
    return ('\n'.join([' '.join(head)] + kept) + '\n').encode() + data, data


def main():
    binary, cache = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    work = tempfile.mkdtemp(prefix='zero-inputs-')
    cases = shaders = 0
    for name in sorted(os.listdir(cache)):
        if not name.endswith('.vks') or '-i' in name[33:]:
            continue
        path = os.path.join(cache, name)
        reply = open(path, 'rb').read()
        lines, _ = sb.split_reply(reply)
        locs = [int(l.split()[1]) for l in lines[1:] if l.startswith('I ')]
        if not locs:
            continue
        shaders += 1
        for mask in sorted({1 << l for l in locs} | {sum(1 << l for l in locs)}):
            want, spv = expected(reply, mask)
            out = os.path.join(work, 'out.vks')
            r = subprocess.run([binary, path, str(mask), out])
            if r.returncode:
                sys.exit('%s mask %#x: C port failed (%d)' % (name, mask, r.returncode))
            got = open(out, 'rb').read()
            if got != want:
                sys.exit('%s mask %#x: C port differs from the bridge (%d vs %d bytes)' % (name, mask, len(got), len(want)))
            check = os.path.join(work, 'check.spv')
            open(check, 'wb').write(spv)
            val = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', check], capture_output=True, text=True)
            if val.returncode:
                sys.exit('%s mask %#x: invalid SPIR-V: %s' % (name, mask, (val.stderr or val.stdout)[:200]))
            cases += 1
        if shaders >= limit:
            break
    if not cases:
        sys.exit('no fragment replies with inputs in %s' % cache)
    print('input stripping: pass (%d shaders, %d masks byte-identical to the bridge, spirv-val clean)' % (shaders, cases))


if __name__ == '__main__':
    main()
