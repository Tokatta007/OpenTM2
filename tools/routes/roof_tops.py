"""Roof Tops (level 12, SROOF.*) computer-car route generator.

Writes src/vplus/roof_tops_route.inc (C arrays plus `sroof_route_write()`); --json also roof_tops_route.json
(everything, for a validator or a plot). Needs the disc's extracted TERRAIN folder in TM2_TERRAIN.

What it keeps from retail SROOF.GRP: all 14 section boxes, floors, flags and surface ranges (untouched, the
wheels depend on them), the follow ranges and the link table (all 0, as in New York). What it writes: the
route points (retail's 9 start points stay points 0..8; 0 and 7 moved, see MOVED_STARTS), the next-node / next-section tables for
sections 0..12 (row and column 13, the street catch-all, stay retail's -1 / 0) and a few speed-table
entries (see SPEED below).

Coordinates: x, y on the ground, z up (worldSectionAt takes the first box whose floor is below z+80)."""
import json, math, heapq, os, sys, struct
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from roof_tops_grp import GRP, load, rmp

RG = GRP(load('SROOF', 'GRP'))
NSEC_USED = 13            # sections 0..12 get route tables; 13 is the street (floor 0, everything outside)
SEC_NAMES = ['A west', 'A east', 'A south', 'platform P', 'platform Q', 'B', 'C', 'pit', 'D', 'D-box 7400',
             'E', 'F', 'G', 'street']

# ------------------------------------------------------------------ points
# (name, x, y, z). z = the section floor, or the RMP ramp height for points on a ramp.
P = [
    # retail's 9 start points, ids 0..8, unmoved (SROOF.PTS)
    ('s0_F', 14600, 1000, 7760), ('s1_E', 9400, 880, 7520), ('s2_D', 14480, 7120, 8000), ('s3_D', 9600, 7600, 8000),
    ('s4_D', 8000, 4000, 8000), ('s5_D', 12400, 3200, 8000), ('s6_A', 1064, 936, 7760), ('s7_P', 2300, 9520, 7440),
    ('s8_C', 7760, 10040, 7200),
    # A (7760): the U; the courtyard x 1760..3360, y 1680..4880 is a hole to the street
    ('a_w1', 960, 2480, 7760), ('a_w2', 960, 4000, 7760),              # west strip; a_w2: takeoff to B
    ('a_s', 2560, 880, 7760),                                           # south bar
    ('a_e0', 4160, 880, 7760), ('a_e1', 4160, 2440, 7760),             # east strip; a_e0: takeoff to E
    ('a_e2', 4160, 4000, 7760),                                         # landing from D, takeoff to B
    # B (6960) and the ramp up to Q (RMP 0: x 4160 at 6960 -> x 1760 at 7440, y 8320..8720)
    ('b_w', 960, 6400, 6960), ('b_s', 4160, 6400, 6960), ('b_c', 2560, 7000, 6960), ('b_ce', 4160, 7800, 6960),
    ('b_x', 2500, 7800, 6960), ('r_foot', 4560, 8520, 6960),
    # Q and P (7440)
    ('q_e', 1650, 8520, 7440), ('q_n', 1360, 9300, 7440), ('p_mid', 3200, 9520, 7440), ('p_to', 4160, 9520, 7440),   # p_to: takeoff to C
    # C (7200) and its ramp up to D (RMP 1: x 7600..8000, y 9600 at 7200 -> y 6400 at 8000)
    ('c_w', 6400, 9520, 7200), ('c_jw', 6800, 7800, 7200),
    # D (8000), around the pit box (9620..11980 x 4020..6380); nothing at x < 8000, y > 5600 (C's box below)
    ('d_rt', 7800, 5600, 8000), ('d_ja', 6400, 4000, 8000), ('d_w', 9000, 5200, 8000), ('d_s1', 9400, 3200, 8000),
    ('d_s2', 13200, 3200, 8000), ('d_rt3', 14600, 3200, 8000), ('d_s3', 15150, 3200, 8000), ('d_e', 12800, 5200, 8000),
    ('d_fe', 15200, 5200, 8000), ('d_n', 10800, 7200, 8000), ('d_ne2', 12600, 6800, 8000), ('d_nw', 8800, 9700, 8000), ('d_ne', 12400, 9200, 8000),
    ('d_fne', 15200, 9200, 8000),
    # E (7520) and its ramp up to F (RMP 2: y 2000..2400, x 10800 at 7520 -> x 12000 at 7760)
    ('e_w', 6400, 880, 7520), ('e_e', 11200, 880, 7520), ('e_rf', 10200, 2200, 7520),
    # F (7760) and its ramp up to D (RMP 3: x 14400..14800, y 1200 at 7760 -> y 2400 at 8000); G (7760)
    ('f_rt', 12800, 2000, 7760), ('f_w', 12400, 1200, 7760), ('f_c', 13200, 1600, 7760), ('f_e', 14000, 1200, 7760), 
    ('g', 15150, 1100, 7760),
    # the pit (section 7, floor 7400 under the pyramid) and the D box's 7400 level (section 9): the way out
    # retail's tables point at (7 -> 9 -> B at speed 120): out through the opening in D's west wall
    # (DMD: x 5600, y 5950..6250, z 7400..7700) and over the street onto B
    ('k7', 10800, 5200, 7400), ('k9', 8800, 5200, 7400), ('k9w', 6400, 5950, 7400),
]
ID = {p[0]: i for i, p in enumerate(P)}
assert len(ID) == len(P)

