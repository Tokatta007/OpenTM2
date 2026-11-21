"""Find the game's retail addresses in the port's C that are not translated with GA() (docs/HYBRID.md).

    python tools/test/check_addresses.py

The port names the game by retail addresses. In a build at a new layout (the hybrid: the decomp's
variant) they are elsewhere, and GA(retail) gives where (src/runtime/addrmap.c). A literal in the
executable's range (800CDC54..801FAD48) used without it works in the retail build only, so the
hybrid breaks there and nowhere else. Comments and strings are skipped. A literal that is meant as a
retail address (compared with gr() of an address, a table keyed by retail addresses) carries the
marker `retail-address` in a comment on its line; a `case` label so marked (compared with gr() of a return
address) must not lie in a function the hybrid rewrites (build/decomp-wc/out, if built). Exit code 1 when any is
found."""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LO, HI = 0x800CDC54, 0x801FAD48
DIRS = ('src', 'src/vplus', 'src/ui', 'src/profile', 'src/music', 'src/controls', 'src/runtime')
SKIP = {'src/runtime/rebase_table.h'}  # generated: the retail column of the moved arrays


def literals(text):
    """(offset, value) of each 0x80xxxxxx literal outside comments and strings, and not already in GA()"""
    i, n = 0, len(text)
    while i < n:
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if text[i] in '"\'':
            q, j = text[i], i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == chr(92) else 1
            i = j + 1
            continue
        if text.startswith('0x80', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            j = i + 2
            while j < n and text[j] in '0123456789abcdefABCDEF':
                j += 1
            if j - i == 10:
                v = int(text[i + 2:j], 16)
                if LO <= v < HI and text[max(0, i - 3):i] != 'GA(':
                    yield i, v
            i = j
            continue
        i += 1


def rewritten_ranges():
    """the retail ranges the hybrid's variant does not keep byte for byte (addrmap rows other than code_linear,
    data and the like): a return address there has no exact retail address, so gr() of one cannot equal a
    `retail-address` label"""
    amap = REPO / 'build' / 'decomp-wc' / 'out' / 'TM2_SOURCE_PORT' / 'addrmap.tsv'
    if not amap.exists():
        return None
    out = []
    for line in amap.read_text(encoding='utf-8').splitlines():
        f = line.split('\t')
        if len(f) >= 5 and not line.startswith('#') and f[0] != 'r_start' and f[4] == 'code_entry':
            out.append((int(f[0], 16), int(f[1], 16), f[5] if len(f) > 5 else ''))
    return out


def main():
    found = 0
    rewritten = rewritten_ranges()
    for d in DIRS:
        for f in sorted((REPO / d).glob('*.[ch]')):
            rel = f.relative_to(REPO).as_posix()
            if rel in SKIP:
                continue
            text = f.read_text(encoding='utf-8')
            lines = text.splitlines()
            for off, v in literals(text):
                line = text.count('\n', 0, off) + 1
                if 'retail-address' in lines[line - 1]:
                    hit = [r for r in rewritten or () if r[0] <= v < r[1] and 'case 0x' in lines[line - 1]]
                    if hit:
                        print('%s:%d: 0x%08X is in %s, which the hybrid rewrites: gr() of a return address there '
                              'is not exact' % (rel, line, v, hit[0][2]))
                        found += 1
                    continue
                print('%s:%d: 0x%08X not in GA(): %s' % (rel, line, v, lines[line - 1].strip()[:100]))
                found += 1
    print('%d untranslated address%s' % (found, '' if found == 1 else 'es'))
    sys.exit(1 if found else 0)


if __name__ == '__main__':
    main()
