"""Run scripted scenarios on two builds of the port and compare them frame by frame (docs/HYBRID.md).

    python tools/test/compare_builds.py [--a EXE] [--b EXE] [--only NAME,...] [--list]

Each scenario runs once per executable in build/port-dev (silent, windowed), with a state log
(TM2PC_STATE_LOG: the cars' records and the random state per frame) and one screenshot; the two
logs must be equal frame for frame and the screenshots byte for byte. Defaults: a = tm2pc.exe (the
retail-based port), b = tm2pc-hooks.exe (the hybrid: build/port-hooks copied there).

The test profiles CBTest and Caesar (build/port-dev/profiles) are restored from build/test-fixtures
after every run (taken from port-dev on the first run). The dev tm2pc.ini is made from the play
build's (build/port/tm2pc.ini, only read) and put back afterwards. The user's profiles in
build/port are never touched."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEV = Path(os.environ.get('CB_DEV', REPO / 'build' / 'port-dev'))  # (CB_DEV, CB_OUT: a second run at the same time elsewhere)
FIX = REPO / 'build' / 'test-fixtures'
OUT = Path(os.environ.get('CB_OUT', REPO / 'build' / 'test-compare'))

# the menus: player 1 signs in, Local Play, Multiplayer, (sign-in: both ready), ...
UI_CHALLENGE = '-,-,-,x,-,-,-,-,-,-,-,-,-,x,-,-,-,-,x,-,-,down,x,-,-,-,-,x,-,-,-,-,up,x,-,-,-,-,-,-,start'
UI_ONE = '-,-,-,x,-,x,-,x,-,down,x,-,-,-,-,x,-,-,-,-,up,x,-,-,-,-,-,-,start'
UI_TOURNEY = '-,-,-,x,-,-,-,-,-,-,-,-,-,x,-,-,-,-,up,x,-,-,-,-,-,-,-,-,x,-,-,-,-,x'
UI_FOUR = ('-,-,-,x,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,start,-,-,-,-,start,-,-,-,-,-,-,x,-,-,-,-,x')
UI_FOUR_CHALLENGE = ','.join(UI_FOUR.split(',')[:33] + ['down', 'x']) + ',-,-,-,-,x,-,-,-,-,up,x,-,-,-,-,-,-,start'  # (as UI_CHALLENGE past its mode page)
UI_THREE = '-,-,-,x,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,start,-,-,-,-,-,-,-,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x'
W = ','.join(['-'] * 40)


def pad_x(start, end, step=100):
    """pad 1: X pressed every `step` VBlanks (by VBlank: the globe screens, firing)"""
    return ','.join('%d:cross,%d:-' % (v, v + 8) for v in range(start, end, step))


TWO = dict(TM2PC_P2_SCRIPT='-,-,-,-,x', TM2PC_CARSEL_SCRIPT='-,-,-,-,x', TM2PC_CARSEL_P2_SCRIPT='-,-,-,-,-,-,x')
FOUR = dict(TM2PC_P2_SCRIPT='-,-,-,-,-,-,x', TM2PC_P3_SCRIPT='-,-,-,-,down,down,down,down,down,x',
            TM2PC_P4_SCRIPT='-,-,-,-,-,down,down,down,down,down,x', TM2PC_CARSEL_SCRIPT='%s,%s,x' % (W, W),
            TM2PC_CARSEL_P2_SCRIPT='-,-,-,-,right,%s,%s,x' % (W, W),
            TM2PC_CARSEL_P3_SCRIPT='-,-,-,-,-,-,right,-,-,right,%s,%s,x' % (W, W),
            TM2PC_CARSEL_P4_SCRIPT='-,-,-,-,-,-,-,-,left,%s,%s,x' % (W, W))

# a game type of one driver (Last Car Standing): every opponent the same car
ONE_CAR = ['name = One Car', 'mode = 0', 'drivers = 0001']
# Juggernaut with the car forced (Minion)
JUGG_MINION = ['name = Forced Jugg', 'mode = 2', 'respawn = 3', 'jugg_force = 1', 'jugg_car = 13']
KOTH3 = ['name = Three Hills', 'mode = 5', 'respawn = 3', 'koth_hills = 3', 'koth_spots = 0']
HUNTED_TEAMS = ['name = Team Hunt', 'mode = 4', 'respawn = 3', 'hunted_teams = 1', 'hunted_count = 2']
HUNTED_ONE = ['name = Team Hunt', 'mode = 4', 'respawn = 3', 'hunted_teams = 0', 'hunted_count = 3']
JUGG_INVIS = ['name = Ghost Jugg', 'mode = 2', 'respawn = 3', 'jugg_invisible = 1']
GUN_SPECIALS = ['name = Gun Specials', 'mode = 3', 'respawn = 3', 'gun_levels = 00', 'gun_specials = 6bba', 'gun_infinite = 1']

# name: seconds, the shot's VBlank, env, profile additions {profile: {section: lines}}
SCENARIOS = {
    'one': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), **TWO), {}),
    'two': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO), {}),
    'one60': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), **TWO), {'ini': ['frame_rate = 60']}),  # (at 60 fps: the pickups' blink, TM2PC_ANIM_DEBUG)
    'mirrors': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                {'CBTest': {'hud': ['hud_mirror = on', 'hud_style = black']},
                 'Caesar': {'hud': ['hud_mirror = on', 'hud_style = 2012']}}),
    'lives': (90, 4200, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
              {'CBTest': {'battle': ['lives = 3'], 'hud': ['hud_energy = blue', 'hud_turbo_shown_as = bar']}}),
    'teams': (120, 6000, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO), {'CBTest': {'battle': ['teams = 2']}}),
    'kills': (100, 4400, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='1@3000,2@3400,50@3600,51@3700', **TWO),
              {'CBTest': {'battle': ['lives = 2']}}),
    'freeze': (90, 4400, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_FREEZE='1@2900', TM2PC_TEST_KILL='1@2960',
                              TM2PC_PAD1_SCRIPT='3500:up', **TWO), {'CBTest': {'battle': ['lives = 3']}}),
    'feed': (80, 3700, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_FEED='1,2,3,4,5', **TWO), {}),
    'four': (200, 9000, dict(TM2PC_UI_SCRIPT=UI_FOUR, **FOUR), {}),
    'four_black': (100, 4200, dict(TM2PC_UI_SCRIPT=UI_FOUR, TM2PC_TEST_HUD_STYLE=os.environ.get('HS', '1'), **FOUR),
                   {'ini': ['hud_style = black'], 'CBTest': {'hud': ['hud_style = black']}, 'Caesar': {'hud': ['hud_style = black']}}),
    'wobble_ps1': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), **TWO), {'ini': ['wobble = ps1']}),  # (= one before precise vertices)
    'msg_long': (60, 2060, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_FLASH='YOU KILLED GRASSHOPPER',
                                TM2PC_TEST_FLASH_AT='2000', **TWO), {}),
    'wide1': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_WINDOW='1280x720', **TWO),
              {'ini': ['widescreen = on']}),
    'wide2': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_WINDOW='1280x720', **TWO), {'ini': ['widescreen = on']}),
    'wide_resize': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_WINDOW='1280x720',
                                   TM2PC_TEST_RESIZE='1680x720@2500', **TWO), {'ini': ['widescreen = on']}),  # (mid-battle: 21:9)
    'wide_black': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_WINDOW='1280x720',
                                  TM2PC_TEST_HUD_STYLE=os.environ.get('HS', '1'), **TWO), {'ini': ['widescreen = on']}),
    'ultra_2012': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_WINDOW='1680x720',
                                  TM2PC_TEST_HUD_STYLE=os.environ.get('HS', '2'), **TWO), {'ini': ['widescreen = on']}),
    'mirror_wide': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_WINDOW='1280x720', **TWO),
                    {'ini': ['widescreen = on', 'hud_mirror = ' + os.environ.get('MIRROR', 'on')]}),
    'wide4_black': (120, 4200, dict(TM2PC_UI_SCRIPT=UI_FOUR, TM2PC_TEST_WINDOW='1280x720', TM2PC_TEST_HUD_STYLE=os.environ.get('HS', '1'),
                                    **FOUR), {'ini': ['widescreen = on']}),
    'wide4': (120, 4200, dict(TM2PC_UI_SCRIPT=UI_FOUR, TM2PC_TEST_WINDOW='1280x720', **FOUR), {'ini': ['widescreen = on']}),
    'rewind': (100, 4300, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100) + ',2800:start,2808:-',
                               TM2PC_PAUSE_SCRIPT='-,-,-,down,down,x', TM2PC_PAUSE_SCRIPT_AFTER='-,-,-,x', TM2PC_REWIND_TEST='600',
                               **TWO), {}),  # (photo mode rewind: 600 frames back, a picture, played to the present, resumed)
    'pause': (100, 4300, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100) + ',2800:start,2808:-',
                              TM2PC_PAUSE_SCRIPT='-,-,-,x', **TWO), {}),  # (rewind's reference: paused and resumed)
    'koth': (100, 4300, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_HILL_DEBUG='1', **TWO),
             {'CBTest': {'battle': ['game_type = King of the Hill']}}),
    'koth_marks': (100, '2400v,3000v,3600v,4200v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_HILL_HERE='1', **TWO),
                   {'CBTest': {'battle': ['game_type = King of the Hill']}}),  # (the hill's box, the markers: shots)
    'tour': (110, '1500v,1575v,1650v,1725v,1800v,1875v,1950v,2025v,2100v,2175v,2250v,2325v,2400v,2475v,2550v,2625v,2700v,2775v,2850v,2925v,3000v,3075v,3150v,3225v,3300v,3375v,3450v,3525v,3600v,3675v,3750v,3825v,3900v,3975v,4050v,4125v,4200v,4275v,4350v,4425v,4500v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
             {'CBTest': {'battle': ['game_type = King of the Hill']}}),  # (TM2PC_TOUR: player 1 moved spot to spot; hill spots)
    'tourlook': (110, ','.join('%dv' % v for v in range(1500, 4500, 75)), dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TOUR_LOOK='1', **TWO),
                 {'CBTest': {'battle': ['game_type = King of the Hill']}}),  # (TM2PC_TOUR: each spot's hill seen from 2400 away)
    'koth3': (100, '2400v,3000v,3600v,4200v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_HILL_DEBUG='1', **TWO),
              {'CBTest': {'battle': ['game_type = Three Hills', 'kills = 100'], 'game_type1': KOTH3}}),  # (3 hills at once, the chosen spots)
    'koth_teams': (100, '2400v,3600v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_HILL_DEBUG='1', **TWO),
                   {'CBTest': {'battle': ['game_type = Three Hills', 'teams = 2', 'kills = 100'], 'game_type1': KOTH3}}),  # (3 hills, 2 teams)
    'laquake': (110, ','.join('%dv' % v for v in range(2200, 2950, 20)), dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                {'CBTest': {'battle': ['game_type = King of the Hill']}}),  # (TM2PC_LA_QUAKE + TM2PC_TOUR: the LA quake watched)
    'dt_player': (100, '2400v,2700v,3000v,3300v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_DT_PLAYER='1', TM2PC_TEST_P1_CAR='14',
                                                    TM2PC_PAD1_SCRIPT=pad_x(2300, 3400, 20), **TWO), {}),  # (player 1 drives Dark Tooth, firing)
    'arena_off': (100, '2400v,3000v,3600v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                  {'CBTest': {'battle': ['game_type = Calm Arena', 'kills = 100'], 'game_type1': ['name = Calm Arena', 'mode = 4', 'respawn = 3',
                                                                                  'hazards = 00', 'destructibles = 00']}}),  # (request 38 (ARENA_SWITCHES.md): both off)
    'dt_carsel': (100, ','.join('%dv' % v for v in range(600, 1700, 100)) + ',2400v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_DT_PLAYER='1',
                  TM2PC_P2_SCRIPT='-,-,-,-,x', TM2PC_CARSEL_SCRIPT='-,-,-,-,left,-,-,-,-,x', TM2PC_CARSEL_P2_SCRIPT='-,-,-,-,-,-,x'), {}),  # (Dark Tooth picked in the 2-player car select)
    'shared_unlocks': (100, '1300v,2400v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_P2_SCRIPT='-,-,-,-,x',
                       TM2PC_CARSEL_SCRIPT='-,-,-,-,left,-,-,-,-,x', TM2PC_CARSEL_P2_SCRIPT='-,-,-,-,-,-,x'),
                       {'CBTest': {'battle': ['game_type = Caesar Hunt', 'kills = 100']},
                        'Caesar': {'endings': ['%s = yes' % n for n in ('Hammerhead', 'Outlaw 2', 'Warthog', 'Mr. Grimm', 'Grasshopper',
                                   'Thumper', 'Spectre', 'Roadkill', 'Twister', 'Axel', 'Mr. Slam', 'Shadow', 'Sweet Tooth', 'Minion')],
                                   'game_type1': ['name = Caesar Hunt', 'mode = 4', 'respawn = 3']}}),  # (player 2's unlocks and game type shared)
    'slam4': (130, ','.join('%dv' % v for v in range(3600, 3636, 3)), dict(TM2PC_UI_SCRIPT=UI_FOUR_CHALLENGE, TM2PC_TEST_RANDOM_CAR='1', **FOUR),
              {'CBTest': {'battle': ['opponents = 9', 'game_type = Slams'],
                          'game_type1': ['name = Slams', 'mode = 0', 'drivers = 0400']}}),  # (4 players among Mr. Slams: his arm, frame by frame)
    'carsel4_slam': (45, ','.join('%dv' % v for v in range(1400, 1448, 2)), dict(TM2PC_UI_SCRIPT=UI_FOUR,
                     **dict(FOUR, TM2PC_CARSEL_SCRIPT='-,-,-,-,right,-,-,right,-,-,right,%s,%s,x' % (W, W))), {}),  # (Mr. Slam in the 4-player car select)
    'carsel2_slam': (40, ','.join('%dv' % v for v in range(1400, 1448, 2)), dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_P2_SCRIPT='-,-,-,-,x',
                     TM2PC_CARSEL_SCRIPT='-,-,-,-,right,-,-,right,-,-,right,-,-,right,%s,%s,x' % (W, W), TM2PC_CARSEL_P2_SCRIPT='%s,%s,x' % (W, W)), {}),  # (Mr. Slam, 2-player car select)
    'secret': (110, ','.join('%dv' % v for v in range(2000, 5000, 300)), dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_AI_DEBUG='1', **TWO),
               {'CBTest': {'battle': ['opponents = 6']}}),  # (TM2PC_TEST_ARENA=10..12: a secret arena with computer cars)
    'koth_marks60': (100, '2400v,3000v,3601v,4202v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_HILL_HERE='1', **TWO),
                     {'CBTest': {'battle': ['game_type = King of the Hill']}, 'ini': ['frame_rate = 60']}),  # (at 60 fps: in-between frames too)
    'far': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), **TWO), {'ini': ['draw_distance = farthest']}),
    'dm_suicide': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='2@2600', TM2PC_TEST_KILL_SELF='1', **TWO),
                   {'CBTest': {'battle': ['game_type = Deathmatch']}}),  # (player 1 destroyed with no killer: -1 kill)
    'jugg_suicide': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='2@2600', TM2PC_TEST_KILL_SELF='1', **TWO),
                     {'CBTest': {'battle': ['game_type = Juggernaut']}}),
    'menu2': (30, 99999, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE[:UI_CHALLENGE.index(',-,-,down,x,-,-,-,-,x,-,-,-,-,up')], TM2PC_LIVE_SHOT='2', **TWO), {}),  # (live.bmp: the 2 and 4 player menus)
    'menu4': (34, 99999, dict(TM2PC_UI_SCRIPT=UI_FOUR[:-len(',x,-,-,-,-,x')], TM2PC_LIVE_SHOT='2', **FOUR), {}),
    'interp': (80, '3600v,3601v,3602v,3603v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100) + ',3000:up', TM2PC_INTERP='1', **TWO),
               {}),  # (interpolated frames: four VBlanks in a row)
    'interp2': (80, '3600v,3601v,3602v,3603v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_PAD1_SCRIPT='3000:up', TM2PC_INTERP='1', **TWO),
                {}),
    'interp4': (120, '4000v,4001v,4002v,4003v', dict(TM2PC_UI_SCRIPT=UI_FOUR, TM2PC_INTERP='1', **FOUR), {}),
    'interp_mirror': (80, '3600v,3601v,3602v,3603v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100) + ',3000:up',
                                                          TM2PC_INTERP='1', **TWO), {'ini': ['hud_mirror = on']}),
    'interp2_off': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_PAD1_SCRIPT='3000:up', TM2PC_INTERP='0', **TWO), {}),  # (the same, without: equal state)
    'interp_mirror_off': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100) + ',3000:up', TM2PC_INTERP='0', **TWO),
                          {'ini': ['hud_mirror = on']}),
    'interp144': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100) + ',3000:up', TM2PC_INTERP='1',
                                  TM2PC_INTERP_HZ='144', TM2PC_INTERP_DEBUG='1', **TWO), {}),  # (uncapped at 144 Hz: frames counted)
    'interp_killcam': (120, 6000, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_INTERP='1', **TWO), {'CBTest': {'battle': ['teams = 2']}}),  # (teams' kill cam)
    'interp_killcam_off': (120, 6000, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_INTERP='0', **TWO), {'CBTest': {'battle': ['teams = 2']}}),
    'carsel_cadence': (40, '1500v,1501v,1502v,1503v,1504v,1505v,1506v,1507v,1508v,1509v,1510v,1511v,1512v,1513v,1514v,1515v,1516v,1517v,1518v,1519v,1520v,1521v,1522v,1523v,1524v,1525v,1526v,1527v,1528v,1529v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_CARSEL_SCRIPT=W + ',' + W, **{k: v for k, v in TWO.items() if k != 'TM2PC_CARSEL_SCRIPT'}), {}),  # (30 VBlanks of car select)
    'teams_shot': (80, '2400v,3000v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_PAD1_SCRIPT='2200:up', **TWO), {'CBTest': {'battle': ['teams = 2']}}),
    'uncapped': (80, 3600, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100) + ',3000:up', TM2PC_INTERP_DEBUG='1', **TWO),
                 {'ini': ['frame_rate = uncapped']}),  # (the display's clock: frames counted; the same state as interp)
    'carsel_uncapped': (40, '1500v,1501v,1502v,1503v,1504v,1505v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_CARSEL_SCRIPT=W + ',' + W, **{k: v for k, v in TWO.items() if k != 'TM2PC_CARSEL_SCRIPT'}),
                        {'ini': ['frame_rate = uncapped']}),  # (car select on the display's clock)
    'axel': (55, '1800v,1812v,1824v,1836v,1848v,1860v,1872v,1884v,1896v,1908v,1920v,1932v,1944v,1956v,1968v,1980v,1992v,2004v,2016v,2028v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_CARSEL_SCRIPT=W + ',' + W, TM2PC_PAD1_SCRIPT='1500:right,1508:-,1560:right,1568:-',
                           **{k: v for k, v in TWO.items() if k != 'TM2PC_CARSEL_SCRIPT'}), {}),  # (Axel turning in car select: 20 shots)
    'axel_ps1': (55, '1800v,1812v,1824v,1836v,1848v,1860v,1872v,1884v,1896v,1908v,1920v,1932v,1944v,1956v,1968v,1980v,1992v,2004v,2016v,2028v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_CARSEL_SCRIPT=W + ',' + W, TM2PC_PAD1_SCRIPT='1500:right,1508:-,1560:right,1568:-',
                           **{k: v for k, v in TWO.items() if k != 'TM2PC_CARSEL_SCRIPT'}), {'ini': ['wobble = ps1']}),  # (Axel turning in car select: 20 shots)
    'carsel1': (42, 99999, dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_LIVE_SHOT='2', TM2PC_CARSEL_SCRIPT=W + ',' + W, **{k: v for k, v in TWO.items() if k != 'TM2PC_CARSEL_SCRIPT'}), {}),
    'carsel2': (40, 99999, dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_LIVE_SHOT='2', TM2PC_P2_SCRIPT='-,-,-,-,x',
                                TM2PC_CARSEL_SCRIPT=W + ',' + W, TM2PC_CARSEL_P2_SCRIPT=W + ',' + W), {}),
    'carsel4': (45, 99999, dict(TM2PC_UI_SCRIPT=UI_FOUR, TM2PC_LIVE_SHOT='2', **FOUR), {}),  # (live.bmp: car select)
    # 16 cars (decomp request 32 in the variant: the plain build plays the game's limits, so only b is the test)
    'sixteen': (100, '2400v,3600v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO), {'CBTest': {'battle': ['opponents = 14']}}),
    **{'sixteen_a%d' % a: (100, '2400v,3600v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_ARENA=str(a), **TWO),
                          {'CBTest': {'battle': ['opponents = 14']}}) for a in range(1, 9)},  # (every arena: the banks)
    'sixteen_same': (100, '2400v,3600v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                     {'CBTest': {'battle': ['opponents = 14', 'game_type = One Car'], 'game_type1': ONE_CAR}}),
    # each car its own special (decomp request 33-2a): 16 Twisters (tornadoes at once), 16 Mr. Slams (arms and claws)
    'sixteen_twister': (110, '2600v,3200v,3800v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                        {'CBTest': {'battle': ['opponents = 14', 'game_type = Twisters'],
                                    'game_type1': ['name = Twisters', 'mode = 0', 'drivers = 0100']}}),
    'sixteen_slam': (110, '2600v,3200v,3800v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                     {'CBTest': {'battle': ['opponents = 14', 'game_type = Slams'],
                                 'game_type1': ['name = Slams', 'mode = 0', 'drivers = 0400']}}),
    'sixteen_teams': (140, '3000v,4200v', dict(TM2PC_UI_SCRIPT=UI_FOUR_CHALLENGE, TM2PC_TEST_RANDOM_CAR='1', **FOUR),
                      {'CBTest': {'battle': ['opponents = 12', 'game_type = One Car', 'teams = 4',
                                             'opponent_teams = 0,0,0,1,1,1,2,2,2,3,3,3', 'player_teams = 0,1,2,3'],
                                  'game_type1': ONE_CAR}}),
    # the Juggernaut's car (Minion, the game type's default): taken over at the start, then by each new Juggernaut
    'sixteen_tourney4': (140, '3000v,4200v', dict(TM2PC_UI_SCRIPT=UI_FOUR, **FOUR), {}),  # (4 players' tournament: its count)
    'jugg_car': (110, '2300v,2700v,3300v,4300v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='2@2500,1@3100,50@3600,51@3900',
                                                     **TWO), {'CBTest': {'battle': ['game_type = Forced Jugg'], 'game_type1': JUGG_MINION}}),
    'hunted_teams': (100, '2400v,3000v,3600v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='2@2500,3@3100', **TWO),
                     {'CBTest': {'battle': ['game_type = Team Hunt', 'teams = 2', 'kills = 100'], 'game_type1': HUNTED_TEAMS}}),  # (Hunted, Swap team: 2 targets)
    'hunted_one': (100, '2400v,3000v,3600v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='2@2500,3@3100', **TWO),
                   {'CBTest': {'battle': ['game_type = Team Hunt', 'teams = 2', 'kills = 100'], 'game_type1': HUNTED_ONE}}),  # (One by one: 3)
    'jugg_invis': (100, '2400v,2403v,2406v,2409v,2412v,3000v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                   {'CBTest': {'battle': ['game_type = Ghost Jugg'], 'game_type1': JUGG_INVIS}}),  # (the Juggernaut shimmers)
    'jugg_invis_me': (100, '2400v,2403v,2406v,2409v,2412v,3000v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_SPECIAL_SLOT='0', **TWO),
                      {'CBTest': {'battle': ['game_type = Ghost Jugg'], 'game_type1': JUGG_INVIS}}),  # (player 1 the Juggernaut: its own car seen)
    'jugg_invis_marks': (100, '2400v,2403v,2406v,2409v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                         {'CBTest': {'battle': ['game_type = Ghost Jugg'], 'game_type1': JUGG_INVIS, 'hud': ['hud_enemy_markers = on']}}),  # (no marker over the ghost)
    'gun_specials': (110, '2400v,2700v,3000v,3300v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, **TWO),
                     {'CBTest': {'battle': ['game_type = Gun Specials'], 'game_type1': GUN_SPECIALS}}),  # (Gun Game: the 10 specials as levels)
    # spectating: player 1 out, watching (the HUD without their own car's radar, weapons and health)
    'spect_hud1': (90, '3300v,3900v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_KILL='1@2600', **TWO), {}),
    'freecam': (100, '3350v,3500v,3700v,3900v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100) +
                                                     ',3300:square,3308:-,3420:square,3428:-,3800:cross,3808:-',
                                                     TM2PC_TEST_KILL='1@2600', **TWO), {}),  # (spectating: cinematic, freecam, chase)
    'spect_hud2': (90, '3300v,3900v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='1@2600', **TWO), {}),
    'spect_cine2': (100, '3420v,3700v,3900v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_PAD1_SCRIPT='3300:square,3308:-,3800:cross,3808:-',
                                                  TM2PC_TEST_KILL='1@2600', **TWO), {}),  # (split screen: cinematic, then the chase camera)
    'spect_hud4': (120, '3400v,4000v', dict(TM2PC_UI_SCRIPT=UI_FOUR_CHALLENGE, TM2PC_TEST_RANDOM_CAR='1', TM2PC_TEST_KILL='1@2700,3@2750,50@2800,51@2900',
                                            **FOUR), {}),
    'feed2': (100, '4150v,4300v,4450v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='50@3000', TM2PC_TEST_FEED='1,2,4', **TWO), {}),
    'feed2_mirror': (70, '3100v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='50@3000', TM2PC_TEST_FEED='1,2,4', **TWO),
                     {'CBTest': {'hud': ['hud_mirror = on']}, 'Caesar': {'hud': ['hud_mirror = on']}}),
    # HUD alignment (TM:Black): the radar, message bar and feed level at the top; the lives and the weapons box at the bottom
    'align1': (90, '2700v,2800v,3000v', dict(TM2PC_UI_SCRIPT=UI_ONE, TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_KILL='50@2600',
                                           TM2PC_TEST_FEED='1,2', TM2PC_TEST_HUD_STYLE='1', **TWO), {}),
    'stats': (120, '5200v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_WIN_EVERY='600', TM2PC_TEST_KILL='2@4400',
                                 TM2PC_TEST_KILL_BY='1', **TWO), {}),  # (the Stats screen's counts: a match won by player 1)
    'tour': (420, '12000v', dict(TM2PC_TMENU_SCRIPT='-,-,-,-,x', TM2PC_UI_SCRIPT='-,-,-,x,-,x,-,x,-,x,-,-,-,down,down,x,-,-,-,left,-,-,up,x,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x', TM2PC_PAD1_SCRIPT=pad_x(800, 2100), TM2PC_TEST_WIN_EVERY='600'),
             {}),  # (a custom tournament of 6 stages, each won: every computer car destroyed 600 VBlanks in)
    'tour_dt': (200, '2400v,2600v,2800v,3000v,3200v,3400v,3600v,3800v,4000v,4200v', dict(TM2PC_TMENU_SCRIPT='-,-,-,-,x', TM2PC_UI_SCRIPT='-,-,-,x,-,x,-,x,-,x,-,-,-,down,down,x,-,-,-,left,-,-,up,x,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x,-,-,-,-,x', TM2PC_PAD1_SCRIPT=pad_x(800, 2100),
                                TM2PC_TEST_WIN_EVERY='600'),
                {'CBTest': {'custom_tournament': ['stage1 = 1, 3, 1, 14, 13']}}),  # (Dark Tooth as stage 1's boss, Los Angeles)
    'net_join': (90, 1500, dict(TM2PC_UI_SCRIPT='-,right,-,x,-,-,-,x,-,start', TM2PC_NET_SCRIPT='-,-,-,x,-,-,-,-,x,-,-,-,-,square,-,-,-,-,down,-,up,-,x,-,-,-,-,square,-,-,-,-,x,-,x,-,x,-,start,-,-,-,-,down,-,x,-,-,-,-,right,-,down,-,x,-,-,-,-,o,-,-,-,-,o,-,-,-,-,o', TM2PC_TEST_SERVERS='1', TM2PC_NET_LOCAL='1'), {}),  # (Network Play: Online, Join, a server, a chat line, a player muted and a vote to kick, a car)
    'net_host': (120, 1500, dict(TM2PC_UI_SCRIPT='-,right,-,x,-,-,-,x,-,start', TM2PC_NET_SCRIPT='-,-,-,down,-,x,-,-,-,-,down,-,x,-,-,-,-,up,-,x,-,-,-,-,triangle,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,select,-,-,-,-,o,-,-,-,-,start,-,-,-,-,x,-,-,-,-,o,-,-,-,-,o,-,-,-,-,o,-,-,-,-,o', TM2PC_PAD1_SCRIPT='1560:right,1568:-,1600:cross,1608:-', TM2PC_NET_LOCAL='1'), {}),  # (Network Play: LAN, Host, Challenge Match, Create server: a car, Edit game, Start, your page, Leave)
    'multi': (90, '2610v,2740v,2870v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='2@2600', TM2PC_TEST_FEED='1,2,3,4,5,6,7',
                                     TM2PC_TEST_FEED_P1='1', **TWO), {}),  # (three kills by player 1 at once: TRIPLE KILL)
    'hunted4': (140, '3600v,4200v', dict(TM2PC_UI_SCRIPT=UI_FOUR_CHALLENGE, TM2PC_TEST_RANDOM_CAR='1', **FOUR),
                {'CBTest': {'battle': ['game_type = Team Hunt', 'teams = 2', 'kills = 100'], 'game_type1': HUNTED_TEAMS}}),  # (4 players: the marks in quarters)
    'align2': (100, '4150v,4300v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='50@3000', TM2PC_TEST_FEED='1,2',
                                       TM2PC_TEST_HUD_STYLE='1', **TWO), {}),
    # Survival's round 10 with Dark Tooth (the decomp's portCarOptions bit 6: his fight on any arena)
    'surv_boss': (100, '2600v,3400v', dict(TM2PC_UI_SCRIPT='-,-,-,x,-,x,-,x,-,down,down,x,-,-,-,-,up,x', TM2PC_TEST_SURV_ROUND='10',
                                          TM2PC_PAD1_SCRIPT=pad_x(800, 2100), **TWO), {}),
    # co-op Survival, 2 players (Dark Tooth the last car, always since 2026-10-16; player 1 out until the next round)
    'surv2': (100, '3400v', dict(TM2PC_UI_SCRIPT=','.join(UI_CHALLENGE.split(',')[:21] + ['down', 'down', 'x']) + ',-,-,-,-,down,-,-,x',
                                 TM2PC_TEST_KILL='1@2600', **TWO), {}),
    # Survival with 4 players (co-op: a player out watches until the next round), from round 9
    'surv4': (150, '3000v,4200v', dict(TM2PC_UI_SCRIPT=','.join(UI_FOUR.split(',')[:33] + ['down', 'down', 'x']) + ',-,-,-,-,up,x',
                                      TM2PC_TEST_SURV_ROUND='9', TM2PC_TEST_KILL='1@2600,3@2650', **FOUR), {}),
    'surv4_banner': (110, ','.join('%dv' % v for v in range(1700, 2600, 100)), dict(TM2PC_UI_SCRIPT=','.join(UI_FOUR.split(',')[:33] + ['down', 'down', 'x']) + ',-,-,-,-,up,x',
                                      TM2PC_TEST_SURV_ROUND='9', TM2PC_TEST_KILL='1@2600,3@2650', **FOUR), {}),
    'feed_side': (90, '3000v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_FEED='1,2,3,4,5', **TWO), {'ini': ['split = side_by_side']}),
    # measurements (decomp questions 3-4): the packet buffers' peak and the car models' area, 16 cars at Farthest
    # (TM2PC_PACKET_PEAK logs them); seeds for other random arenas
    **{'peak4_%d' % seed: (150, '4200v', dict(TM2PC_UI_SCRIPT=UI_FOUR_CHALLENGE, TM2PC_TEST_RANDOM_CAR='1', TM2PC_PACKET_PEAK='1',
                                              TM2PC_SEED=str(seed), **FOUR),
                            {'CBTest': {'battle': ['opponents = 12']}, 'ini': ['draw_distance = farthest']}) for seed in (7, 3, 11, 19)},
    **{'peak2_%d' % seed: (110, '3600v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_PACKET_PEAK='1', TM2PC_SEED=str(seed), **TWO),
                            {'CBTest': {'battle': ['opponents = 14']}, 'ini': ['draw_distance = farthest']}) for seed in (7, 3, 11, 19)},
    # a Juggernaut match to its end (2 kills): the final kill cam replays the rules (and the car's handovers)
    'jugg_end': (200, '9000v', dict(TM2PC_UI_SCRIPT=UI_CHALLENGE, TM2PC_TEST_KILL='2@2500,1@3100', **TWO),
                 {'CBTest': {'battle': ['game_type = Forced Jugg', 'kills = 2'], 'game_type1': JUGG_MINION}}),
    'three': (200, 9000, dict(TM2PC_UI_SCRIPT=UI_THREE, **{k: v for k, v in FOUR.items() if 'P4' not in k}), {}),
    'tournament': (780, 45000, dict(TM2PC_UI_SCRIPT=UI_TOURNEY, TM2PC_TMENU_SCRIPT='-,-,-,-,x', TM2PC_TEST_WIN_EVERY='600',
                                    TM2PC_PAD1_SCRIPT=pad_x(1000, 60000, 60), **TWO), {}),
}


def dev_ini(video=()):
    """the play build's ini (only read), with the test profile and auto controllers; `video`: lines for [video]"""
    if not any(l.startswith('frame_rate') for l in video):  # (the comparisons: the game's frames; interp* scenarios: 60)
        video = list(video) + ['frame_rate = original']
    if not any(l.startswith('draw_distance') for l in video):  # (the far LOD is the hybrid's only: the comparisons at the PS1's)
        video = list(video) + ['draw_distance = original']
    if not any(l.startswith('split') for l in video):  # (not the player's own: the baselines' split, top and bottom)
        video = list(video) + ['split = top_bottom']
    keys = {l.split('=')[0].strip() for l in video}
    vplus = [l.strip() for l in os.environ.get('CB_VPLUS', '').split(';') if l.strip()]
    vkeys = {l.split('=')[0].strip() for l in vplus}  # (the play ini's own lines for these keys left out: CB_VPLUS wins)
    lines = []
    section = ''
    for line in (REPO / 'build' / 'port' / 'tm2pc.ini').read_text(encoding='utf-8').splitlines():
        if line.strip().startswith('['):
            section = line.strip()
        if section == '[update]':  # (the tests' own below: no update check, no asking GitHub)
            continue
        if line.startswith('name ='):
            line = 'name = CBTest'
        elif line.startswith(('player1_controller =', 'player2_controller =')):
            line = line.split('=')[0].rstrip() + ' = auto'
        if line.split('=')[0].strip() in keys or (section == '[vanilla_plus]' and line.split('=')[0].strip() in vkeys):
            continue
        lines.append(line)
        if line.strip() == '[video]':
            lines += list(video)
        if line.strip() == '[vanilla_plus]':  # (CB_VPLUS="bal_mines = off;bal_axel = off": every scenario, e.g. the
            lines += vplus  # balance off)
    lines += ['[update]', 'check = off']
    (DEV / 'tm2pc.ini').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def profiles(extra):
    for name in ('CBTest', 'Caesar'):
        text = (FIX / (name + '.ini')).read_text(encoding='utf-8')
        for section, lines in extra.get(name, {}).items():
            keys = {l.split('=')[0].strip() for l in lines}
            if section == 'hud':
                text = text.rstrip('\n') + '\n\n[hud]\n' + '\n'.join(lines) + '\n'
                continue
            if '[%s]' % section not in text:  # (a section the profile has not: added, e.g. a game type of its own)
                text = text.rstrip('\n') + '\n\n[%s]\n' % section + '\n'.join(lines) + '\n'
                continue
            out = []
            for l in text.splitlines():
                if l.split('=')[0].strip() in keys:
                    continue
                out.append(l)
                if l.strip() == '[%s]' % section:
                    out += lines
            text = '\n'.join(out) + '\n'
        (DEV / 'profiles' / (name + '.ini')).write_text(text, encoding='utf-8')


