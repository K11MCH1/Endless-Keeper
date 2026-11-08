"""Build the phone's KeeperFX engine on the PC from a local KeeperFX source tree.

The launcher's "Rebuild game" downloads an official KeeperFX revision and compiles it on
the phone (EngineBuilder.java). This script does the same build on the PC, with the NDK's
Clang and the same engine kit, for source trees the phone cannot download: a KeeperFX
revision with your own patches on top. It can then install the library on a phone as
that phone's phone-built engine, so Play KeeperFX loads it like one built on the phone.

  1. copy --src (a KeeperFX tree without the Android seams: stock plus your patches) to
     <work>/src and apply the kit's seams there once, as the phone does on a fresh download.
     A seam whose anchor changed in that revision: --skip-seams N,... and give its new form
     in --extra-seams FILE (a JSON list of {"file", "old", "new"}, paths under src/)
  2. generate ver_defs.h and the window icon, compile src/**, the kit's deps and native
     sources with kit.json's flags (-O2 -g0, as on the phone), link libkeeperfx.so
     against the kit's libkrdeps.so, libSDL3.so and libc++_shared.so
     (--native DIR: the kit's native sources adapted to a newer KeeperFX API, same names)
  3. --install SERIAL: copy it to files/engine/<tag>/ of the debuggable app (run-as) and
     point state.json's engine entry at it, for the KeeperFX data version given by --for

The kit and the three libraries must be the ones inside the APK installed on the phone
(their kit_id is what the launcher compares). --from-phone SERIAL takes them from there.

  python scripts/android/build_engine_pc.py --src C:/path/keeperfx --build-number 5593 \
      --revision 89e4728+mypatch+android --from-phone SERIAL --install SERIAL
"""
import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOCAL = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local'))
EXE = '.exe' if os.name == 'nt' else ''
LIBS = ('libkrdeps.so', 'libSDL3.so', 'libc++_shared.so')
NO_PATHCONV = dict(os.environ, MSYS_NO_PATHCONV='1')


class BuildError(Exception):
    pass


def run(args, **kw):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, env=NO_PATHCONV, **kw)
    if result.returncode:
        raise BuildError(f'{Path(str(args[0])).name} {" ".join(map(str, args[1:4]))} failed:\n'
                         f'{result.stdout[-3000:]}{result.stderr[-3000:]}')
    return result.stdout


# ---- kit ----

def from_phone(serial, package, dest):
    """The installed APK's engine kit and link libraries, as the phone's own builds use them."""
    apk = run(['adb', '-s', serial, 'shell', 'pm', 'path', package]).splitlines()[0].strip().removeprefix('package:')
    tmp = '/data/local/tmp/kr-engine-kit'
    run(['adb', '-s', serial, 'shell', f'rm -rf {tmp} && mkdir -p {tmp} && cd {tmp} && unzip -o -q {apk} '
         'assets/engine-kit.zip assets/compiler.json ' + ' '.join(f'lib/arm64-v8a/{n}' for n in LIBS)])
    dest.mkdir(parents=True, exist_ok=True)
    run(['adb', '-s', serial, 'pull', f'{tmp}/assets/.', str(dest)])
    run(['adb', '-s', serial, 'pull', f'{tmp}/lib/arm64-v8a/.', str(dest)])
    run(['adb', '-s', serial, 'shell', f'rm -rf {tmp}'])
    return dest / 'engine-kit.zip', dest


def unpack_kit(archive, dest):
    with zipfile.ZipFile(archive) as z:
        z.extractall(dest)
    return dest, json.loads((dest / 'kit.json').read_text(encoding='utf-8'))


# ---- source ----

