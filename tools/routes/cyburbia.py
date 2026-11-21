"""Cyburbia (game level 11) computer-car route for the PC port: our own data.

Writes src/vplus/cyburbia_route.inc (C arrays and the write function for src/vplus/secret_routes.c);
--json also writes route.json (everything, for a validator and a plot).

Draft source: the Singletrac fan mod's 173-node route (routes/cybnav*), used only for where the roads
are. Changes: parked/unreachable nodes dropped (54 71 72 75 81 82 at -8000,-8000 and 26), the doubled
x=9600 / x=19200 trunks (5 6 7 8) dropped, fork stubs (110 113 116 119 122 125 128 131) and the extra
curve points (corners, wiggles, 0, 18, 154) thinned, one-way slips 46->45 and 57->56 made two-way, the
right side's 18/33 pair merged, the fork stubs replaced by one junction per fork, three corners and the
tunnel-mouth points moved off walls; the plaza's two north entries stay one-way as the mod had them
(README); our own 35 sections (the plaza = retail's section 0 box, 24 street rectangles found by
opt_sections.py, 10 ring pieces) and our own next-node / next-section tables, checked by val.py.

Node layout in guest memory (aiRouteNodes 801B1DE8, 24 bytes; ai_route_nodes.c TM2RouteNode):
  +0 x s32, +4 y s32, +8 z s16, +0x0A next[4] s16, +0x12 lane[4] s8, +0x16 count s8, +0x17 route s8
"""
import json, math, heapq, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- nodes: name -> (x, y, class)
# class: 2 straight/intersection, 1 corner or bridge end, 0 fork branch or wiggle (sets edge speeds)
N = {}
def node(name, x, y, cls=2): N[name] = (x, y, cls)

# outer ring (the mod's ids in comments)
node('c_tl', -2024, 16112, 1)   # 172
# t3 / t5 / b3 / b5: the mod's 15 16 21 22 sat on the tunnel mouths (a portal face at x 11520 / 17280
# over the middle stretch); 200 out from them
for nm, x in (('t1', 3200), ('t2', 7680), ('t3', 11320), ('t4', 14400), ('t5', 17480), ('t6', 21120), ('t7', 24800)):
    node(nm, x, 16320)          # 135 14 15 34 16 17 136
node('c_tr', 30848, 16096, 1)   # 138
node('r1', 31056, 12800, 1)     # 140 (bridge end)
node('r2', 31056, 7424, 1)      # 18 (33 merged: bridge end)
node('r3', 32488, 6000, 0)      # 144 (wiggle)
node('r4', 32488, 4848, 0)      # 145
node('r5', 31056, 3408, 1)      # 149
node('r6', 31056, 1400)         # 150
node('c_br', 30832, -1696, 1)   # 152
for nm, x in (('b7', 24200), ('b6', 21120), ('b5', 17480), ('b4', 14400), ('b3', 11320), ('b2', 7680), ('b1', 3200)):
    node(nm, x, -1920)          # 155 19 20 35 21 22 156
node('c_bl', -2048, -1696, 1)   # 158
node('l6', -2256, 1200)         # 160
node('l5', -2256, 3408, 1)      # 161
node('l4', -3680, 4832, 0)      # 165 (wiggle)
node('l3', -3680, 5984, 0)      # 166
node('l2', -2256, 7440, 1)      # 23 (bridge end)
node('l1', -2256, 12800, 1)     # 170 (bridge end)
# the four Y forks between the street grid and the ring: a junction on the x 9600 / 19200 line just
# off the grid (the mod's fork stubs 110/113 .. at y 14640 / -240), then one point a branch (its 111,
# 114, 117, 120, 123, 126, 129, 132; the second arc points 112.. are within 100 of the chord)
for nm, x, y in (('j_nw', 9600, 14650), ('j_ne', 19200, 14650), ('j_sw', 9600, -250), ('j_se', 19200, -250)):
    node(nm, x, y, 1)
for nm, x, y in (('fa', 9104, 15360), ('fb', 10096, 15360), ('fc', 18704, 15360), ('fd', 19696, 15360),
                 ('fe', 9104, -960), ('ff', 10096, -960), ('fg', 18704, -960), ('fh', 19696, -960)):
    node(nm, x, y, 0)