def restore():
    for name in ('CBTest', 'Caesar'):
        shutil.copyfile(FIX / (name + '.ini'), DEV / 'profiles' / (name + '.ini'))
    for p in ('Fresh', 'Player'):
        if (FIX / (p + '.ini')).exists():
            shutil.copyfile(FIX / (p + '.ini'), DEV / 'profiles' / (p + '.ini'))
    shutil.copyfile(FIX / 'tm2pc.ini', DEV / 'tm2pc.ini')


def run(exe, name, tag, seconds, shot, env, extra):
    d = OUT / name / tag
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    dev_ini(extra.get('ini', ()))
    profiles(extra)
    e = dict(os.environ, SDL_AUDIO_DRIVER='dummy', SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', SDL_WINDOW_ACTIVATE_WHEN_RAISED='0', TM2PC_SEED='7', TM2PC_STATE_LOG=str(d / 'state.txt'),
             TM2PC_NO_PRECISE_C='1')  # (the hybrid's own subpixel sprites: off, as the plain build)
    e.update(env)  # (a scenario may set its own seed: another random arena)
    if e.get('TM2PC_UI_SCRIPT', '').startswith('-,-,-,x,-,x,'):  # (Local Play, one player: the sign-in without player 2)
        e['TM2PC_SIGNIN_ONE'] = '1'
    log = open(d / 'log.txt', 'w', encoding='utf-8')
    p = subprocess.Popen([str(DEV / exe), '--shot-dir', str(d), '--shots', '%dv' % shot if isinstance(shot, int) else shot], cwd=DEV, env=e,
                         stdout=log, stderr=subprocess.STDOUT)
    try:
        p.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    log.close()
    restore()
    return d