def apply_seams(kit, src, skip, extra):
    """Once, on a fresh copy: a later seam may edit an earlier one's text, so a second run
    would not recognise the earlier seam as applied."""
    doc = json.loads((kit / 'seams.json').read_text(encoding='utf-8'))
    base = src / doc.get('base', 'src')
    applied = present = 0
    failed = []
    seams = [s for i, s in enumerate(doc['seams']) if i not in skip] + extra
    for i, seam in enumerate(seams):
        path = base / seam['file']
        if not path.is_file():
            failed.append(f'#{i} {seam["file"]}: file missing')
            continue
        text = path.read_text(encoding='utf-8').replace('\r\n', '\n')
        if seam['new'] in text:
            present += 1
            continue
        if text.count(seam['old']) != 1:
            failed.append(f'#{i} {seam["file"]}: anchor found {text.count(seam["old"])} times: {seam["old"][:80]!r}')
            continue
        path.write_bytes(text.replace(seam['old'], seam['new']).encode('utf-8'))
        applied += 1
    if failed:
        raise BuildError('Seams that do not apply (skip them with --skip-seams and give their new form in '
                         '--extra-seams; numbers count the kit\'s seams.json, then the extra ones):\n' + '\n'.join(failed))
    if present:
        raise BuildError(f'{present} seams were already in --src: give a tree without the Android seams')
    print(f'seams: {applied} applied ({len(extra)} extra), {len(skip)} of the kit\'s skipped')


def generate(src, gen, build_number, revision, suffix, defines=()):
    gen.mkdir(parents=True, exist_ok=True)
    mk = (src / 'version.mk').read_text(encoding='utf-8')
    v = dict(re.findall(r'(?m)^(VER_MAJOR|VER_MINOR|VER_RELEASE)=(\d+)', mk))
    if len(v) != 3:
        raise BuildError('version.mk has no version numbers')
    v.update(BUILD_NUMBER=str(build_number), PACKAGE_SUFFIX=suffix, GIT_REVISION=revision,
             VER_STRING=f"{v['VER_MAJOR']}.{v['VER_MINOR']}.{v['VER_RELEASE']}.{build_number} {suffix}")
    for d in defines:
        key, _, value = d.partition('=')
        v[key] = value
    template = (src / 'ver_defs.h.in').read_text(encoding='utf-8')
    (gen / 'ver_defs.h').write_text(re.sub(r'\$\{(\w+)\}', lambda m: v.get(m.group(1), ''), template), encoding='utf-8')
    icon = (src / 'res/keeperfx_icon256-24bpp.png').read_bytes()
    (gen / 'window_icon.c').write_text('const unsigned char kfx_window_icon_png[] = {' + ','.join(f'0x{b:02x}' for b in icon)
                                       + '};\nconst unsigned int kfx_window_icon_png_size = sizeof(kfx_window_icon_png);\n',
                                       encoding='utf-8')
    return v['VER_STRING']


def units(recipe, src, kit, gen):
    sources = recipe['sources']
    exclude = [re.compile(p) for p in sources['exclude']]
    found = []
    for root in sources['roots']:
        for path in (src / root).rglob('*'):
            if path.suffix in ('.c', '.cpp') and path.is_file():
                rel = '/' + path.relative_to(src).as_posix()
                if not any(p.search(rel) for p in exclude):
                    found.append(path)
    found.sort(key=lambda p: p.as_posix())
    found += [src / e for e in sources['extra']]
    found += [kit / 'native' / n for n in sources['native']]
    found.append(gen / 'window_icon.c')
    return found


# ---- compile and link ----

def llvm_bin(ndk):
    """The NDK's clang folder for this host (windows-x86_64, linux-x86_64 ...)."""
    found = sorted((ndk / 'toolchains/llvm/prebuilt').glob('*/bin'))
    if not found:
        raise BuildError(f'No NDK clang under {ndk}')
    return found[0]


def compile_all(ndk, recipe, src, kit, gen, objects, jobs):
    bin_dir = llvm_bin(ndk)
    target = f"--target=aarch64-linux-android{recipe['api']}"

    def subst(s):
        return s.replace('@SRC@', str(src)).replace('@GEN@', str(gen)).replace('@KIT@', str(kit))
    common = [target, '-O2', '-g0', *recipe['defines'], *('-I' + subst(i) for i in recipe['includes']),
              *(a for i in recipe['system_includes'] for a in ('-isystem', subst(i))),
              '-include', subst(recipe['force_include'])]
    todo = units(recipe, src, kit, gen)
    objects.mkdir(parents=True, exist_ok=True)
    for stale in objects.glob('*.o'):
        stale.unlink()

    def one(index, unit):
        cxx = unit.suffix == '.cpp'
        obj = objects / f'{index:04d}.o'
        cmd = [bin_dir / ('clang++' + EXE if cxx else 'clang' + EXE), *(recipe['cxx'] if cxx else recipe['c']),
               '-std=gnu++20' if cxx else '-std=gnu11', *common, '-c', unit, '-o', obj]
        result = subprocess.run([str(a) for a in cmd], capture_output=True, text=True)
        if result.returncode:
            raise BuildError(f'{unit}:\n{result.stderr[-4000:]}')
        return obj, result.stderr.count('warning:')
    started, warnings, done = time.time(), 0, 0
    with ThreadPoolExecutor(jobs) as pool:
        futures = [pool.submit(one, i, u) for i, u in enumerate(todo)]
        try:
            for f in as_completed(futures):
                warnings += f.result()[1]
                done += 1
                if done % 50 == 0 or done == len(todo):
                    print(f'  compiled {done}/{len(todo)}', flush=True)
        except BaseException:
            for f in futures:
                f.cancel()
            raise
    print(f'compiled {len(todo)} files in {time.time() - started:.0f} s ({warnings} warnings)')
    return sorted(objects.glob('*.o'))


