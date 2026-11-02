#!/usr/bin/env python3
"""Import a user's base NSP/XCI and supported update NSP into ignored local work files."""
import argparse, importlib.util, json, shutil, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True)
    p.add_argument('--update',type=Path,required=True)
    p.add_argument('--keys',type=Path,required=True)
    p.add_argument('--hactool',type=Path,help='Existing hactool with this project’s private-title-key and bounded-base hooks; otherwise built from pinned source')
    p.add_argument('--exefs-only',action='store_true',help='Extract and verify ExeFS only; do not merge RomFS or prepare runtime')
    a=p.parse_args()
    for value in (a.base,a.update,a.keys):
        if not value.is_file():p.error('Input is not a file: '+str(value))
    za=(ROOT/'games/totk/assets/expected-za-2.0.2.json').exists()
    reference='expected-za-2.0.2.json' if za else 'expected-v1.4.3.json'
    expected=json.loads((ROOT/'games/totk/assets'/reference).read_text())
    work=ROOT/'publication-work';out=work/'input'
    if out.exists():p.error('publication-work/input already exists; use a fresh checkout to avoid overwriting private input')
    out.mkdir(parents=True)
    def run(*args):subprocess.run([str(x) for x in args],check=True,cwd=ROOT)
    scripts=ROOT/'games/totk/scripts'
    run(sys.executable,scripts/'extract_exefs.py',a.update.resolve(),'--update','--keys',a.keys.resolve(),'--output',out/'exefs')
    manifest=json.loads((out/'extraction-manifest.json').read_text())
    hashes={x['name']:x['sha256'] for x in manifest['files']}
    if hashes!=expected['exefs_sha256']:p.error('Unsupported update: expected '+('Z-A 2.0.2' if za else 'TOTK 1.4.3')+'; executable hashes do not match')
    shutil.copy2(out/'extraction-manifest.json',out/'exefs/extraction-manifest.json')
    if a.exefs_only:print('Supported ExeFS verified. Private output retained in publication-work/input.');return
    tool=a.hactool.resolve() if a.hactool else None
    if tool is None:
        spec=importlib.util.spec_from_file_location('local_build_helpers',ROOT/'build_app.py')
        helpers=importlib.util.module_from_spec(spec);spec.loader.exec_module(helpers)
        logs=work/'logs';logs.mkdir(exist_ok=True)
        tool=helpers.build_hactool(work/'game',logs)
    run(sys.executable,scripts/'extract_update_romfs.py',a.base.resolve(),a.update.resolve(),'--keys',a.keys.resolve(),'--hactool',tool,'--output',out/'romfs')
    if za:run(sys.executable,ROOT/'tools/prepare_za.py','--exefs',out/'exefs','--romfs',out/'romfs/romfs.bin')
    else:run(sys.executable,ROOT/'tools/port.py','prepare','--exefs',out/'exefs','--romfs',out/'romfs/romfs.bin')
    print('Private NSP import and runtime preparation complete. No APK built, uploaded or installed.')
if __name__=='__main__':main()