# ------------------------------------------------------------------ links (lane per direction)
SUCC = {i: [] for i in range(len(P))}
KIND = {}      # (a, b) -> 'roof' | 'jump' | 'drop' | 'ramp' | 'pit'
def one(a, b, lane, kind='roof'):
    a, b = ID[a], ID[b]
    assert b not in [s for s, l in SUCC[a]], (P[a][0], P[b][0])
    SUCC[a].append((b, lane)); KIND[(a, b)] = kind
    assert len(SUCC[a]) <= 4, P[a][0]
def two(a, b, lane, kind='roof', lane_back=None):
    one(a, b, lane, kind); one(b, a, lane if lane_back is None else lane_back, kind)

# A
two('s6_A', 'a_w1', 2); two('a_w1', 'a_w2', 2); two('s6_A', 'a_s', 1); two('a_s', 'a_e0', 2)
two('a_e0', 'a_e1', 1); two('a_e1', 'a_e2', 2)
one('a_w2', 'b_w', 2, 'jump'); one('a_e2', 'b_s', 2, 'jump'); one('a_e0', 'e_w', 2, 'jump')
# B
two('b_w', 'b_c', 2); two('b_c', 'b_s', 2); two('b_c', 'b_ce', 2); two('b_s', 'b_ce', 1)
two('b_ce', 'r_foot', 0); two('r_foot', 'b_x', 0)
one('r_foot', 'q_e', -1, 'ramp')
two('b_ce', 'b_x', 1); two('b_x', 'b_c', 1); two('b_x', 'b_w', 1)
# Q, P
two('q_e', 'q_n', 1); two('q_n', 's7_P', 1); two('s7_P', 'p_mid', 2); two('p_mid', 'p_to', 2)
one('p_to', 'c_w', 2, 'jump'); one('p_to', 'r_foot', -2, 'drop')
# C
two('c_w', 's8_C', 1); two('c_w', 'c_jw', 1)
one('c_jw', 'b_ce', 2, 'jump')
one('s8_C', 'd_rt', 0, 'ramp')
# D
two('d_rt', 's4_D', 1); two('d_rt', 'd_ja', 1); two('s4_D', 'd_ja', 2); two('s4_D', 'd_w', 1); two('s4_D', 'd_s1', 2)
two('d_rt', 's3_D', 1); two('d_w', 's3_D', 1)
two('d_s1', 's5_D', 2); two('s5_D', 'd_s2', 2); two('s5_D', 'd_e', 1); two('d_s2', 'd_rt3', 2); two('d_rt3', 'd_s3', 2)
two('d_e', 'd_rt3', 1); two('d_e', 'd_fe', 2); two('d_fe', 'd_s3', 2); two('d_fe', 's2_D', 1);
two('s2_D', 'd_fne', 1); two('s2_D', 'd_ne', 1); two('d_ne', 'd_n', 1); two('d_n', 'd_ne2', 2); two('d_ne2', 'd_e', 1); two('d_ne2', 's2_D', 1); two('d_n', 's3_D', 1)
two('s3_D', 'd_nw', 1); two('d_nw', 'd_n', 1)
one('d_ja', 'a_e2', 2, 'jump')
one('d_s1', 's1_E', -2, 'drop'); one('d_nw', 'c_w', -2, 'drop'); one('d_s2', 'f_c', -2, 'drop'); one('d_s3', 'g', -2, 'drop')
# E, F, G
one('e_w', 's1_E', 2); two('s1_E', 'e_e', 2); two('s1_E', 'e_rf', 1); two('e_e', 'e_rf', 1)
two('e_rf', 'f_rt', 0, 'ramp')
two('f_rt', 'f_w', 1); two('f_rt', 'f_c', 1); two('f_w', 'f_c', 1); two('f_c', 'f_e', 1); two('f_e', 's0_F', 1)
two('s0_F', 'd_rt3', 0, 'ramp'); one('g', 's0_F', 1)
one('f_w', 'e_e', -2, 'drop')
# the pit's way out (one-way)
one('k7', 'k9', 0, 'pit'); one('k9', 'k9w', 0, 'pit'); one('k9w', 'b_s', 2, 'jump')

