#!/usr/bin/env python3
"""Translate every shader of the game ahead of play, so the Thor needs no Mac.

Finds every shader in the verified RomFS and the game module (prepare_shader_seed.inventory, as the macOS build),
then translates the default bind state of every shader (the macOS seed: startup shaders only) and every bind state
the shared list (games/totk/assets/shader_list.tsv) names for a shader of this game. Replies already in <out>/cache
from the same translator are reused. The translation is the bridge's own
(shader_bridge.handle: same translator, FP64 lowering, interlock removal, set rewrite, vertex defaults), so a seeded
variant is byte-identical to what the bridge would have answered.

Output (game-derived data: never in the repo):
  <out>/cache/<variant>.vks   bridge replies, named as the device names variants (key[:32]-sha256(answers)[:12])
  <out>/cache/seed.idx        one line per variant: key, file, stage and the answers of the slots it uses
  <out>/cache/<variant>.fetch framebuffer-fetch proofs (fetch_proof.py): fragment "S <sample id> <slot> <location>",
                              vertex "V <location>" (an output that is the screen position of the clip position)
  <out>/seed-manifest.json    counts, sources and tool hashes

seed.idx line: <key32> <variant> <stage 0|1|5> then tokens t<reg>:<id> (texture / texel buffer slot),
i<reg> (image slot translated as unbound), a<loc>:<cls> (vertex input), o<loc>:<cls> (colour output).
Texture ids: 1d 1, 2d 2, 3d 3, 1darray 4, 2darray 5, 2dms 6, 2dmsarray 7, cube 8, cubearray 9, buffer 10,
+32 for a depth texture. Classes: float 1, uint 2, sint 3. A slot the answers leave out is translated with the
translator's default (2d, float), so that is what the line records.

usage: android_shader_seed.py --out DIR [--limit N]
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO/'games/totk/scripts'))
sys.path.insert(0, str(HERE))
import prepare_shader_seed as seed   # noqa: E402
import shader_bridge as bridge       # noqa: E402
import fetch_proof                   # noqa: E402
from shader_list import ShaderList   # noqa: E402

TEX_IDS = {'1d': 1, '2d': 2, '3d': 3, '1darray': 4, '2darray': 5, '2dms': 6, '2dmsarray': 7, 'cube': 8,
           'cubearray': 9, 'buffer': 10}
CLS_IDS = {'float': 1, 'uint': 2, 'sint': 3}
STAGES = {'vs': 0, 'ps': 1, 'cs': 5}
DEPTH_MINUS_W = True   # TOTK sets NVN_DEPTH_MODE_NEAR_IS_MINUS_W ("深度模式 近=-w" in every run log)


def device_answers(answers, local):
    """answers() of nvn_vk_shader.c as text: slots in numeric order, the same spacing."""
    def members(d, fmt):
        return ', '.join('"%s": %s' % (k, fmt(d[k])) for k in sorted(d, key=int))
    def img(v):
        return ('{"fmt": "%s", "type": "%s"}' % (v['fmt'], v['type']) if 'fmt' in v else '{"type": "%s"}' % v['type'])
    text = ('"attr": {%s}, "depth_minus_w": %s, "fout": {%s}, "img": {%s}, "tex": {%s}' % (
        members(answers['attr'], '"%s"'.__mod__), 'true' if DEPTH_MINUS_W else 'false',
        members(answers['fout'], '"%s"'.__mod__), members(answers['img'], img), members(answers['tex'], '"%s"'.__mod__)))
    if local:
        text += ', "local": [%d, %d, %d]' % tuple(local)
    return text


def tex_id(name):
    depth = name.endswith('_depth')
    return TEX_IDS[name[:-6] if depth else name] + (32 if depth else 0)


def index_line(key, variant, stage, answers, reply):
    """The seed.idx line of a reply: every slot the translation uses, with the answer it was translated for."""
    lines, _ = bridge.split_reply(reply)
    tokens = []
    for line in lines[1:]:
        f = line.split()
        if f[0] == 'B':
            kind, reg = int(f[3]), int(f[4])
            if kind in (bridge.K_TEX, bridge.K_TBUF):
                tokens.append('t%d:%d' % (reg, tex_id(answers['tex'].get(str(reg), '2d'))))
            elif kind == bridge.K_IMG:
                if answers['img'].get(str(reg)):
                    raise ValueError('seeded image answers are not matched on the device')
                tokens.append('i%d' % reg)
        elif f[0] == 'I' and stage == 'vs':
            tokens.append('a%d:%d' % (int(f[1]), CLS_IDS[answers['attr'].get(f[1], 'float')]))
        elif f[0] == 'O' and stage == 'ps' and int(f[1]) < 8:
            tokens.append('o%d:%d' % (int(f[1]), CLS_IDS[answers['fout'].get(f[1], 'float')]))
    return ' '.join([key[:32], variant, str(STAGES[stage])] + sorted(set(tokens)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--version', default='v143')
    ap.add_argument('--limit', type=int, default=0, help='translate only the first N jobs (trial)')
    ap.add_argument('--startup-defaults-only', dest='all_defaults', action='store_false',
                    help='default bind state only for the startup shaders (the macOS seed); default: every shader')
    a = ap.parse_args()
    root = REPO/'games/totk'
    romfs, modules = root/'extracted'/a.version/'romfs', root/'rewritten'/('android-' + a.version)
    started = time.time()
    _, shaders, startup, _ = seed.inventory(romfs, modules)
    shared = ShaderList.load(seed.LIST)
    if shared.locators:
        found = seed.located(shared.locators, seed.source_reader(romfs, modules))
        shaders.update({k: v for k, v in found.items() if k not in shaders})
    # Every shader gets its default bind state, not only the startup ones as on the Mac (whose iPhone app carries
    # the seed): effect and layout shaders are met while playing, and the Thor has room for them.
    jobs = seed.plan(shaders, set(shaders) if a.all_defaults else startup, shared.variants)
    if a.limit:
        jobs = jobs[:a.limit]
    cache = a.out/'cache'
    cache.mkdir(parents=True, exist_ok=True)
    xlate_sha = hashlib.sha256(Path(bridge.XLATE).read_bytes()).hexdigest()
    old_manifest = a.out/'seed-manifest.json'
    manifest_old = json.loads(old_manifest.read_text()) if old_manifest.exists() else {}
    if manifest_old.get('reply_version') != bridge.REPLY_VERSION:
        manifest_old = {}
    work = tempfile.mkdtemp(prefix='totk-seed-')
    tr = bridge.Translator(work)
    index, seen, written, failed = [], {}, 0, {}
    defaults = sum(1 for j in jobs if j[3] == 'default')
    proofs = 0
    for n, (key, answers, _, why) in enumerate(jobs, 1):
        stage, control, code, _ = shaders[key]
        c1o, length = struct.unpack_from('<II', control, 0x700)
        local = list(struct.unpack_from('<3I', control, 0x748)) if stage == 'cs' else None
        text = device_answers(answers, local)
        variant = key[:32] + '-' + hashlib.sha256(text.encode()).hexdigest()[:12]
        if variant in seen:
            continue
        skip = 0 if stage == 'cs' else 0x30   # graphics code drops the 0x30-byte NVN header (nvn_vk_shader.c)
        rq = dict(variant=variant, stage=stage, code_hex=code[skip:length].hex(), c1=code[c1o:length].hex(),
                  depth_minus_w=DEPTH_MINUS_W, tex=answers['tex'], img=answers['img'], attr=answers['attr'],
                  fout=answers['fout'])
        if local:
            rq['local'] = local
        path = cache/(variant + '.vks')
        try:
            # A reply of this translation chain already in the cache is reused (rerun after a list update).
            old = path.read_bytes() if path.exists() and manifest_old.get('translator_sha256') == xlate_sha else None
            reply = old if old and old.startswith(b'OK ') else bridge.handle(tr, rq)
        except Exception as e:   # noqa: BLE001 (reported per message, the device translates or skips these)
            msg = ('%s: %s' % (type(e).__name__, e))[:120]
            failed[msg] = failed.get(msg, 0) + 1
            continue
        line = index_line(key, variant, stage, answers, reply)
        seen[variant] = True
        # Same shader, same answers for every slot it uses: the device would match either, one entry is enough.
        dedup = (key[:32], stage, line.split(' ', 3)[3] if line.count(' ') >= 3 else '')
        if dedup in seen:
            continue
        seen[dedup] = True
        if not path.exists() or path.read_bytes() != reply:
            tmp = path.with_suffix('.tmp')
            tmp.write_bytes(reply)
            os.replace(tmp, path)
            written += 1
        words = fetch_proof.reply_words(reply)
        proof = (['S %d %d %d' % x for x in fetch_proof.prove_ps(words)] if stage == 'ps' else
                 ['V %d' % loc for loc in sorted(fetch_proof.prove_vs(words))] if stage == 'vs' else [])
        fetch_path = cache/(variant + '.fetch')
        if proof:
            fetch_path.write_text('\n'.join(proof) + '\n')
            proofs += 1
        elif fetch_path.exists():
            fetch_path.unlink()
        index.append(line)
        if n % 2000 == 0:
            print('%d / %d jobs, %d variants, %.0f s' % (n, len(jobs), len(index), time.time() - started), flush=True)
    index.sort()
    (cache/'seed.idx.tmp').write_text('\n'.join(index) + '\n')
    os.replace(cache/'seed.idx.tmp', cache/'seed.idx')
    listed = {line.split(' ', 2)[1] + '.vks' for line in index}
    stale = [p for p in cache.glob('*.vks') if p.name not in listed]   # superseded by another entry of a rerun
    stale += [p for p in cache.glob('*.fetch') if p.name[:-len('.fetch')] + '.vks' not in listed]
    for p in stale:
        p.unlink()
    size = sum(p.stat().st_size for p in cache.glob('*.vks'))
    manifest = dict(limit=a.limit, complete_run=(a.limit == 0), version=a.version, shaders=len(shaders), default_jobs=defaults, jobs=len(jobs), variants=len(index), written=written, removed=len(stale), fetch_proofs=proofs,
                    failed=failed, bytes=size, reply_version=bridge.REPLY_VERSION,
                    translator_sha256=xlate_sha,
                    shader_list_sha256=hashlib.sha256(seed.LIST.read_bytes()).hexdigest(),
                    seconds=round(time.time() - started))
    (a.out/'seed-manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(json.dumps(manifest, indent=1))


if __name__ == '__main__':
    main()
