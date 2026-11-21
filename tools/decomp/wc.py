"""The decomp working copy: the port's own changes to the game's C (docs/HYBRID.md, "Our changes in C").

references/TM2-Decomp is the decomp's checkout and is never written. This script copies it to
build/decomp-wc (its sources, tools and config; the toolchain through a junction, read only), applies
the port's overlay (decomp/files: new files, decomp/patches: changes to the decomp's files, every
change under -DTM2_PORT_HOOKS), and builds the port variant from the decomp's last build's objects,
recompiling only the units the overlay changes.

    python tools/decomp/wc.py sync      copy the decomp, apply the overlay
                                        (--rev REV: the decomp's commit REV, e.g. origin/main, exported with git
                                        archive, instead of its folder as it is: its uncommitted work left out)
    python tools/decomp/wc.py build     build the variant: build/decomp-wc/out/TM2_SOURCE_PORT.EXE (+ .json, maps)
                                        (--quick: without the decomp's proof against E0; units cached by content)
    python tools/decomp/wc.py diff      write the overlay back from the working copy (after editing it there)

The working copy holds retail bytes (split/, the build): it stays in build/ (gitignored)."""
import json
import shutil
import tarfile
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DECOMP = REPO / 'references' / 'TM2-Decomp'
WC = REPO / 'build' / 'decomp-wc'
OVERLAY = REPO / 'decomp'
COPY_DIRS = ('bin', 'config', 'include', 'src', 'tools', 'split', 'reference')
BASE_DIRS = ('config', 'include', 'src', 'tools')  # the ones the overlay changes
COPY_FILES = ('build.py', 'build.cmd', 'BUILD_MANIFEST.json', 'PACKAGE.json', 'LAST_BUILD.json',
              'LAST_SOURCE_BUILD.json', 'LAST_RELOC_BUILD.json', 'LAST_PORT_BUILD.json')
PY = WC / 'toolchain' / 'python' / 'python.exe'


def run(cmd, **kw):
    print('>', ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def base_build_dir():
    """The decomp build whose objects the variant starts from: the newest one with its E0 linked (the decomp
    may be building another meanwhile). sync records it in build/decomp-wc/BASE_BUILD."""
    done = [d for d in (DECOMP / 'work').glob('build-*') if (d / 'source_reloc_e0' / 'report.json').exists()]
    return max(done, key=lambda d: d.name).relative_to(DECOMP)


def synced_build_dir():
    return Path((WC / 'BASE_BUILD').read_text(encoding='utf-8').strip())


def source_root():
    """Where the decomp's files come from: its folder, or with --rev REV that commit, exported (git archive, read
    only) to build/decomp-src; the files git does not track (bin/, split/: retail bytes) still from the folder."""
    if '--rev' not in sys.argv:
        return DECOMP, 'HEAD'
    rev = sys.argv[sys.argv.index('--rev') + 1]
    out = REPO / 'build' / 'decomp-src'
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    tar = out.parent / 'decomp-src.tar'
    with open(tar, 'wb') as f:
        subprocess.run(['git', '-C', DECOMP, 'archive', '--format=tar', rev], check=True, stdout=f)
    with tarfile.open(tar) as t:
        t.extractall(out)
    tar.unlink()
    for d in ('bin', 'split'):
        if not (out / d).exists() and (DECOMP / d).exists():
            shutil.copytree(DECOMP / d, out / d)
    return out, rev


def sync():
    src, rev = source_root()
    for d in COPY_DIRS:
        if (WC / d).exists():
            shutil.rmtree(WC / d)
        shutil.copytree(src / d, WC / d, ignore=shutil.ignore_patterns('__pycache__'))
    for f in COPY_FILES:
        if (src / f).exists():
            shutil.copy2(src / f, WC / f)
    if not (WC / 'toolchain').exists():
        run(['cmd', '/c', 'mklink', '/J', WC / 'toolchain', DECOMP / 'toolchain'])
    # the last build's objects (the variant recompiles only what changes)
    b = base_build_dir()
    (WC / 'BASE_BUILD').write_text(b.as_posix() + '\n', encoding='utf-8')
    dst = WC / b
    if not dst.exists():
        dst.mkdir(parents=True)
        for part in ('compile', 'source_reloc_e0'):
            if (DECOMP / b / part).exists():
                shutil.copytree(DECOMP / b / part, dst / part)
        shutil.copy2(DECOMP / b / 'selection.json', dst / 'selection.json')
    # the decomp's files as synced, untouched: what `diff` compares the working copy with (the decomp's own
    # folder moves on while we work)
    if (WC / '.base').exists():
        shutil.rmtree(WC / '.base')
    for d in BASE_DIRS:
        shutil.copytree(src / d, WC / '.base' / d, ignore=shutil.ignore_patterns('__pycache__'))
    for old in (WC / 'work').glob('build-*'):
        if old.name != b.name:
            shutil.rmtree(old)
    # the overlay: new files, then the patches
    for f in sorted((OVERLAY / 'files').rglob('*')):
        if f.is_file():
            t = WC / f.relative_to(OVERLAY / 'files')
            t.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, t)
    prev = (WC / 'BASE_REV').read_text(encoding='utf-8').strip() if (WC / 'BASE_REV').exists() else None
    for p in sorted((OVERLAY / 'patches').glob('*.patch')):
        apply_merging(p, prev)
    full = subprocess.run(['git', '-C', DECOMP, 'rev-parse', rev], capture_output=True, text=True).stdout.strip()
    (WC / 'BASE_REV').write_text(full + '\n', encoding='utf-8')
    print('synced: decomp', full[:7], '+ overlay; objects of', b)


