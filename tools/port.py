#!/usr/bin/env python3
"""Local developer workflow. Game input/output stays ignored and on this host."""
import argparse,hashlib,importlib.util,json,os,platform,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
GAME=ROOT/'games/totk'
EXPECTED=json.loads((GAME/'assets/expected-v1.4.3.json').read_text())

def run(*args,env=None,cwd=ROOT):
    subprocess.run([str(x) for x in args],cwd=cwd,env=env,check=True)

def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(8*1024*1024),b''):h.update(b)
    return h.hexdigest()

def doctor():
    failures=[]
    def check(name,ok):
        print(('OK   ' if ok else 'NEED ')+name)
        if not ok:failures.append(name)
    check('macOS ARM64 host',platform.system()=='Darwin' and platform.machine()=='arm64')
    check('Python 3.14+ (compression.zstd)',sys.version_info>=(3,14))
    for module in ['lz4','capstone','cryptography']:
        check('Python '+module,importlib.util.find_spec(module) is not None)
    for command in ['cmake','ninja','patch','spirv-val','spirv-opt','spirv-cross']:
        check(command,shutil.which(command) is not None)
    for key,tail in [('ANDROID_HOME','build-tools/35.0.0/aapt'),('ANDROID_HOME','platforms/android-35/android.jar'),('JAVA_HOME','bin/javac'),('ANDROID_NDK_HOME','build/cmake/android.toolchain.cmake')]:
        check(key+'/'+tail,bool(os.environ.get(key)) and (Path(os.environ.get(key,''))/tail).exists())
    translator=GAME/'prebuilt/macos-arm64/totk_xlate'
    check('.NET SDK 10 available for the Android translator',shutil.which('dotnet') is not None or bool(os.environ.get('DOTNET')))
    return not failures

def prepare(args):
    exefs=args.exefs.resolve(); image=args.romfs.resolve()
    # Validate all input before creating outputs. Never read key files or NSPs here.
    files=[]
    for name,want in EXPECTED['exefs_sha256'].items():
        p=exefs/name
        if not p.is_file() or digest(p)!=want:raise ValueError('Unsupported or damaged TOTK 1.4.3 ExeFS: '+name)
        files.append({'name':name,'sha256':want})
    if image.stat().st_size!=EXPECTED['romfs']['size'] or digest(image)!=EXPECTED['romfs']['sha256']:
        raise ValueError('Unsupported or damaged TOTK 1.4.3 RomFS')
    extraction=GAME/'extracted/v143'; rewritten=GAME/'rewritten/android-v143'
    if extraction.exists() or rewritten.exists():raise ValueError('Preparation outputs already exist; use a fresh checkout/work directory')
    (extraction/'exefs').mkdir(parents=True);(extraction/'romfs').mkdir()
    for record in files:shutil.copy2(exefs/record['name'],extraction/'exefs'/record['name'])
    (extraction/'extraction-manifest.json').write_text(json.dumps({'files':files},indent=2))
    # Image is read-only throughout this workflow; symlinking avoids a second 17 GB copy.
    (extraction/'romfs/romfs.bin').symlink_to(image)
    sys.path.insert(0,str(GAME/'scripts'))
    from extract_romfs import index_romfs
    index={'status':'pass','romfs_size':image.stat().st_size,'romfs_sha256':EXPECTED['romfs']['sha256'],'files':index_romfs(image)}
    (extraction/'romfs/index.json').write_text(json.dumps(index))
    run(sys.executable,GAME/'scripts/prepare_romfs_backend.py','--version','v143')
    env=os.environ.copy();env.update(DM_RULESET='native',DM_PRIVATE_GATES='0',DM_THREAD_POINTER_REGISTER='tpidr_el0')
    for name in ['main','sdk','subsdk0']:
        run(sys.executable,GAME/'third_party/d2r/patch_nso.py',extraction/'exefs'/name,rewritten,name,env=env)
    run(sys.executable,ROOT/'android/prepare_android_runtime.py')
    print('Prepared verified local modules and RomFS index. Keep the original RomFS image in place.')

def main():
    ap=argparse.ArgumentParser(description=__doc__);subs=ap.add_subparsers(dest='command',required=True)
    subs.add_parser('doctor')
    p=subs.add_parser('prepare');p.add_argument('--exefs',type=Path,required=True);p.add_argument('--romfs',type=Path,required=True)
    subs.add_parser('build');subs.add_parser('translator');subs.add_parser('android-translator')
    p=subs.add_parser('shaders');p.add_argument('--limit',type=int,default=0)
    p=subs.add_parser('install');p.add_argument('--serial',required=True);p.add_argument('--apply',action='store_true',help='Actually install/copy to the named device; otherwise print plan')
    args=ap.parse_args()
    if args.command=='doctor':return 0 if doctor() else 1
    if args.command=='prepare':prepare(args)
    elif args.command=='build':run('bash',ROOT/'android/game_activity/build_apk.sh')
    elif args.command=='android-translator':run('zsh',GAME/'tools/totk_xlate/publish_android.sh',os.environ.get('DOTNET','dotnet'))
    elif args.command=='translator':run('zsh',GAME/'tools/totk_xlate/publish_native.sh',os.environ.get('DOTNET','dotnet'))
    elif args.command=='shaders':run(sys.executable,ROOT/'android/tools/android_shader_seed.py','--out',ROOT/'publication-work/shader-seed','--limit',args.limit)
    elif args.command=='install':
        # Deliberately no launch, save import, environment overwrite or driver change.
        device='/data/local/tmp/totk-android-v143'
        apk=ROOT/'android/game_activity/build/TOTK-Game-Loader.apk'
        seed=ROOT/'publication-work/shader-seed'
        required=[apk,GAME/'extracted/v143/romfs/runtime-index.bin']+[GAME/f'rewritten/android-v143/{n}.patched.bin' for n in ['main','sdk','subsdk0']]
        for p in required:
            if not p.is_file():raise ValueError('Missing install input: '+str(p))
        print('Install private APK, 3 local modules, RomFS image/index onto device (shaders compile locally) '+args.serial+'. No game launch.')
        if not args.apply:print('Plan only. Add --apply to execute.');return 0
        adb=['adb','-s',args.serial]
        run(*adb,'install','-r',apk)
        run(*adb,'shell','mkdir','-p',device+'/romfs')
        for name in ['main','sdk','subsdk0']:run(*adb,'push',GAME/f'rewritten/android-v143/{name}.patched.bin',device+'/')
        for name in ['romfs.bin','runtime-index.bin']:run(*adb,'push',GAME/f'extracted/v143/romfs/{name}',device+'/romfs/')
        print('Installed. Existing saves and driver/environment selections were left in place.')
    return 0
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,FileNotFoundError,subprocess.CalledProcessError) as exc:sys.exit(str(exc))