# retail start points moved after the first in-game run (README section 10)
MOVED_STARTS = {0: "retail (14592, 720): 720 from F's south edge, at the end of the 46->0 run; a car crept on past it off F/G's corner",
                7: 'retail (1800, 9440): 40 from the P/Q box line at the turn into narrow Q; a car stalled there 15 s'}
# ------------------------------------------------------------------ start points: retail 0..8 plus 6 spread ones
START_EXTRA = ['b_c', 'a_e1', 'd_fne', 'e_e', 'p_mid', 'd_w']

# ------------------------------------------------------------------ speed table (+0x1281, [from node's section][to node's section])
# aiRouteSpeedLimit (world_object_register.c:130) applies while the car's own section differs from its next point's:
# throttle (and turbo) below the value, brake above 1.25x. Retail SROOF has 60 everywhere and 110/120 on its jump
# pairs. Changes (each one explained in README.md):
SPEED = {
    # flat crossings (same height, no gap): no forced speed, as New York has 0 there
    (0, 2): 0, (2, 0): 0, (1, 2): 0, (2, 1): 0, (3, 4): 0, (4, 3): 0, (11, 12): 0, (12, 11): 0, (7, 9): 0,
    # A east -> E: retail jumps from section 2 (sets 120 there); our takeoff is in section 1
    (1, 10): 120,
    # P -> C and D -> A land 1600..2400 short of a wall / the A courtyard: 80 (still 1.39x the speed needed)
    (3, 6): 80, (8, 1): 80,
    # B -> Q up the RMP 0 ramp: slow, so the car can turn north on Q (800 wide; its west edge drops into B's alley)
    (5, 4): 40,
    # drops off an edge onto a lower roof with no gap: slow, as New York's (30..50, lane -2), so the car does not
    # carry 60..75 toward the far edge of the roof it lands on
    (8, 10): 30, (8, 11): 30, (8, 12): 30, (8, 6): 30, (11, 10): 30, (3, 5): 30,
}