# street grid: name g<X>_<Y> in 2400 steps; edge rows/columns at 96 / 14304 / 28704 like the mod
GX = {0: 96, 1: 2400, 2: 4800, 3: 7200, 4: 9600, 5: 12000, 6: 14400, 7: 16800, 8: 19200, 9: 21600, 10: 24000, 11: 26400, 12: 28704}
GY = {0: 96, 1: 2400, 2: 4800, 3: 7200, 4: 9600, 5: 12000, 6: 14304}
def g(i, j): return 'g%d_%d' % (i, j)
present = set()
for i in range(0, 5):                       # west grid: every crossing but (7200, 14304)
    for j in range(7):
        if (i, j) != (3, 6): present.add((i, j))
for i in (5, 6, 7):                         # middle: the bottom street and the block north of the plaza
    present.add((i, 0)); present.update((i, j) for j in (4, 5, 6))
present.add((8, 0)); present.update((8, j) for j in (4, 5, 6))
present.update((9, j) for j in (0, 4, 5, 6))
for i in (10, 11, 12):
    present.update((i, j) for j in range(7))
for (i, j) in sorted(present):
    node(g(i, j), GX[i], GY[j])
# three grid corners 100 in from a corner wall the level geometry has right on the mod's points
node(g(0, 6), 196, 14204); node(g(12, 6), 28604, 14204); node(g(12, 0), 28604, 196)
# the plaza (retail section 0's box): a ring inside the raised rim
for nm, x, y in (('p_w', 10920, 4800), ('p_nw', 11200, 8080), ('p_n1', 14400, 8240), ('p_n2', 19200, 7680),
                 ('p_ne', 22320, 8080), ('p_e', 22640, 4800), ('p_se', 22400, 1440), ('p_s2', 19200, 1920),
                 ('p_s1', 14400, 1880), ('p_sw', 11200, 1440)):
    node(nm, x, y)

# ---------------------------------------------------------------- edges
TWO = []   # two-way
ONE = []   # one-way (from, to)
def chain(*names):
    for a, b in zip(names, names[1:]): TWO.append((a, b))
chain('c_tl', 't1', 't2', 't3', 't4', 't5', 't6', 't7', 'c_tr', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'c_br',
      'b7', 'b6', 'b5', 'b4', 'b3', 'b2', 'b1', 'c_bl', 'l6', 'l5', 'l4', 'l3', 'l2', 'l1', 'c_tl')
chain(g(4, 6), 'j_nw'); chain('j_nw', 'fa', 't2'); chain('j_nw', 'fb', 't3')
chain(g(8, 6), 'j_ne'); chain('j_ne', 'fc', 't5'); chain('j_ne', 'fd', 't6')
chain(g(4, 0), 'j_sw'); chain('j_sw', 'fe', 'b2'); chain('j_sw', 'ff', 'b3')
chain(g(8, 0), 'j_se'); chain('j_se', 'fg', 'b5'); chain('j_se', 'fh', 'b6')
# west grid (the mod's links; its slips 46->45 and 57->56 made two-way)
for i in (0, 2, 4):
    chain(*[g(i, j) for j in range(7)])            # x 96, 4800, 9600: whole columns