def apply_merging(patch, prev):
    """Apply an overlay patch to the working copy. Where a file of it no longer applies (the decomp changed it since
    the patch was made), merge three ways: the file at `prev` (the decomp commit the patch was made against: the last
    sync's BASE_REV) with the patch applied, against `prev`'s file, onto the new one (git merge-file). A conflict
    stops the sync with the files to resolve."""
    wc_dir = WC.relative_to(REPO).as_posix()
    # (from the port repo's root: build/decomp-wc is inside it, and git apply in a subdirectory skips the paths)
    check = subprocess.run(['git', 'apply', '--check', '--whitespace=nowarn', '--directory=' + wc_dir, patch], cwd=REPO,
                           capture_output=True, text=True)
    failing = sorted({l.split(': ')[1] for l in check.stderr.splitlines() if l.startswith('error: ') and
                      l.endswith('patch does not apply')})
    if not failing:
        run(['git', 'apply', '--whitespace=nowarn', '--directory=' + wc_dir, patch], cwd=REPO)
        return
    if not prev:
        raise SystemExit('the overlay does not apply and no BASE_REV to merge from: ' + ', '.join(failing))
    excl = ['--exclude=' + f for f in failing]
    run(['git', 'apply', '--whitespace=nowarn', '--directory=' + wc_dir] + excl + [patch], cwd=REPO)
    tmp = REPO / 'build' / 'decomp-merge'
    if tmp.exists():
        shutil.rmtree(tmp)
    rels = [f[len(wc_dir) + 1:] for f in failing]
    for sub in ('old', 'patched'):
        for r in rels:
            t = tmp / sub / r
            t.parent.mkdir(parents=True, exist_ok=True)
            data = subprocess.run(['git', '-C', DECOMP, 'show', prev + ':' + r], capture_output=True, check=True).stdout
            t.write_bytes(data.replace(b'\r\n', b'\n'))
    subprocess.run(['git', 'init', '-q', tmp / 'patched'], check=True)
    run(['git', 'apply', '--whitespace=nowarn'] + ['--include=' + r for r in rels] + [patch], cwd=tmp / 'patched')
    for r in rels:  # (all three LF: the patch's lines may bring CRs)
        t = tmp / 'patched' / r
        t.write_bytes(t.read_bytes().replace(b'\r\n', b'\n'))
    conflicts = []
    for r in rels:
        cur = WC / r
        raw = cur.read_bytes()
        crlf = b'\r\n' in raw
        merged = tmp / 'merged' / r
        merged.parent.mkdir(parents=True, exist_ok=True)
        merged.write_bytes(raw.replace(b'\r\n', b'\n'))
        if subprocess.run(['git', 'merge-file', merged, tmp / 'old' / r, tmp / 'patched' / r]).returncode:
            conflicts.append(r)
        out = merged.read_bytes()
        cur.write_bytes(out.replace(b'\n', b'\r\n') if crlf else out)
        print('merged three ways:', r, '(CONFLICT)' if r in conflicts else '')
    if conflicts:
        raise SystemExit('resolve the conflicts in ' + ', '.join(conflicts) + ', then run diff')


