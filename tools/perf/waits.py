"""python waits.py EXE SAMPLES: for the busiest thread's samples outside the exe (or in SDL_GetAtomicInt), the exe callers
on the stack (the first few 'e' return addresses, named)"""
import bisect, collections, os, subprocess, sys
exe, samples = sys.argv[1], sys.argv[2]
NM = os.environ.get('LLVM_NM', 'llvm-nm')  # (llvm-mingw's)
syms = []
for line in subprocess.run([NM, '--defined-only', exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
def name(rva):
    a = 0x140000000 + rva
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][1] if i >= 0 else '?'
threads = collections.Counter()
rows = []
for line in open(samples):
    p = line.split()
    if len(p) < 3:
        continue
    rows.append(p)
    if p[1].lower().startswith('tm2pc'):
        threads[p[0]] += 1
main = threads.most_common(1)[0][0]
chains = collections.Counter()
total = 0
for p in rows:
    if p[0] != main:
        continue
    total += 1
    here = p[1] if not p[1].lower().startswith('tm2pc') else name(int(p[2], 16))
    if p[1].lower().startswith('tm2pc') and here != 'SDL_GetAtomicInt_REAL':
        continue
    stack = p[3].split(',') if len(p) > 3 else []
    callers = []
    for x in stack:
        if x.startswith('e'):
            n = name(int(x[1:], 16))
            if not callers or callers[-1] != n:
                callers.append(n)
        if len(callers) >= 4:
            break
    chains[here + ' <- ' + ' <- '.join(callers)] += 1
print('main thread', main, total, 'samples')
for c, k in chains.most_common(15):
    print('%5.1f%%  %s' % (100.0 * k / total, c))