chain(g(1, 0), g(1, 1), g(1, 2)); chain(g(1, 4), g(1, 5), g(1, 6))    # x 2400
chain(g(3, 2), g(3, 3), g(3, 4))                                      # x 7200
chain(*[g(i, 0) for i in range(13)])                                  # y 96: the whole bottom street
chain(g(2, 1), g(3, 1), g(4, 1))                                      # y 2400 west
chain(*[g(i, 2) for i in range(5)])                                   # y 4800 west
chain(g(0, 3), g(1, 3), g(2, 3))                                      # y 7200 west
chain(*[g(i, 4) for i in range(13)])                                  # y 9600: the whole street
chain(g(2, 5), g(3, 5), g(4, 5))                                      # y 12000 west
chain(g(0, 6), g(1, 6), g(2, 6))                                      # y 14304 west
# middle block north of the plaza
chain(g(4, 6), g(5, 6), g(6, 6), g(7, 6), g(8, 6))                   # y 14304
chain(g(6, 5), g(7, 5), g(8, 5))                                      # y 12000
chain(g(5, 4), g(5, 5), g(5, 6)); chain(g(6, 4), g(6, 5), g(6, 6))   # x 12000, 14400
# east grid
chain(*[g(8, j) for j in (4, 5, 6)])                                  # x 19200
chain(g(9, 4), g(9, 5), g(9, 6))                                      # x 21600
chain(*[g(10, j) for j in range(7)]); chain(*[g(12, j) for j in range(7)])   # x 24000, 28704
chain(g(11, 2), g(11, 3), g(11, 4))                                   # x 26400
chain(g(10, 1), g(11, 1), g(12, 1))                                   # y 2400 east
chain(g(10, 2), g(11, 2), g(12, 2))                                   # y 4800 east
chain(g(10, 5), g(11, 5), g(12, 5))                                   # y 12000 east
chain(*[g(i, 6) for i in range(8, 13)])                               # y 14304 east
# plaza ring and its entries
chain('p_w', 'p_nw', 'p_n1', 'p_n2', 'p_ne', 'p_e', 'p_se', 'p_s2', 'p_s1', 'p_sw', 'p_w')
# no link g8_0 <-> p_s2 (the mod's 83 -> 8 -> 106): the chute at x 19200 is a jump ramp aimed at a walled
# pocket (x 18750..19600, y 1100..1600: lane walls each side, a bar on top); cars taking it in game (cyb16.log)
# land in the pocket and stay, and cars going p_s2 -> g8_0 stall against the bar. The plaza's south side
# is entered by the x 14400 chute (g6_0 <-> p_s1) only.
TWO += [(g(4, 2), 'p_w'), (g(10, 2), 'p_e'), (g(6, 0), 'p_s1')]
ONE += [(g(6, 4), 'p_n1'), (g(8, 4), 'p_n2')]      # north entries: in only (the mod's gates; README)

# ---------------------------------------------------------------- successors (name level; the order is
# the order written to the game, and it breaks aiPickLinkNode's distance ties: first strictly nearer wins)
SUCC = {n: [] for n in N}
for _a, _b in TWO:
    SUCC[_a].append(_b); SUCC[_b].append(_a)
for _a, _b in ONE:
    SUCC[_a].append(_b)
for _n in N:   # by direction: east, north, west, south
    SUCC[_n].sort(key=lambda m: math.atan2(N[m][1] - N[_n][1], N[m][0] - N[_n][0]) % (2 * math.pi))

def pick_name(cur, prev, rep, aim):
    """aiPickLinkNode 800F7B00 on names (f001 < 2, no section filter: hasDrop 0)."""
    best = SUCC[cur][0]; bd = 0x7FFFFFFF
    for s in SUCC[cur]:
        d = (aim[0] - N[s][0]) ** 2 + (aim[1] - N[s][1]) ** 2
        if d < bd:
            if s != prev: best, bd = s, d
            elif rep < 3: best, bd = s, d + 80
    return best, (rep + 1 if best == prev else 0)

def walk_inside(members, u, v, maxs=40):
    """the car's walk from u towards v's position stays in the node set and reaches v"""
    cur, prev, rep = u, None, 0
    for _ in range(maxs):
        if cur == v: return True
        nn, rep = pick_name(cur, prev, rep, N[v][:2])
        if nn not in members: return False
        prev, cur = cur, nn
    return cur == v

def greedy_convex(members):
    return all(walk_inside(members, u, v) for u in members for v in members if u != v)

# ---------------------------------------------------------------- sections (priority order = index)
BIG = 40000
PLAZA = ('plaza', 9888, 288, 23712, 9312)   # = retail CYB.GRP section 0 exactly (surfaces 0..39)
RING = [  # the outer road: chains, greedy-safe as they are
    ('ring_nw', -BIG, 15000, 9600, BIG), ('ring_n', 9600, 15000, 19200, BIG), ('ring_ne', 19200, 15000, BIG, BIG),
    ('ring_sw', -BIG, -BIG, 9600, -700), ('ring_s', 9600, -BIG, 19200, -700), ('ring_se', 19200, -BIG, BIG, -700),
    ('ring_w_n', -BIG, 6700, -1000, 15000), ('ring_w_s', -BIG, -700, -1000, 6700),   # the sides in halves: one
    ('ring_e_n', 29800, 6700, BIG, 15000),                                             # exit each way
    ('ring_e_s', 29800, -700, BIG, 6700),    # last: also the fallback for any point outside every box
]
# the street area between the ring and the plaza: cells around the 2400 grid, merged into rectangles
SPAN = int(os.environ.get('CYB_SPAN', 4900))   # a section's nodes span at most this (x and y)
XC = [-1000, 1248, 3600, 6000, 8400, 10800, 13200, 15600, 18000, 20400, 22800, 25200, 27552, 29800]
YC = [-700, 1248, 3600, 6000, 8400, 10800, 13152, 15000]

