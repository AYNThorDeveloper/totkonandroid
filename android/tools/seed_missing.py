#!/usr/bin/env python3
"""Close the seed gaps the Thor met while playing without a Mac.

A variant neither the seed nor the device cache covers is drawn with the nearest seeded variant as a stand-in (or
skipped), and nvn_vk_shader.c notes its request in files/shaders/missing/<variant>.json: the bridge request as the
device would have sent it, plus the full shader key. This tool

  1. pulls those notes from the Thor            -> <seed>/missing/<variant>.json
  2. translates them with the bridge's chain    -> <seed>/extra/<variant>.vks (byte-identical to a bridge reply)
  3. pushes the replies into the app cache and removes the notes it answered.

The device loads <variant>.vks by its exact name before it consults seed.idx, so no index change is needed; the next
start draws the exact variant. push_shader_seed.sh also pushes <seed>/extra. <seed>/missing/list_additions.tsv holds
the bind states in the shared list's format (games/totk/assets/shader_list.tsv), for a later full seed rerun; the
list itself is not changed here.

usage: seed_missing.py [--seed DIR] [--no-pull] [--no-push]
       seed_missing.py --selftest N    translate N seeded variants afresh from notes made like the device makes them
                                       and compare them byte for byte with the seed (no Thor needed)
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import shader_bridge as bridge        # noqa: E402
from android_shader_seed import device_answers   # noqa: E402

PACKAGE = 'org.macnativegames.totk.gameprobe'
VARIANT = re.compile(r'^[0-9a-f]{32}-[0-9a-f]{12}$')


def adb(*args, **kw):
    return subprocess.run(['adb', *args], check=True, **kw)


def pull(missing):
    """The device's notes, unpacked into `missing`; the number of notes on the device."""
    missing.mkdir(parents=True, exist_ok=True)
    out = adb('exec-out', 'run-as', PACKAGE, 'sh', '-c', 'cd files/shaders/missing 2>/dev/null && tar -cf - . || true',
              capture_output=True).stdout
    if not out:
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        tar = Path(tmp)/'missing.tar'
        tar.write_bytes(out)
        names = [n for n in subprocess.run(['tar', '-tf', str(tar)], check=True, capture_output=True, text=True).stdout.split()
                 if re.fullmatch(r'(\./)?[0-9a-f]{32}-[0-9a-f]{12}\.json', n)]
        if names:
            subprocess.run(['tar', '-xf', str(tar), '-C', str(missing), *names], check=True)
    return len(names)


def check_name(rq):
    """The variant name hashes the shader key and the answers (nvn_vk_shader.c): a note that disagrees is damaged."""
    variant = rq.get('variant', '')
    if not VARIANT.match(variant) or not re.fullmatch(r'[0-9a-f]{64}', rq.get('key', '')) or rq['key'][:32] != variant[:32]:
        return False
    if not rq.get('depth_minus_w'):
        return True   # device_answers assumes TOTK's depth mode; such a note is translated unchecked
    answers = {k: rq.get(k, {}) for k in ('attr', 'fout', 'img', 'tex')}
    return hashlib.sha256(device_answers(answers, rq.get('local')).encode()).hexdigest()[:12] == variant[33:]


def list_line(rq):
    answers = {k: rq.get(k, {}) for k in ('attr', 'fout', 'img', 'tex')}
    return 'V\t%s\t%s' % (rq['key'], json.dumps(answers, sort_keys=True, separators=(',', ':')))


def translate(notes, extra, work):
    """Every note in `notes` without a reply in `extra`: (translated, failed messages, list lines)."""
    extra.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    tr, done, failed, lines = None, [], {}, []
    for path in sorted(notes):
        try:
            rq = json.loads(path.read_text())
        except (OSError, ValueError) as e:
            failed[path.name] = 'unreadable: %s' % e
            continue
        if not check_name(rq):
            failed[path.name] = 'variant name does not match key and answers'
            continue
        variant = rq['variant']
        lines.append(list_line(rq))
        out = extra/(variant + '.vks')
        if out.exists() and out.read_bytes().startswith(b'OK '):
            done.append(variant)
            continue
        tr = tr or bridge.Translator(str(work))
        body = {k: v for k, v in rq.items() if k != 'key'}
        try:
            reply = bridge.handle(tr, body)
        except Exception as e:   # noqa: BLE001 (reported per note; the device keeps its stand-in)
            failed[variant] = ('%s: %s' % (type(e).__name__, e))[:200]
            continue
        if not reply.startswith(b'OK '):
            failed[variant] = 'reply without OK header'
            continue
        tmp = out.with_suffix('.tmp')
        tmp.write_bytes(reply)
        os.replace(tmp, out)
        done.append(variant)
    return done, failed, lines