def link(ndk, recipe, objs, libs, out):
    bin_dir = llvm_bin(ndk)
    rsp = out.with_suffix('.rsp')
    rsp.write_text('\n'.join(f'"{o.as_posix()}"' for o in objs), encoding='utf-8')
    run([bin_dir / ('clang' + EXE), f"--target=aarch64-linux-android{recipe['api']}", '-shared', '-nostdlib++',
         '-o', out, '-Wl,-soname,libkeeperfx.so', '-Wl,-z,now', '-Wl,-z,relro', '-Wl,-z,noexecstack',
         '-Wl,-z,max-page-size=16384', '-Wl,--hash-style=gnu', '-Wl,--build-id=sha1', '-Wl,--no-undefined',
         '-Wl,--gc-sections', '-Wl,--fix-cortex-a53-843419', f'@{rsp}', *(libs / n for n in recipe['link']),
         *('-l' + n for n in recipe['system_libs']), '-latomic'])
    print(f'linked {out} ({out.stat().st_size / 1048576:.1f} MiB)')


# ---- install ----

def install(serial, package, lib, tag, version, revision, label, recipe, seconds):
    def shell(cmd):
        return run(['adb', '-s', serial, 'shell', cmd])
    top = shell('dumpsys activity activities | grep -m1 topResumedActivity')
    if package in top:
        raise BuildError(f'{package} is in front on the phone; close it before installing the engine')
    files = f'/data/user/0/{package}/files'
    shell(f'am force-stop {package}')  # the launcher keeps state.json in memory while it runs
    run(['adb', '-s', serial, 'push', str(lib), '/data/local/tmp/kr-libkeeperfx.so'])
    shell(f'run-as {package} mkdir -p files/engine/{tag} && run-as {package} cp /data/local/tmp/kr-libkeeperfx.so '
          f'files/engine/{tag}/libkeeperfx.so && rm /data/local/tmp/kr-libkeeperfx.so')
    state = json.loads(shell(f'run-as {package} cat files/state.json'))
    data = state.get('alpha_version') or state.get('kfx_version', '')
    if version != data:
        print(f'warning: the phone has KeeperFX {data or "?"} data; the engine is for {version}', file=sys.stderr)
    previous = state.get('engine')
    state['engine'] = {'lib': f'{files}/engine/{tag}/libkeeperfx.so', 'for': version, 'commit': revision,
                       'label': label, 'kit': recipe['kit'], 'kit_id': recipe['kit_id'], 'source': 'pc',
                       'built_at': datetime.datetime.now().astimezone().strftime('%Y-%m-%dT%H:%M:%S%z'),
                       'seconds': seconds}
    state['engine_error'] = ''
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False, encoding='utf-8') as f:
        json.dump(state, f, indent=1)
    run(['adb', '-s', serial, 'push', f.name, '/data/local/tmp/kr-state.json'])
    os.unlink(f.name)
    shell(f'run-as {package} cp /data/local/tmp/kr-state.json files/state.json && rm /data/local/tmp/kr-state.json')
    print(f'installed as {files}/engine/{tag}/libkeeperfx.so for KeeperFX {version}'
          + (f' (previous engine: {previous.get("lib")})' if previous else ''))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--src', type=Path, required=True, help='KeeperFX source tree without the Android seams (copied)')
    p.add_argument('--build-number', type=int, required=True, help='KeeperFX build number, e.g. 5593')
    p.add_argument('--revision', required=True, help='GIT_REVISION logged by the game, e.g. 89e4728+mine+android')
    p.add_argument('--suffix', default='Android ARM64', help='PACKAGE_SUFFIX (default: as phone builds)')
    p.add_argument('--define', action='append', default=[], metavar='KEY=VALUE',
                   help='another ver_defs.h.in value, e.g. ENDLESS_RELEASE=4 (may be repeated)')
    p.add_argument('--work', type=Path, default=ROOT / 'build/engine-pc')
    p.add_argument('--kit', type=Path, help='engine-kit.zip (default: --from-phone, else build/engine-kit)')
    p.add_argument('--libs', type=Path, help='folder with libkrdeps.so, libSDL3.so, libc++_shared.so')
    p.add_argument('--from-phone', metavar='SERIAL', help='take the kit and libraries from the installed APK')
    p.add_argument('--package', default='org.keeperrecomped.app')
    p.add_argument('--skip-seams', default='', help='comma-separated numbers of kit seams that no longer apply')
    p.add_argument('--extra-seams', type=Path, help='JSON list of {"file","old","new"} applied after the kit\'s seams')
    p.add_argument('--native', type=Path, help='folder of the kit\'s native sources adapted to this KeeperFX revision '
                   '(same names; they replace the kit\'s copies)')
    p.add_argument('--ndk', type=Path, default=LOCAL / 'Android/Sdk/ndk/27.3.13750724')
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    p.add_argument('--install', metavar='SERIAL', help='install on this phone as its phone-built engine')
    p.add_argument('--for', dest='version', help='KeeperFX data version it is for (default 1.4.0.<build>)')
    p.add_argument('--tag', help='engine folder on the phone (default: from --revision)')
    p.add_argument('--label', help='name the launcher shows (default: KeeperFX <version> (<revision>))')
    a = p.parse_args()
    src = a.src.resolve()
    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if a.from_phone:
        kit_zip, libs = from_phone(a.from_phone, a.package, work / 'phone-kit')
    else:
        kit_zip = a.kit or ROOT / 'build/engine-kit/engine-kit.zip'
        libs = a.libs or work / 'libs'
        if not a.libs:
            libs.mkdir(exist_ok=True)
            for n, d in zip(LIBS, (ROOT / 'build/engine-kit', ROOT / 'build/android/native/arm64-v8a',
                                   ROOT / 'build/android/native/arm64-v8a')):
                (libs / n).write_bytes((d / n).read_bytes())
    kit, recipe = unpack_kit(kit_zip, work / 'kit')
    print(f"kit {recipe['kit_id']} (kit {recipe['kit']}, API {recipe['api']})")
    if a.native:
        for f in sorted(a.native.iterdir()):
            if not (kit / 'native' / f.name).is_file():
                raise BuildError(f'{f.name} is not one of the kit\'s native sources')
            (kit / 'native' / f.name).write_bytes(f.read_bytes())
            print(f'native source replaced: {f.name}')
    tree = work / 'src'
    if tree.exists():
        shutil.rmtree(tree)
    shutil.copytree(src, tree, ignore=shutil.ignore_patterns('.git'))
    src = tree
    extra = json.loads(a.extra_seams.read_text(encoding='utf-8')) if a.extra_seams else []
    apply_seams(kit, src, {int(s) for s in a.skip_seams.split(',') if s.strip()}, extra)
    started = time.time()
    ver = generate(src, work / 'gen', a.build_number, a.revision, a.suffix, a.define)
    print(f'version {ver} git:{a.revision}')
    objs = compile_all(a.ndk, recipe, src, kit, work / 'gen', work / 'obj', a.jobs)
    out = work / 'libkeeperfx.so'
    link(a.ndk, recipe, objs, libs, out)
    seconds = int(time.time() - started)
    if a.install:
        version = a.version or f'1.4.0.{a.build_number}'
        tag = a.tag or re.sub(r'[^A-Za-z0-9._-]', '_', a.revision)[:40]
        install(a.install, a.package, out, tag, version, a.revision,
                a.label or f'KeeperFX {version} ({a.revision})', recipe, seconds)


if __name__ == '__main__':
    try:
        main()
    except BuildError as error:
        sys.exit(f'build_engine_pc: {error}')