def compare(name, a, b):
    la = [l.split() for l in (a / 'state.txt').read_text().splitlines()] if (a / 'state.txt').exists() else []  # (menus only: none)
    lb = [l.split() for l in (b / 'state.txt').read_text().splitlines()] if (b / 'state.txt').exists() else []  # (menus only: none)
    n = min(len(la), len(lb))
    diff = [f for f in range(n) if la[f][1:3] != lb[f][1:3]]
    shots = sorted(a.glob('vblank-*[0-9].bmp')), sorted(b.glob('vblank-*[0-9].bmp'))
    if shots[0] and shots[1]:
        shot = 'shot equal' if shots[0][0].read_bytes() == shots[1][0].read_bytes() else 'SHOT DIFFERS'
    elif shots[0] or shots[1]:
        shot = 'SHOT ONLY IN ' + ('A' if shots[0] else 'B')  # (the other stopped first)
    else:
        shot = 'no shot'
    errors = [l for d in (a, b) for l in (d / 'log.txt').read_text(errors='replace').splitlines()
              if l.startswith('runtime:') or l.startswith('game called exit')]
    menus_only = not la and not lb and shot == 'no shot'  # (a menu's run: nothing to compare)
    ok = menus_only or (n > 0 and not diff and la[0][1] == lb[0][1] and len(la) == len(lb) and shot in ('shot equal', 'no shot')
          and not errors)
    print('%-11s %s  frames %d/%d, %s%s, %s%s' % (
        name, 'OK  ' if ok else 'FAIL', len(la), len(lb), 'equal' if not diff else 'differ from frame %d' % diff[0],
        '' if not n or la[0][1] == lb[0][1] else ' (start %s/%s)' % (la[0][1], lb[0][1]), shot,
        ', runtime errors: %d' % len(errors) if errors else ''), flush=True)
    return ok