# ------------------------------------------------------------------ physical ground (GRP floors + RMP), for lane caps
class World:
    def __init__(s, G):
        s.G = G; s.S = {r['k']: r for r in rmp('SROOF') if (r['w'] or r['l']) and r['type'] == 0}
    def surf_h(s, r, x, y):
        dx, dy = x - r['x'], y - r['y']
        if (r['sn'], r['cs']) != (0, 4096):
            dx, dy = (dx * r['cs'] - dy * r['sn']) / 4096, (dy * r['cs'] + dx * r['sn']) / 4096
        if not (0 <= dx <= r['w'] and 0 <= dy <= r['l']): return None
        u, v = dx / r['w'], dy / r['l']; h = r['h']
        return h[0] * (1 - u) * (1 - v) + h[1] * u * (1 - v) + h[2] * (1 - u) * v + h[3] * u * v
    def ground(s, x, y, z):
        sec = s.G.section_at(int(x), int(y), int(z)); g = s.G.floor(sec) if s.G.hasFloor[sec] else 0
        for k in range(s.G.first[sec], s.G.last[sec] + 1):
            if k in s.S:
                h = s.surf_h(s.S[k], x, y)
                if h is not None and h <= z + 150: g = max(g, h)
        return sec, g
    def lethal_ray(s, x, y, z, ux, uy, maxd=3000):
        """distance straight on to where the ground falls to the street (section 13)."""
        for d in range(25, maxd, 25):
            sec, g = s.ground(x + ux * d, y + uy * d, z)
            if sec == 13 or g < 1000: return d
            if abs(g - z) > 40: z = g
        return maxd
W = World(RG)
# Lane cap by run-out (third in-game run: cars overshot e_w and b_w at speed and went off the edge): a link's
# lane is lowered so a car arriving fast at a point with little roof straight on past it does not carry on off it.
#   run-out >= 1500: as set;  1200..1500: <= 1 (2/3 speed);  900..1200: <= 0 (1/2);  < 900: <= -1 (1/3).
# Jump links are not capped (their speed comes from the speed table from the takeoff point on, as on New York).
def lane_cap(d): return 2 if d >= 1500 else 1 if d >= 1200 else 0 if d >= 900 else -1

# ------------------------------------------------------------------ helpers
N = len(P)
def sec_of(i): return RG.section_at(P[i][1], P[i][2], P[i][3])
SEC = [sec_of(i) for i in range(N)]
def D(a, b): return math.hypot(P[a][1] - P[b][1], P[a][2] - P[b][2])
def succ(i): return [s for s, l in SUCC[i]]

def pick(cur, prev, rep, aim, grp):
    """aiPickLinkNode (ai_link_node_pick.c:174): nearest successor to the aim point; the previous node costs
    +80 and at most 3 times in a row; with grp >= 0 only successors in section grp count (else node[0])."""
    best = succ(cur)[0]; bd = 0x7FFFFFFF
    for s in succ(cur):
        if grp >= 0 and SEC[s] != grp: continue
        d = (aim[0] - P[s][1]) ** 2 + (aim[1] - P[s][2]) ** 2
        if d < bd:
            if s != prev: best, bd = s, d
            elif rep < 3: best, bd = s, d + 80
    return best, (rep + 1 if best == prev else 0)

def walk_in(u, e, maxs=40):
    """inside u's section towards node e, only successors in that section (hasDrop filter)."""
    S = SEC[u]; cur, prev, rep, L = u, -1, 0, 0.0
    for _ in range(maxs):
        if cur == e: return L
        nn, rep = pick(cur, prev, rep, (P[e][1], P[e][2]), S)
        if SEC[nn] != S: return None
        L += D(cur, nn); prev, cur = cur, nn
    return None

def dijkstra_to_set(targets):
    """shortest distance from every node to the nearest node of `targets` (reverse graph)."""
    rev = {i: [] for i in range(N)}
    for a in range(N):
        for b in succ(a): rev[b].append(a)
    dist = {t: 0.0 for t in targets}; h = [(0.0, t) for t in targets]
    while h:
        d, u = heapq.heappop(h)
        if d > dist[u]: continue
        for v in rev[u]:
            nd = d + D(u, v)
            if nd < dist.get(v, 1e18): dist[v] = nd; heapq.heappush(h, (nd, v))
    return dist