def in_box(x, y, b): return b[1] <= x <= b[3] and b[2] <= y <= b[4]

def grid_partition():
    """Cover the 13 x 7 cells with few rectangles whose nodes are greedy-convex (a computer car walking
    from any of their nodes towards any other gets there without leaving the set). Largest first."""
    cells = {}
    for n, (x, y, c) in N.items():
        if in_box(x, y, PLAZA) or any(in_box(x, y, r) for r in RING): continue
        i = max(k for k in range(13) if XC[k] <= x); j = max(k for k in range(7) if YC[k] <= y)
        cells.setdefault((i, j), []).append(n)
    cand = []
    for i0 in range(13):
        for i1 in range(i0, 13):
            for j0 in range(7):
                for j1 in range(j0, 7):
                    mem = [n for i in range(i0, i1 + 1) for j in range(j0, j1 + 1) for n in cells.get((i, j), [])]
                    if not mem: continue
                    xs = [N[n][0] for n in mem]; ys = [N[n][1] for n in mem]
                    if max(xs) - min(xs) > SPAN or max(ys) - min(ys) > SPAN: continue   # compact: one exit fits all
                    cand.append(((i1 - i0 + 1) * (j1 - j0 + 1), len(mem), i0, i1, j0, j1, mem))
    cand.sort(key=lambda c: (-c[1], -c[0], c[2], c[4]))
    covered = set(); chosen = []
    for area, cnt, i0, i1, j0, j1, mem in cand:
        cs = {(i, j) for i in range(i0, i1 + 1) for j in range(j0, j1 + 1)}
        if cs & covered: continue
        if not greedy_convex(set(mem)): continue
        chosen.append((i0, i1, j0, j1)); covered |= cs
    inside_plaza = {(i, j) for i in range(13) for j in range(7)
                    if PLAZA[1] <= XC[i] and XC[i + 1] <= PLAZA[3] and PLAZA[2] <= YC[j] and YC[j + 1] <= PLAZA[4]}
    left = {(i, j) for i in range(13) for j in range(7)} - covered - inside_plaza
    if left: raise SystemExit('cells not covered: %s' % sorted(left))
    out = []
    for i0, i1, j0, j1 in sorted(chosen, key=lambda r: (r[2], r[0])):
        out.append(('st_%d_%d' % (i0, j0), XC[i0], YC[j0], XC[i1 + 1], YC[j1 + 1]))
    return out

# The street sections: rectangles of cells (i0, i1, j0, j1) found by opt_sections.py (start: the finest
# greedy-convex cover, then the merges that shorten the computer cars' route walks most, down to 35).
GRID_RECTS = [(0, 1, 0, 3), (2, 2, 0, 1), (3, 3, 0, 0), (4, 4, 0, 0), (5, 7, 0, 0), (8, 8, 0, 0), (9, 12, 0, 1),
              (3, 3, 1, 1), (4, 8, 1, 2), (2, 3, 2, 4), (9, 11, 2, 2), (12, 12, 2, 4), (4, 5, 3, 4), (6, 7, 3, 4),
              (8, 9, 3, 4), (10, 11, 3, 4), (0, 1, 4, 6), (2, 3, 5, 6), (4, 4, 5, 6), (5, 6, 5, 6), (7, 8, 5, 5),
              (9, 12, 5, 6), (7, 7, 6, 6), (8, 8, 6, 6)]
def grid_sections(rects):
    return [('st_%d_%d' % (i0, j0), XC[i0], YC[j0], XC[i1 + 1], YC[j1 + 1]) for i0, i1, j0, j1 in rects]
if os.environ.get('CYB_SEARCH'):
    SECTIONS = [PLAZA] + grid_partition() + RING
else:
    SECTIONS = [PLAZA] + grid_sections(GRID_RECTS) + RING
