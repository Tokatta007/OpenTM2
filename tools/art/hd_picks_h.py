"""The PC version's sharper battle textures the game uses (src/assets/hd_picks.h): each a PS1 texture (its pack, its
index) and its twin in the PC version (pack, index), the same picture at twice the size. Found by hd_audit (shape,
color and see-through alike), every pair then checked by eye; the PC's changed ones (effects, new details) left out:
the PS1's stay. tools/art/hd_picks.txt is the list.

    python tools/art/hd_picks_h.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
picks = []
for line in (ROOT / 'tools/art/hd_picks.txt').read_text(encoding='utf-8').splitlines():
    line = line.split('#')[0].strip()
    if not line:
        continue
    a, i, b, j = line.split(':')
    picks.append((a, int(i), b, int(j)))
out = ['/* Made by tools/art/hd_picks_h.py from tools/art/hd_picks.txt: the PC battle textures used (a PS1 pack and index,',
       ' * its PC twin\'s pack and index: the same picture at twice the size). */',
       '#pragma once',
       'static const struct { const char *ps1; int i; const char *pc; int j; } HD_PICKS[] = {']
out += ['    {"%s", %d, "%s", %d},' % p for p in picks]
out.append('};')
(ROOT / 'src/assets/hd_picks.h').write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
print(len(picks), 'picks')
