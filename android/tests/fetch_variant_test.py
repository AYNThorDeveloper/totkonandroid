#!/usr/bin/env python3
"""Framebuffer-fetch variants of proven fragment shaders: valid SPIR-V (spirv-val, Vulkan 1.3), the proven samples
gone and read as input attachment `att` instead, the sampled slots' binding rows marked unused (kind 15) and one
input-attachment row (kind 14) added. Attachment 8 (depth): binding 72 without InputAttachmentIndex, every read followed
by an OpVectorShuffle with the zsw channel indices. usage: fetch_variant_test.py <binary> <cache dir> [max] [att] [zsw]"""
import os
import subprocess
import sys
import tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import fetch_proof     # noqa: E402
import shader_bridge   # noqa: E402


def main():
    binary, cache = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    att = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    zsw = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x7650
    work = tempfile.mkdtemp(prefix='fetch-variant-')
    done = 0
    for name in sorted(os.listdir(cache)):
        if not name.endswith('.fetch'):
            continue
        proof = open(os.path.join(cache, name)).read().split()
        if not proof or proof[0] != 'S':
            continue
        ids = {int(proof[i + 1]) for i in range(0, len(proof), 4)}
        slots = {int(proof[i + 2]) for i in range(0, len(proof), 4)}
        reply = os.path.join(cache, name[:-6] + '.vks')
        out = os.path.join(work, 'out.vks')
        r = subprocess.run([binary, reply, os.path.join(cache, name), str(att), out, hex(zsw)])
        if r.returncode:
            sys.exit('%s: no fetch variant (%d)' % (name, r.returncode))
        data = open(out, 'rb').read()
        lines, spv = shader_bridge.split_reply(data)
        rows = [l.split() for l in lines[1:] if l.startswith('B ')]
        if [l[0] for l in lines[1:] if l][:len(rows)] != ['B'] * len(rows):
            sys.exit('%s: binding rows not together before the I/O lines (the parser reads them by position)' % name)
        if not any(r[3] == '14' and r[4] == str(att) and r[2] == str(64 + att) for r in rows):
            sys.exit('%s: no input attachment row' % name)
        if any(r[3] == '3' and int(r[4]) in slots for r in rows):
            sys.exit('%s: a fetched slot is still bound as a sampled texture' % name)
        words = fetch_proof.reply_words(data)
        i, reads, shuffles, index_decorated, binding72 = 5, 0, 0, set(), set()
        want = [zsw >> (4 * c) & 7 for c in range(4)]
        while i < len(words):
            op, n = words[i] & 0xffff, words[i] >> 16
            if op in (87, 88) and words[i + 2] in ids:
                sys.exit('%s: sample %d still there' % (name, words[i + 2]))
            if op == 71 and words[i + 2] == 43:
                index_decorated.add(words[i + 1])
            if op == 71 and words[i + 2] == 33 and words[i + 3] == 72:
                binding72.add(words[i + 1])
            if op == 79 and list(words[i + 5:i + 9]) == want:
                shuffles += 1
            reads += op == 98
            i += n
        if att == 8 and (len(binding72) != 1 or binding72 & index_decorated or shuffles < len(ids)):
            sys.exit('%s: depth read not as expected (binding 72 vars %d, decorated %d, shuffles %d for %d samples)'
                     % (name, len(binding72), len(binding72 & index_decorated), shuffles, len(ids)))
        if reads < len(ids):
            sys.exit('%s: %d input attachment reads for %d samples' % (name, reads, len(ids)))
        check = os.path.join(work, 'check.spv')
        open(check, 'wb').write(spv)
        val = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', check], capture_output=True, text=True)
        if val.returncode:
            sys.exit('%s: invalid SPIR-V: %s' % (name, (val.stderr or val.stdout)[:300]))
        done += 1
        if done >= limit:
            break
    if not done:
        sys.exit('no fragment fetch proofs in %s' % cache)
    print('framebuffer fetch variants: pass (%d shaders, attachment %d%s, spirv-val clean, samples replaced, binding rows updated)'
          % (done, att, ', channels %#06x' % zsw if att == 8 else ''))


if __name__ == '__main__':
    main()