def tables():
    NEXT = [[-1] * 35 for _ in range(35)]; NS = [[0] * 35 for _ in range(35)]
    for s in range(35): NS[s][s] = s
    choice = {}
    for T in range(NSEC_USED):
        dT = dijkstra_to_set([i for i in range(N) if SEC[i] == T])
        for S in range(NSEC_USED):
            if S == T: continue
            members = [i for i in range(N) if SEC[i] == S]
            cands = []
            for e in members:
                ws = [walk_in(u, e) for u in members]
                if any(w is None for w in ws): continue
                mean = sum(ws) / len(ws)
                for F in sorted({SEC[f] for f in succ(e) if SEC[f] != S}):
                    fs = [f for f in succ(e) if SEC[f] == F]
                    worst = max(D(e, f) + dT.get(f, 1e18) for f in fs)
                    cands.append((mean + worst, e, F))
            cands = [c for c in cands if c[0] < 1e17]
            cands.sort()
            if not cands:      # T cannot be driven into (the pit and its 7400 level: only by falling in)
                NEXT[S][T] = -1; NS[S][T] = RG.nsec(S, T); continue
            best = cands[0]
            rF = RG.nsec(S, T)   # retail's next section: keep it when within 15% of the best
            alt = [c for c in cands if c[2] == rF and c[0] < 1e17]
            if alt and alt[0][0] <= best[0] * 1.15: best = alt[0]
            choice[(S, T)] = best
            NEXT[S][T] = best[1]; NS[S][T] = best[2]
    # row/column 13 (the street) and unused rows stay retail's
    for S in range(35):
        for T in range(35):
            if S >= NSEC_USED or T >= NSEC_USED:
                NEXT[S][T] = RG.next(S, T); NS[S][T] = RG.nsec(S, T)
    return NEXT, NS, choice

def speed_table():
    sp = [[RG.speed(a, b) for b in range(35)] for a in range(35)]
    for (a, b), v in SPEED.items(): sp[a][b] = v
    return sp

def cap_lanes():
    capped = []
    for a in range(N):
        for k, (b, lane) in enumerate(SUCC[a]):
            if KIND[(a, b)] == 'jump': continue
            L = D(a, b); d = W.lethal_ray(P[b][1], P[b][2], P[b][3], (P[b][1] - P[a][1]) / L, (P[b][2] - P[a][2]) / L)
            c = min(lane, lane_cap(d))
            if c != lane: SUCC[a][k] = (b, c); capped.append((P[a][0], P[b][0], lane, c, d))
    return capped

def hold_node(S):
    """the point of S where a car waits while its target is where it cannot drive (the street: mid-jump or falling;
    the pit and its 7400 level). The car then shuttles between it and its first or last own-section successor (the
    game's pick at an exit point), so the choice maximizes the smaller run-out straight on past either end of those
    shuttle links (a turn-around is where cars were seen creeping straight on)."""
    def run(a, b):
        L = D(a, b); return W.lethal_ray(P[b][1], P[b][2], P[b][3], (P[b][1] - P[a][1]) / L, (P[b][2] - P[a][2]) / L)
    best = None
    for i in [j for j in range(N) if SEC[j] == S]:
        nb = [j for j in succ(i) if SEC[j] == S]
        if nb:
            score = min(min(run(i, j), run(j, i)) for j in {nb[0], nb[-1]})
        else:
            score = -1
        if best is None or score > best[0]: best = (score, i)
    return best[1]