# Baselines (tests/baselines/<scenario>.json): the hybrid's own results of a known-good build, so a later build is
# checked against it (every scenario, the 16-car ones too) without the plain build. Recorded after the last check
# against the plain build (2026-10-13: equal at 11 cars or fewer); --record again after an intended change.
BASELINES = REPO / 'tests' / 'baselines'
BASELINE_SET = ('one,two,teams,kills,mirrors,pause,lives,dm_suicide,koth,feed,jugg_suicide,interp_killcam,'
                'sixteen,sixteen_same,sixteen_teams,sixteen_twister,sixteen_slam,jugg_car,jugg_end,surv2,surv4,surv_boss,'
                'sixteen_a3,sixteen_a8,spect_hud2,spect_hud4,feed2,align1,three')


def result_of(d):
    """a run's result: the state log's frames (frame, VBlank, hash) and each screenshot's hash"""
    text = (d / 'state.txt').read_text() if (d / 'state.txt').exists() else ''
    state = text.splitlines()
    if state and not text.endswith('\n'):  # (the game killed at the run's time mid-write: its last line cut short, a
        state.pop()                         # false "differ" at the end; the user's pause runs, 2026-11-15)
    shots = {f.name: hashlib.sha256(f.read_bytes()).hexdigest()[:16] for f in sorted(d.glob('vblank-*[0-9].bmp'))}
    return {'frames': [' '.join(l.split()[:3]) for l in state], 'shots': shots}