PLAZA_SURF = (0, 39)   # retail: section 0 probes surfaces 0..39 (the plaza's rim and ramps)
OTHER_SURF = (40, 45)  # retail: section 1 (everything else) probes 40..45 (the side bridges)

def section_at(x, y, z=0):
    """worldSectionAt 800EFF34 (car_wheel_sway.c) for our boxes: rot 0, shape 0, floor 0."""
    r = len(SECTIONS) - 1
    for i, (nm, x0, y0, x1, y1) in enumerate(SECTIONS):
        dx, dz = x - x0, y - y0
        if dx >= 0 and dz >= 0 and (x1 - x0) >= dx and (y1 - y0) >= dz:
            r = i
            if 0 < z + 80: return i
    return r

# ---------------------------------------------------------------- start points (ids 0..14)
# spread over the map, on straights or crossings; node 0 in a section < 4 (Dark Tooth's Hong Kong warp
# test in aiFollowRouteTarget fires at node 0 when route >= 4 and the warps are on).
START = [g(1, 2), g(10, 5), 't1', 'b7', g(10, 0), g(2, 5), 'r1', 'l6', g(6, 5), 'p_s1',
         g(11, 2), g(0, 0), g(12, 6), 'p_ne', g(4, 3)]

# ---------------------------------------------------------------- build
def build():
    names = list(N)
    sec = {n: section_at(N[n][0], N[n][1]) for n in names}
    rest = sorted((n for n in names if n not in START), key=lambda n: (sec[n], N[n][1], N[n][0]))
    order = START + rest
    idx = {n: i for i, n in enumerate(order)}
    succ = SUCC
    nodes = []
    for n in order:
        x, y, c = N[n]
        lanes = [min(c, N[m][2]) for m in succ[n]]
        nodes.append(dict(id=idx[n], name=n, x=x, y=y, z=0, succ=[idx[m] for m in succ[n]], lane=lanes, section=sec[n]))
    return nodes

def dist(a, b): return math.hypot(a['x'] - b['x'], a['y'] - b['y'])

def pick_link(nodes, cur, prev, rep, aim, f001=0):
    """aiPickLinkNode 800F7B00 (ai_link_node_pick.c), f001 < 2, no section filter (hasDrop 0)."""
    n = nodes[cur]; best = n['succ'][0]; bd = 0x7FFFFFFF
    for s in n['succ']:
        d = (aim[0] - nodes[s]['x']) ** 2 + (aim[1] - nodes[s]['y']) ** 2
        if d < bd:
            if s != prev: best, bd = s, d
            elif rep < 3: best, bd = s, d + 80
    rep = rep + 1 if best == prev else 0
    return best, rep

def simulate(nodes, NEXT, NSEC, start, goal, maxsteps=200, rng_pick=0):
    """The route walk of a computer car (aiFollowRouteTarget 800F7D54 + aiPickLinkNode) towards a target
    standing on node goal; personality f001 < 2. rng_pick 0/1: which of several follow-section successors
    the exit node takes (retail breaks on the first match when GsGetWorkBase_0()&1, else keeps the last)."""
    cur, prev, rep, length = start, -1, 0, 0.0
    tsec = nodes[goal]['section']; tpos = (nodes[goal]['x'], nodes[goal]['y'])
    path = [cur]
    for _ in range(maxsteps):
        if cur == goal: return True, length, path
        route = nodes[cur]['section']
        nxt = NEXT[route][tsec]; follow = NSEC[route][tsec]
        if tsec == route or nxt < 0:
            aim = tpos
        else:
            if nxt == cur:
                cands = [s for s in nodes[cur]['succ'] if nodes[s]['section'] == follow]
                best = nodes[cur]['succ'][0] if not cands else (cands[0] if rng_pick else cands[-1])
                aim = (nodes[best]['x'], nodes[best]['y'])
            else:
                aim = (nodes[nxt]['x'], nodes[nxt]['y'])
        nn, rep = pick_link(nodes, cur, prev, rep, aim)
        length += dist(nodes[cur], nodes[nn]); prev, cur = cur, nn; path.append(cur)
    return cur == goal, length, path