def main():
    capped = cap_lanes()
    NEXT, NS, choice = tables()
    # -1 cells off the diagonal of a roof row: the target is in the street (13: mid-jump or falling) or in the pit (7)
    # or its 7400 level (9). Retail -1 = chase straight with no successor filter: cars followed targets off the edges
    # (the in-game cascade). Instead the car holds on its roof: next = a central point of S, next section = S, so the
    # hasDrop filter keeps it on S's own points. The pit's own rows (7, 9) take their way out instead.
    holds = {}
    for S in range(NSEC_USED):
        for T in range(14):
            if T == S or NEXT[S][T] >= 0: continue
            if S in (7, 9):
                NEXT[S][T], NS[S][T] = NEXT[S][5], NS[S][5]
            else:
                if S not in holds: holds[S] = hold_node(S)
                NEXT[S][T], NS[S][T] = holds[S], S
    sp = speed_table()
    starts = list(range(9)) + [ID[n] for n in START_EXTRA]
    nodes = []
    for i, (nm, x, y, z) in enumerate(P):
        nodes.append(dict(id=i, name=nm, x=x, y=y, z=z, section=SEC[i], succ=succ(i), lane=[l for s, l in SUCC[i]],
                          kind=[KIND[(i, s)] for s in succ(i)]))
    secs = []
    for s in range(RG.count):
        secs.append(dict(index=s, name=SEC_NAMES[s], x=RG.pos[s][0], y=RG.pos[s][1], floor=RG.pos[s][2], w=RG.w[s], l=RG.l[s],
                         hasDrop=RG.hasDrop[s], first_surface=RG.first[s], last_surface=RG.last[s]))
    agree = sum(1 for S in range(NSEC_USED) for T in range(NSEC_USED) if S != T and NS[S][T] == RG.nsec(S, T))
    R = dict(level=12, nodes=nodes, holds={str(k): v for k, v in holds.items()}, capped=capped, sections=secs, section_count=RG.count, tables_for=NSEC_USED,
             next_node=NEXT, next_section=NS, speed=sp, speed_changes=[[a, b, RG.speed(a, b), v] for (a, b), v in sorted(SPEED.items())],
             start_nodes=starts, moved_starts={str(k): v for k, v in MOVED_STARTS.items()}, retail_next_section_agreement=[agree, NSEC_USED * (NSEC_USED - 1)])
    if '--json' in sys.argv:  # (for a validator or a plot: beside this script)
        json.dump(R, open(os.path.join(HERE, 'roof_tops_route.json'), 'w'), indent=1)
    print('%d points, %d links, %d start points; next-section agrees with retail in %d of %d pairs' % (
        N, sum(len(n['succ']) for n in nodes), len(starts), agree, NSEC_USED * (NSEC_USED - 1)))
    write_c(R)