def baseline_check(name, got):
    want = json.loads((BASELINES / (name + '.json')).read_text())
    fa, fb = want['frames'], got['frames']
    diff = next((i for i in range(min(len(fa), len(fb))) if fa[i] != fb[i]), None)
    shots = [k for k in sorted(set(want['shots']) | set(got['shots'])) if want['shots'].get(k) != got['shots'].get(k)]
    # (the frames both runs have: a run's length follows the machine's speed; but under half the baseline's is a run
    # that died: a boot crash once passed as "equal" with 0 frames)
    short = len(fb) < len(fa) // 2
    ok = diff is None and not shots and not short
    print('%-14s %s  frames %d/%d, %s, %s' % (name, 'OK  ' if ok else 'FAIL', len(fa), len(fb),
          'TOO FEW FRAMES' if short else 'equal' if diff is None else 'differ from frame %d' % diff,
          'shots equal' if not shots else 'SHOTS DIFFER: ' + ', '.join(shots)))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--a', default='tm2pc.exe')
    ap.add_argument('--b', default='tm2pc-hooks.exe')
    ap.add_argument('--only', help='scenario names, comma separated')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--b-only', action='store_true', help='run only b (measurements, the hybrid alone)')
    ap.add_argument('--record', action='store_true', help='run b and save its results as the baselines (tests/baselines)')
    ap.add_argument('--check', action='store_true', help='run b and compare it with the baselines')
    args = ap.parse_args()
    if args.list:
        for k, (s, v, _, _) in SCENARIOS.items():
            print('%-11s %4d s, shot at VBlank %s' % (k, s, v))
        return
    FIX.mkdir(parents=True, exist_ok=True)
    for f in ('CBTest.ini', 'Caesar.ini', 'Fresh.ini', 'Player.ini'):
        if not (FIX / f).exists() and (DEV / 'profiles' / f).exists():
            shutil.copyfile(DEV / 'profiles' / f, FIX / f)
    if not (FIX / 'tm2pc.ini').exists():
        shutil.copyfile(DEV / 'tm2pc.ini', FIX / 'tm2pc.ini')
    names = args.only.split(',') if args.only else BASELINE_SET.split(',') if args.record or args.check else list(SCENARIOS)
    bad = 0
    t0 = time.time()
    for name in names:
        seconds, shot, env, extra = SCENARIOS[name]
        if args.record or args.check:
            b = run(args.b, name, 'b', seconds, shot, env, extra)
            got = result_of(b)
            if args.record:
                BASELINES.mkdir(parents=True, exist_ok=True)
                (BASELINES / (name + '.json')).write_text(json.dumps(got, indent=0) + '\n', encoding='utf-8')
                print('%-14s recorded (%d frames, %d shots)' % (name, len(got['frames']), len(got['shots'])))
            else:
                bad += not baseline_check(name, got)
            continue
        if args.b_only:
            run(args.b, name, 'b', seconds, shot, env, extra)
            print('%-11s ran (b only)' % name)
            continue
        a = run(args.a, name, 'a', seconds, shot, env, extra)
        b = run(args.b, name, 'b', seconds, shot, env, extra)
        bad += not compare(name, a, b)
    print('%d of %d scenarios equal (%.0f min); logs and shots in %s' % (len(names) - bad, len(names), (time.time() - t0) / 60, OUT))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