def dijkstra(nodes, sources):
    D = {s: 0.0 for s in sources}; h = [(0.0, s) for s in sources]
    pred = {i: [] for i in range(len(nodes))}
    for n in nodes:
        for s in n['succ']: pred[s].append(n['id'])
    while h:   # distances TO the sources (walk the reversed edges)
        d, u = heapq.heappop(h)
        if d > D.get(u, 1e18): continue
        for p in pred[u]:
            nd = d + dist(nodes[p], nodes[u])
            if nd < D.get(p, 1e18): D[p] = nd; heapq.heappush(h, (nd, p))
    return D

def greedy_len(nodes, start, goal, maxsteps=60):
    cur, prev, rep, L = start, -1, 0, 0.0
    for _ in range(maxsteps):
        if cur == goal: return L
        nn, rep = pick_link(nodes, cur, prev, rep, (nodes[goal]['x'], nodes[goal]['y']))
        L += dist(nodes[cur], nodes[nn]); prev, cur = cur, nn
    return None

def tables(nodes):
    """next-node / next-section [S][T]: per pair, the exit node e of S and the section F after it that
    minimise (mean greedy walk inside S to e) + |e f| + (shortest route from f into T); f is every
    successor of e in F (the game picks one of them), the worst counts."""
    S = len(SECTIONS)
    members = {s: [n['id'] for n in nodes if n['section'] == s] for s in range(S)}
    toT = {t: dijkstra(nodes, members[t]) for t in range(S)}
    NEXT = [[-1] * S for _ in range(S)]; NSEC = [[s] * S for s in range(S)]
    for s in range(S):
        # the mean greedy walk from the section's nodes to each of its exit nodes
        exits = {}
        for e in members[s]:
            outs = [f for f in nodes[e]['succ'] if nodes[f]['section'] != s]
            if not outs: continue
            ls = [greedy_len(nodes, u, e) for u in members[s]]
            if any(l is None for l in ls): continue          # not reachable by the walk: never an exit
            exits[e] = sum(ls) / len(ls)
        for t in range(S):
            if t == s: continue
            best = None
            for e, a in exits.items():
                byF = {}
                for f in nodes[e]['succ']:
                    F = nodes[f]['section']
                    if F == s: continue
                    byF.setdefault(F, []).append(dist(nodes[e], nodes[f]) + toT[t].get(f, 1e18))
                for F, cs in byF.items():
                    c = a + max(cs)
                    if best is None or c < best[0]: best = (c, e, F)
            if best: NEXT[s][t], NSEC[s][t] = best[1], best[2]
    return NEXT, NSEC

def fix_loops(nodes, NEXT, NSEC):
    """Section-level consistency: following NSEC from S must reach T. Where it cycles, re-pick S's exit
    with the shortest-path successor section only (rare)."""
    S = len(SECTIONS); changed = True; rounds = 0
    while changed and rounds < 10:
        changed = False; rounds += 1
        for s in range(S):
            for t in range(S):
                seen = set(); c = s
                while c != t and c not in seen and NEXT[c][t] >= 0:
                    seen.add(c); c = NSEC[c][t]
                if c != t:
                    print('section loop', SECTIONS[s][0], '->', SECTIONS[t][0], file=sys.stderr)
    return NEXT, NSEC

def main():
    nodes = build()
    NEXT, NSEC = tables(nodes)
    fix_loops(nodes, NEXT, NSEC)
    S = len(SECTIONS)
    sections = []
    for i, (nm, x0, y0, x1, y1) in enumerate(SECTIONS):
        first, last = PLAZA_SURF if nm == 'plaza' else OTHER_SURF
        sections.append(dict(index=i, name=nm, x=x0, y=y0, w=x1 - x0, l=y1 - y0, floor=0, sn=0, cs=4096,
                             rot=0, has_floor=1, has_drop=0, shape=0, ground=0, follow_range=0x57,
                             first_surface=first, last_surface=last))
    out = dict(level=11, node_count=len(nodes), section_count=S, nodes=nodes, sections=sections,
               start_nodes=list(range(len(START))), next_node=NEXT, next_section=NSEC)
    if '--json' in sys.argv:  # (for a validator or a plot: route.json beside this script)
        json.dump(out, open(os.path.join(HERE, 'route.json'), 'w'), indent=1)
    write_c(out)
    print('nodes', len(nodes), 'sections', S, 'start points', len(START))

