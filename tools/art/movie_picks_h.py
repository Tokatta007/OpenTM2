"""tools/art/movie_portraits.json as a C table for the first-time setup (src/assets/movie_picks.h): the picks are the
port's own data (which frames, where to crop), not the game's art.

    python tools/art/movie_picks_h.py
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
picks = json.loads((ROOT / 'tools/art/movie_portraits.json').read_text(encoding='utf-8'))
lines = ['/* Made by tools/art/movie_picks_h.py from tools/art/movie_portraits.json: the movie portraits\' picks (id, who, movie,',
         ' * frame, x, gain; an image pick: frame -1). */',
         '#pragma once',
         'static const struct { const char *id, *who, *movie; int frame, x; double gain; } MOVIE_PICKS[] = {']
for p in picks:
    lines.append('    {"%s", "%s", "%s", %d, %d, %r},' % (p['id'], p['who'], p['movie'], -1 if p.get('image') else p['frame'],
                                                       p.get('x', 0), float(p.get('gain', 1.0))))
lines.append('};')
(ROOT / 'src/assets/movie_picks.h').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
print(len(picks), 'picks')