# the decomp's port options the port uses (request 32: duplicate car types, 16 cars, the car swap; request 33: each car's
# own specials, Dark Tooth per car with portCarOptions bit 6 for Survival's boss anywhere)
PORT_DEFINES = ['--port-define', 'TM2_PORT_CAR_GROUPS', '--port-define', 'TM2_PORT_CARS=16', '--port-define', 'TM2_PORT_CAR_SWAP',
                '--port-define', 'TM2_PORT_CAR_DROPIN',  # (requests 33 and 50: a player joining a match on takes a computer car's place)
                '--port-define', 'TM2_PORT_CAR_SPECIALS', '--port-define', 'TM2_PORT_DARK_TOOTH',
                '--port-define', 'TM2_PORT_TEXTURE_BANKS',  # (request 32 step 4: portCarOptions bit 5 when a match needs them)
                '--port-define', 'TM2_PORT_CAR_SOUND',  # (request 33-3: each car's engine its own voice, bit 4)
                '--port-define', 'TM2_PORT_SPU48',  # (request 34: 48 sound voices, the driver on 0..43, 44..47 the port's)
                '--port-define', 'TM2_PORT_SPECIAL_AS',  # (request 35: Gun Game's specials, any car fires another's)
                '--port-define', 'TM2_PORT_HUMANS=16',
                '--port-define', 'TM2_PORT_HAZARDS',  # (request 38: arena hazards / destructibles off, portArenaOptions)
                '--port-define', 'TM2_PORT_BALANCE',  # (requests 40-41: Vanilla Plus balancing, portBalance)
                '--port-define', 'TM2_PORT_DT_PLAYER',  # (request 36: a player drives Dark Tooth; offered with portCarOptions bit 8)
                '--port-define', 'TM2_PORT_C_EMITTER',  # (request 61 addendum / 65: the C emitter, subpixel beam and sprite corners)
                '--port-define', 'TM2_PORT_CAR_NUMBERS',  # (request 42 items 3, 5: car numbers past retail's, mod cars 0x20 + m in portModCars)
                '--port-define', 'TM2_PORT_CAR_PROFILE',  # (requests 70-71: a mod character's own numbers, portCarProfile; portCarSelLoad)
                '--port-define', 'TM2_PORT_FAR_DISTANCE',  # (request 31: past 8,000 units, portFarDistance; the raised caps with it)
                '--port-define', 'TM2_PORT_RENDER_SLOTS=400',  # (request 47: 400 matrix slots a view, the all-C walker)
                '--port-define', 'TM2_PORT_ROLLBACK',  # (request 48 stage 1: fixed ticks, the seed, hold, the port's pause; ONLINE_ROLLBACK.md 10)
                '--port-define', 'TM2_PORT_VISUAL',  # (request 42: portVisual, the taser and the lightning by depth, the taser lower)
                '--port-define', 'TM2_PORT_HIDDEN_AREAS',  # (request 72: Paris's church seen from anywhere, portVisual bit 6)
                '--port-define', 'TM2_PORT_FALL_WHEELS',  # (request 107: the pit death from the wheels: Dark Tooth dies on the floor too)
                '--port-define', 'TM2_PORT_CAR_POSE',  # (request 108: portCarPose / portCarSpecialSound, the Versus podium)
                '--port-define', 'TM2_PORT_WHOLE_MAPS', '--port-define', 'TM2_PORT_FWY_GROUND',  # (111: Antarctica's floes kept with the whole maps in co-op; 112: LA's corner curves' ground, always)
                '--port-define', 'TM2_PORT_MOD_ARENA',  # (request 114: a mod arena's health spot and the AI's warp exits, portArenaRules)
                '--port-define', 'TM2_PORT_WRECK_FALL',  # (request 117: a falling wreck hidden with its splash on Antarctica, or exploding at the floor)
                '--port-define', 'TM2_PORT_SPECIAL_PROFILE']  # (request 73: mod characters' specials, portSpecialProfile)