def write_c(R):
    nodes, secs, S = R['nodes'], R['sections'], R['section_count']
    L = []
    w = L.append
    w('/* Cyburbia (level 11) computer-car route: generated by tools/routes/cyburbia.py, do not edit by hand.')
    w(' * %d route points (aiRouteNodes 801B1DE8, 140 at most), %d sections (GRP at 801A9848), %d start points.' % (len(nodes), S, len(R['start_nodes'])))
    w(' * Written after arenaLoadCyburbia (80119F94) has loaded CYB.PTS / CYB.GRP / CYB.RMP. */')
    w('#define CYB_NODES %d' % len(nodes))
    w('#define CYB_SECTIONS %d' % S)
    w('#define CYB_STARTS %d' % len(R['start_nodes']))
    w('typedef struct { int32_t x, y; int16_t z; int16_t next[4]; int8_t lane[4]; uint8_t count, section; } CybNode;')
    w('static const CybNode cyb_nodes[CYB_NODES] = {')
    for n in nodes:
        nx = n['succ'] + [0] * (4 - len(n['succ'])); ln = n['lane'] + [0] * (4 - len(n['lane']))
        w('    {%6d, %6d, %d, {%3d, %3d, %3d, %3d}, {%d, %d, %d, %d}, %d, %2d}, /* %3d %s */' % (
            n['x'], n['y'], n['z'], *nx, *ln, len(n['succ']), n['section'], n['id'], n['name']))
    w('};')
    w('/* sections: box x, y (lower corner), w, l; floor 0, hasFloor 1, hasDrop 0, shape 0, rot 0, ground 0 */')
    w('typedef struct { int32_t x, y, w, l; uint8_t first_surface, last_surface; } CybSection;')
    w('static const CybSection cyb_sections[CYB_SECTIONS] = {')
    for s in secs:
        w('    {%6d, %6d, %6d, %6d, %2d, %2d}, /* %2d %s */' % (s['x'], s['y'], s['w'], s['l'], s['first_surface'], s['last_surface'], s['index'], s['name']))
    w('};')
    w('/* [my section][target section]: the exit point in my section, -1 on the diagonal (chase the target) */')
    w('static const int8_t cyb_next_node[CYB_SECTIONS][CYB_SECTIONS] = {')
    for r in R['next_node']: w('    {' + ', '.join('%3d' % v for v in r) + '},')
    w('};')
    w('/* [my section][target section]: the section after mine on the way (mine on the diagonal) */')
    w('static const int8_t cyb_next_section[CYB_SECTIONS][CYB_SECTIONS] = {')
    for r in R['next_section']: w('    {' + ', '.join('%2d' % v for v in r) + '},')
    w('};')
    w('static const int16_t cyb_starts[CYB_STARTS] = {' + ', '.join(str(v) for v in R['start_nodes']) + '};')
    w('')
    w('#define CYB_GRP GA(0x801A9848u)              /* worldSectionGrpFile, 6036 bytes */')
    w('static void cyb_route_write(void)')
    w('{')
    w('    CPU *cpu = &M.cpu;')
    w('    /* route points: the whole 140-point array (unused ones zero), then the count */')
    w('    for (uint32_t i = 0; i < 140; i++) {')
    w('        uint32_t a = GA(0x801B1DE8u) + 24u * i;')
    w('        for (uint32_t k = 0; k < 24; k += 4) mem_w32(cpu, a + k, 0);')
    w('        if (i >= CYB_NODES) continue;')
    w('        const CybNode *n = &cyb_nodes[i];')
    w('        mem_w32(cpu, a + 0x00, (uint32_t)n->x);')
    w('        mem_w32(cpu, a + 0x04, (uint32_t)n->y);')
    w('        mem_w16(cpu, a + 0x08, (uint16_t)n->z);')
    w('        for (uint32_t k = 0; k < 4; k++) {')
    w('            mem_w16(cpu, a + 0x0A + 2u * k, (uint16_t)n->next[k]);')
    w('            mem_w8(cpu, a + 0x12 + k, (uint8_t)n->lane[k]);')
    w('        }')
    w('        mem_w8(cpu, a + 0x16, n->count);')
    w('        mem_w8(cpu, a + 0x17, n->section);')
    w('    }')
    w('    mem_w32(cpu, GA(0x80180AD8u), CYB_NODES);   /* aiRouteNodeCount */')
    w('    /* start points: startNodeList s16[15] 801B2B28, taken flags u8[15] 801B2B46, count 801B2B55 */')
    w('    for (uint32_t i = 0; i < 15; i++) {')
    w('        mem_w16(cpu, GA(0x801B2B28u) + 2u * i, (uint16_t)(i < CYB_STARTS ? cyb_starts[i] : 0));')
    w('        mem_w8(cpu, GA(0x801B2B46u) + i, 0);')
    w('    }')
    w('    mem_w8(cpu, GA(0x801B2B55u), CYB_STARTS);')
    w('    /* sections: all 35 slots rewritten (unused ones zero, like the retail file past its count) */')
    w('    for (uint32_t s = 0; s < 35; s++) {')
    w('        int u = s < CYB_SECTIONS;')
    w('        const CybSection *c = &cyb_sections[u ? s : 0];')
    w('        mem_w32(cpu, CYB_GRP + 0x0C + 12u * s, u ? (uint32_t)c->x : 0);       /* box x */')
    w('        mem_w32(cpu, CYB_GRP + 0x10 + 12u * s, u ? (uint32_t)c->y : 0);       /* box y */')
    w('        mem_w32(cpu, CYB_GRP + 0x14 + 12u * s, 0);                             /* floor height */')
    w('        mem_w32(cpu, CYB_GRP + 0x1B0 + 4u * s, u ? (uint32_t)c->w : 0);        /* width (x) */')
    w('        mem_w32(cpu, CYB_GRP + 0x23C + 4u * s, u ? (uint32_t)c->l : 0);        /* depth (y) */')
    w('        mem_w16(cpu, CYB_GRP + 0x2C8 + 2u * s, 0);                             /* sn */')
    w('        mem_w16(cpu, CYB_GRP + 0x30E + 2u * s, u ? 4096 : 0);                  /* cs */')
    w('        mem_w8(cpu, CYB_GRP + 0x11AF + s, u ? 0x57 : 0);                      /* aiRouteFollowRanges */')
    w('        mem_w8(cpu, CYB_GRP + 0x11D2 + s, 0);                                  /* rotated */')
    w('        mem_w8(cpu, CYB_GRP + 0x11F5 + s, u ? 1 : 0);                          /* hasFloor */')
    w('        mem_w8(cpu, CYB_GRP + 0x1218 + s, 0);                                  /* hasDrop */')
    w('        mem_w8(cpu, CYB_GRP + 0x123B + s, 0);                                  /* shape: box */')
    w('        mem_w8(cpu, CYB_GRP + 0x125E + s, 0);                                  /* ground class */')
    w('        mem_w8(cpu, CYB_GRP + 0x174A + s, u ? c->first_surface : 0);           /* first surface */')
    w('        mem_w8(cpu, CYB_GRP + 0x176D + s, u ? c->last_surface : 0);            /* last surface */')
    w('        for (uint32_t t = 0; t < 35; t++) {')
    w('            int v = u && t < CYB_SECTIONS;')
    w('            mem_w8(cpu, CYB_GRP + 0x354 + 35u * s + t, v ? (uint8_t)cyb_next_node[s][t] : 0xFF);   /* next node */')
    w('            mem_w8(cpu, CYB_GRP + 0x81D + 35u * s + t, v ? (uint8_t)cyb_next_section[s][t] : 0);  /* next section */')
    w('            mem_w8(cpu, CYB_GRP + 0xCE6 + 35u * s + t, 0);                     /* link table: off */')
    w('            mem_w8(cpu, CYB_GRP + 0x1281 + 35u * s + t, 0);                    /* speed limit: none */')
    w('        }')
    w('    }')
    w('    mem_w8(cpu, CYB_GRP + 0x1790, CYB_SECTIONS);    /* the count worldSectionAt reads */')
    w('    mem_w32(cpu, GA(0x80180AD4u), CYB_SECTIONS);    /* terrainSectionCount (written by the loader, not read) */')
    w('}')
    open(os.path.join(HERE, '..', '..', 'src', 'vplus', 'cyburbia_route.inc'), 'w', newline='\n').write('\n'.join(L) + '\n')

if __name__ == '__main__':
    main()