def push(extra, variants):
    if not variants:
        return
    with tempfile.TemporaryDirectory() as tmp:
        tar = Path(tmp)/'extra.tar'
        env = dict(os.environ, COPYFILE_DISABLE='1')
        subprocess.run(['tar', '-cf', str(tar), '-C', str(extra), *[v + '.vks' for v in variants]], check=True, env=env)
        adb('push', str(tar), '/data/local/tmp/totk-shader-extra.tar', capture_output=True)
    notes = ' '.join('files/shaders/missing/%s.json' % v for v in variants)
    adb('shell', 'run-as', PACKAGE, 'sh', '-c',
        "'cd files/shaders && tar -xf /data/local/tmp/totk-shader-extra.tar && cd ../.. && rm -f %s'" % notes)
    adb('shell', 'rm', '-f', '/data/local/tmp/totk-shader-extra.tar')


def selftest(seed, n):
    """Notes made from seeded default variants, as shader_slow makes them; their translation must equal the seed."""
    sys.path.insert(0, str(HERE.parents[1]/'games/totk/scripts'))
    import struct
    import prepare_shader_seed as inv   # noqa: E402
    root = HERE.parents[1]/'games/totk'
    _, shaders, _, _ = inv.inventory(root/'extracted/v143/romfs', root/'rewritten/v143')
    empty = {'attr': {}, 'fout': {}, 'img': {}, 'tex': {}}
    picked = []
    for key in sorted(shaders):
        stage, control, code, _ = shaders[key]
        local = list(struct.unpack_from('<3I', control, 0x748)) if stage == 'cs' else None
        variant = key[:32] + '-' + hashlib.sha256(device_answers(empty, local).encode()).hexdigest()[:12]
        if (seed/'cache'/(variant + '.vks')).exists():
            picked.append((key, stage, control, code, local, variant))
        if len(picked) == n:
            break
    with tempfile.TemporaryDirectory() as tmp:
        notes, extra = Path(tmp)/'missing', Path(tmp)/'extra'
        notes.mkdir()
        for key, stage, control, code, local, variant in picked:
            c1o, length = struct.unpack_from('<II', control, 0x700)
            skip = 0 if stage == 'cs' else 0x30
            rq = dict(variant=variant, stage=stage, code_hex=code[skip:length].hex(), c1=code[c1o:length].hex(),
                      depth_minus_w=True, **empty, key=key)
            if local:
                rq['local'] = local
            (notes/(variant + '.json')).write_text(json.dumps(rq) + '\n')
        done, failed, _ = translate(list(notes.glob('*.json')), extra, Path(tmp)/'work')
        same = [v for v in done if (extra/(v + '.vks')).read_bytes() == (seed/'cache'/(v + '.vks')).read_bytes()]
    print('selftest: %d notes, %d translated, %d byte-identical to the seed, %d failed' % (len(picked), len(done), len(same), len(failed)))
    for k, v in failed.items():
        print('  %s: %s' % (k, v))
    return 0 if picked and len(same) == len(picked) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seed', type=Path, default=HERE.parents[2]/'Uitgepakt/shader-seed-v143')
    ap.add_argument('--no-pull', action='store_true', help='translate the notes already in <seed>/missing')
    ap.add_argument('--no-push', action='store_true', help='leave the Thor untouched after translating')
    ap.add_argument('--selftest', type=int, default=0, metavar='N')
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.seed, a.selftest)
    missing, extra = a.seed/'missing', a.seed/'extra'
    if not a.no_pull:
        print('pulled %d notes from the Thor' % pull(missing))
    notes = list(missing.glob('*.json'))
    done, failed, lines = translate(notes, extra, a.seed/'missing-work')
    if lines:
        (missing/'list_additions.tsv').write_text('\n'.join(sorted(set(lines))) + '\n')
    print('%d notes: %d replies ready in %s, %d failed' % (len(notes), len(done), extra, len(failed)))
    for k, v in failed.items():
        print('  %s: %s' % (k, v))
    if not a.no_push:
        push(extra, done)
        print('pushed %d replies; the notes they answer are removed from the Thor' % len(done))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