def port_units():
    """--port-unit for each C file the overlay adds under src/port (port code: .port_text)"""
    out = []
    for f in sorted((OVERLAY / 'files' / 'src' / 'port').glob('*.c')):
        out += ['--port-unit', 'src/port/' + f.name]
    return out


def build():
    b = synced_build_dir()
    out = WC / 'work' / 'pv' / 'P4'
    if out.exists():
        shutil.rmtree(out)
    run([PY, '-I', '-B', 'tools/build_source_only.py', '--build-dir', b.as_posix(), '--relocatable', '--sdk-from-lib',
         '--port-variant', '--port-define', 'TM2_PORT_HOOKS', '--port-define', 'TM2_PORT_FAR_LOD'] + PORT_DEFINES + port_units() + ['--output', 'work/pv/P4'], cwd=WC)
    e0 = WC / b / 'source_reloc_e0'
    if e0.exists() and '--quick' not in sys.argv:  # (--quick: no proof, while iterating; run it before a commit)
        run([PY, '-I', '-B', 'tools/verify_reloc.py', 'work/pv/P4', '--e0', e0.relative_to(WC).as_posix()], cwd=WC)
    # publish as the decomp does (out/TM2_SOURCE_PORT.*), for the port's CMake (TM2_EXE, TM2_ADDRMAP, TM2_PORT_JSON)
    pub = WC / 'out'
    pub.mkdir(exist_ok=True)
    run([PY, '-I', '-B', REPO / 'tools' / 'decomp' / 'publish.py', out, pub], cwd=WC)
    # the rollback snapshot's regions for this link (decomp ONLINE_ROLLBACK.md 12.4): the port's recompiler reads them
    if (WC / 'tools' / 'rollback_regions.py').exists():
        run([PY, '-I', '-B', 'tools/rollback_regions.py', 'work/pv/P4', '--out', (pub / 'TM2_SOURCE_PORT' / 'rollback_regions.tsv').as_posix()], cwd=WC)


def diff():
    """Write decomp/patches/0001-port-hooks.patch: the working copy's changes to the decomp's files, against the
    files as synced (build/decomp-wc/.base), not the decomp's folder (which changes meanwhile); and the overlay's
    own files (decomp/files) as the working copy has them."""
    base = WC / '.base'
    if not base.exists():
        raise SystemExit('no build/decomp-wc/.base: sync first')
    new_files = {f.relative_to(OVERLAY / 'files').as_posix() for f in (OVERLAY / 'files').rglob('*') if f.is_file()}
    patch = []
    for d in BASE_DIRS:
        for f in sorted((WC / d).rglob('*')):
            if not f.is_file() or '__pycache__' in f.parts:
                continue
            rel = f.relative_to(WC).as_posix()
            src = base / rel
            if not src.exists():
                if rel not in new_files:
                    print('new file (add it to decomp/files):', rel)
                elif (OVERLAY / 'files' / rel).read_bytes() != f.read_bytes():  # the overlay's own files: kept too
                    shutil.copy2(f, OVERLAY / 'files' / rel)
                    print('overlay file saved:', rel)
                continue
            if src.read_bytes() == f.read_bytes():
                continue
            r = subprocess.run(['git', 'diff', '--no-index', src, f], capture_output=True, text=True)
            lines = r.stdout.splitlines(keepends=True)
            start = next(i for i, l in enumerate(lines) if l.startswith('@@'))
            head = ['diff --git a/%s b/%s\n' % (rel, rel), '--- a/%s\n' % rel, '+++ b/%s\n' % rel]
            patch.append((rel, ''.join(head + lines[start:])))
    (OVERLAY / 'patches').mkdir(parents=True, exist_ok=True)
    (OVERLAY / 'patches' / '0001-port-hooks.patch').write_text(''.join(p for _, p in patch), encoding='utf-8',
                                                                 newline='\n')
    print('patch:', len(patch), 'files:', ', '.join(rel for rel, _ in patch))


if __name__ == '__main__':
    {'sync': sync, 'build': build, 'diff': diff}[sys.argv[1]]()
