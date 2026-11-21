"""python symtop.py EXE SAMPLES.txt [N]: the samples' (tid rva) hottest functions per busiest thread"""
import bisect, collections, os, subprocess, sys
exe, samples = sys.argv[1], sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
NM = os.environ.get('LLVM_NM', 'llvm-nm')  # (llvm-mingw's)
syms = []
for line in subprocess.run([NM, '--defined-only', exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
BASE = 0x140000000
EXE_BASE = 0  # (sampler.py writes the exe's samples as offsets from its base)
per = collections.defaultdict(collections.Counter)
for line in open(samples):
    p = line.split()
    if len(p) != 3:
        continue
    tid, mod, addr = p[0], p[1], int(p[2], 16)
    if not mod.lower().startswith('tm2pc'):
        per[tid][mod] += 1
        continue
    a = BASE + addr - EXE_BASE
    rva = addr - EXE_BASE
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][1] if i >= 0 and a - addrs[i] < 0x200000 else '?%x' % rva
    per[tid][name] += 1
for tid, c in sorted(per.items(), key=lambda kv: -sum(v for k2, v in kv[1].items() if not k2.endswith(('.dll', '.DLL'))))[:2]:
    tot = sum(c.values())
    print('thread', tid, tot, 'samples')
    for name, k in c.most_common(n):
        print('  %5.1f%%  %s' % (100.0 * k / tot, name))