def write_c(R):
    nodes = R['nodes']; L = []; w = L.append
    w('/* Roof Tops (level 12) computer-car route: generated by tools/routes/roof_tops.py, do not edit by hand.')
    w(' * %d route points (aiRouteNodes 801B1DE8, 140 at most; points 0..8 = retail SROOF.PTS start points,'
      ' 0 and 7 moved off bad spots after the first in-game run),' % len(nodes))
    w(' * retail SROOF.GRP sections kept (14), %d start points. Written after arenaLoadRoofTops (80119FDC) has loaded' % len(R['start_nodes']))
    w(' * SROOF.PTS / SROOF.GRP / SROOF.RMP; replaces the section-byte fix and the -1 table patch.')
    w(' * GRP writes: next node (+0x354) and next section (+0x81D) for rows 0..12 x columns 0..13 (no -1 left: a target in')
    w(' * the street, the pit or its 7400 level makes cars hold on their roof), and the speed-table')
    w(' * (+0x1281) entries listed in sroof_speed. Section boxes, floors, flags, surfaces, link table, follow ranges,')
    w(' * row 13 and the section count are retail and are not touched. */')
    w('#define SROOF_NODES %d' % len(nodes))
    w('#define SROOF_STARTS %d' % len(R['start_nodes']))
    w('#define SROOF_TSEC %d' % R['tables_for'])
    w('typedef struct { int32_t x, y; int16_t z; int16_t next[4]; int8_t lane[4]; uint8_t count, section; } SroofNode;')
    w('static const SroofNode sroof_nodes[SROOF_NODES] = {')
    for n in nodes:
        nx = n['succ'] + [0] * (4 - len(n['succ'])); ln = n['lane'] + [0] * (4 - len(n['lane']))
        w('    {%6d, %6d, %d, {%3d, %3d, %3d, %3d}, {%d, %d, %d, %d}, %d, %2d}, /* %3d %s */' % (
            n['x'], n['y'], n['z'], *nx, *ln, len(n['succ']), n['section'], n['id'], n['name']))
    w('};')
    T = R['tables_for']
    w('/* [my section][target section], sections 0..12: the exit point in my section (-1 on the diagonal) */')
    w('static const int8_t sroof_next_node[SROOF_TSEC][SROOF_TSEC + 1] = {   /* column 13 = the street */')
    for r in R['next_node'][:T]: w('    {' + ', '.join('%3d' % v for v in r[:T + 1]) + '},')
    w('};')
    w('/* [my section][target section]: the section after mine on the way (mine on the diagonal) */')
    w('static const int8_t sroof_next_section[SROOF_TSEC][SROOF_TSEC + 1] = {')
    for r in R['next_section'][:T]: w('    {' + ', '.join('%2d' % v for v in r[:T + 1]) + '},')
    w('};')
    w('/* speed table entries changed from retail: {from section, to section, value} (retail value in the comment) */')
    w('static const uint8_t sroof_speed[][3] = {')
    for a, b, old, new in R['speed_changes']: w('    {%2d, %2d, %3d}, /* retail %d */' % (a, b, new, old))
    w('};')
    w('static const int16_t sroof_starts[SROOF_STARTS] = {' + ', '.join(str(v) for v in R['start_nodes']) + '};')
    w('')
    w('#define SROOF_GRP GA(0x801A9848u)            /* worldSectionGrpFile, 6036 bytes */')
    w('static void sroof_route_write(void)')
    w('{')
    w('    CPU *cpu = &M.cpu;')
    w('    /* route points: the whole 140-point array (unused ones zero), then the count */')
    w('    for (uint32_t i = 0; i < 140; i++) {')
    w('        uint32_t a = GA(0x801B1DE8u) + 24u * i;')
    w('        for (uint32_t k = 0; k < 24; k += 4) mem_w32(cpu, a + k, 0);')
    w('        if (i >= SROOF_NODES) continue;')
    w('        const SroofNode *n = &sroof_nodes[i];')
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
    w('    mem_w32(cpu, GA(0x80180AD8u), SROOF_NODES);  /* aiRouteNodeCount */')
    w('    /* start points: startNodeList s16[15] 801B2B28, taken flags u8[15] 801B2B46, count 801B2B55 */')
    w('    for (uint32_t i = 0; i < 15; i++) {')
    w('        mem_w16(cpu, GA(0x801B2B28u) + 2u * i, (uint16_t)(i < SROOF_STARTS ? sroof_starts[i] : 0));')
    w('        mem_w8(cpu, GA(0x801B2B46u) + i, 0);')
    w('    }')
    w('    mem_w8(cpu, GA(0x801B2B55u), SROOF_STARTS);')
    w('    /* route tables: rows 0..12, columns 0..13 (row 13, the street itself, keeps retail -1 / 0) */')
    w('    for (uint32_t s = 0; s < SROOF_TSEC; s++)')
    w('        for (uint32_t t = 0; t <= SROOF_TSEC; t++) {')
    w('            mem_w8(cpu, SROOF_GRP + 0x354 + 35u * s + t, (uint8_t)sroof_next_node[s][t]);   /* next node */')
    w('            mem_w8(cpu, SROOF_GRP + 0x81D + 35u * s + t, (uint8_t)sroof_next_section[s][t]); /* next section */')
    w('        }')
    w('    for (uint32_t k = 0; k < sizeof sroof_speed / sizeof sroof_speed[0]; k++)')
    w('        mem_w8(cpu, SROOF_GRP + 0x1281 + 35u * sroof_speed[k][0] + sroof_speed[k][1], sroof_speed[k][2]); /* speed table */')
    w('}')
    open(os.path.join(HERE, '..', '..', 'src', 'vplus', 'roof_tops_route.inc'), 'w', newline='\n').write('\n'.join(L) + '\n')

if __name__ == '__main__':
    main()
