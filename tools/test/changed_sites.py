"""The port's return-address matches (lines marked retail-address) that fall inside a function the port's overlay
changes: there the call has moved, and an exact match no longer fires (2026-10-12: every projectile kill in the
feed was "DESTROYED" after the Dark Tooth change to weaponProjectileUpdate). Run after building the variant;
exit 1 if any is found that is not matched by function (ALLOWED). Match such calls by their function's range."""
import sys
import json, re, glob, os
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..') + '/'
ALLOWED = {0x80109BF4,  # feat_hooks.c: the mine kill, matched by weaponDropCheckHit's range
           0x800DC018,  # feat_WeaponHit: matched by weaponProjectileUpdate's range
           # feat_hooks.c: matched again by their function (gr_function) where the variant moved them
           0x8010D45C, 0x8010D770, 0x8010AA08, 0x8010E424, 0x8010E7E4, 0x800E7340, 0x80108B50, 0x80108BA0,
           0x8010F8D0, 0x801069E8, 0x800E7C04, 0x8011C1A0, 0x80110638,
           # matched by function too (2026-10-17, the decomp's requests 41-42 changed them): bulletsMove, carContactRespond,
           # areaDamage, the splash's callers (weaponFireSpecial, weaponDropWrite), targetActorsUpdate
           0x800D81EC, 0x800E3A20, 0x8010B7E4, 0x8010BA00, 0x80106EAC, 0x80109908, 0x80109CAC, 0x80126C80}
j = json.load(open(R + 'build/decomp-wc/out/TM2_SOURCE_PORT.json'))
chs = {int(x['retail_va'], 16): (x['name'], x.get('delta')) for x in j['changed_functions']}
print(len(chs), 'changed:', ', '.join('%s(%+d)' % (n, d or 0) for n, d in chs.values()))
bad = 0
rng = []
for l in open(R + 'build/port-hooks/gen/addrmap.c'):
    m = re.search(r'\{0x([0-9A-F]+)u, 0x([0-9A-F]+)u, 0x([0-9A-F]+)u\}', l)
    if m:
        rng.append((int(m.group(1), 16), int(m.group(2), 16)))
for f in glob.glob(R + 'src/**/*.c', recursive=True):
    for i, l in enumerate(open(f, encoding='utf-8', errors='replace'), 1):
        if 'retail-address' not in l:
            continue
        for m in re.finditer(r'0x(80[0-9A-Fa-f]{6})u', l):
            a = int(m.group(1), 16)
            for s, e in rng:
                if s <= a < e:
                    if s in chs and a != s and a not in ALLOWED:
                        print('moved:', os.path.relpath(f, R), i, hex(a), chs[s][0])
                        bad += 1
                    break
sys.exit(1 if bad else 0)
