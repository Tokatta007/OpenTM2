# Menu art: clean backgrounds

New and reworked menu pages draw their titles, button legends and labels with a
real font (Averia Serif Libre, free and close to Flexure, the game's own typeface;
see [OPTIONS.md](OPTIONS.md)). The backgrounds must lose their baked-in text for that. Rule: text is
only removed where another version of the same picture shows what is under it;
nothing is painted in.

`tools/art/clean_menus.py` does this for the PC version's 640x480 images
(`extracted/images-pc`, from `tools/disc/extract_images.py --pc`) and writes
`extracted/menus-clean/` with a before/after report (`index.html`).

## How

Thirteen screens share one shattered-glass picture: 1 PLAYER, 2 PLAYER, OPTIONS,
AUDIO, HISTORY and the eight level previews (SHOWOPS1-8), each with its own artwork in
different places. Where at least 3 of them have exactly the same pixel, that pixel is
bare glass. The button legend is identical on the five menus, so in the legend box only
the eight level previews (which have none) vote; elsewhere all thirteen do. Two agreeing
screens are enough for a dark (glass, not text) pixel.

Choose Battleground and the PC version's load, save and audio screens use the same glass but
were saved with slightly different colors (within a few levels), so they cannot vote. Their
text areas are filled from the clean plate shifted to their colors, measured on the glass
around each area.

## Inventory (2026-10-04)

| Screen | Baked-in text | Result |
|---|---|---|
| 1 PLAYER, 2 PLAYER | button legend | clean (10 and 61 pixels without agreement, left as they were) |
| OPTIONS, AUDIO, HISTORY | title, button legend | clean (8-10 pixels without agreement) |
| Clean glass plate | - | 99.3% clean; a patch in the middle (where every screen has artwork) has no source |
| Car select (CARSEL) | button legend over the road | edited (`carsel_road.py`): the legend box is solid black in every version, so the road under it exists nowhere; the box is filled by texture synthesis from real road at the same distance (rows within ~30 of the box: further up the grain is finer), pieces chosen to match their surroundings, lighting and grain contrast, and the center dash is extended along its slant, colored from its own interior. Fallback: our own legend bar |
| Choose Battleground (LEVELSEL) | title, button legend | clean (same glass, saved with slightly different colors: filled from the plate with a color offset; 16 pixels without a source) |
| PC load/save (LOAD, SAVE, with and without boxes) | LOAD GAME / SAVE GAME | clean (same glass in the corner; color-offset fill) |
| PC audio (AUDIO_BG) | title, button legend | clean (color-offset fill; 16 pixels without a source) |
| GLASS2 (PC) | none | already clean (a different glass picture) |
| PC network (NETMAIN): the Network background | title, labels, three boxes | edited (`net_background.py`): the four network screens put the flame in different places, so they can't vote; NETMAIN's glass is GLASS2, darker, at the same place. Under the covered areas the glass is GLASS2 mapped to NETMAIN's colors (scale and offset per channel, fitted on bare glass); the flame's glow is filled in smoothly from around each area (harmonic fill) and its texture mirrored in from just outside, fading within ~10 pixels. Where the two thin boxes cross the flame, the flame is rebuilt from nearby flame (6x6 blocks matched to their surroundings, blended at the edges with a smooth offset). The flame behind the boxes is therefore an invention that fits, not the original. Exported as `ui/network.bmp` |
| Title screen | copyright line | stays (the port draws its menu over the PC title picture: OPTIONS.md) |
| World/level select globe | none | - |

Covering (the fallback) means our own legend bar or title panel, drawn in the game's
style over the same place, with its text in the font.

## New text screens in the co-op ending's lettering

`tools/art/duel_text.py` sets new lines in the lettering of the co-op ending screen
(COWIN: italic Flexure with a drop shadow on the game's emblem), from that screen's own
letters, so it matches exactly without the font license:

    py tools/art/duel_text.py --replace-from 5 --text "Be the last one standing|to earn your reward..." --out DUEL.png

`--replace-from 5` keeps COWIN's first two paragraphs as they are and replaces the third
(its lines 6-8) with the new lines, on the same baselines.

- Letters are cut with their shading and shadow (six of its eight lines split into exactly
  one shape per character; "mode" and "reward" have touching pairs, after the letters used).
- Color: a radial gradient fitted to the original (red within about 150 pixels of the screen
  center, through orange to yellow by about 240; vertical distance counts 0.85), applied at
  each letter's new place while it keeps its own shading.
- Spacing: the closest distance between neighboring letter outlines, word spaces and line
  pitch as measured in the original.
- Background: COWIN's own, except around the letters being replaced, where the emblem comes
  from the credits screens that share it (their text and a margin left out of the vote),
  color-corrected locally (COWIN's tint drifts against theirs across the screen).
- Letters it has: capitals A B D E H I K L M N O R S T W Y, lowercase all but j k q x,
  digit 1, . and ,. Others are reported as missing.

PS1 images (320x240) are the same pictures at half the resolution; the PC images are
used wherever they exist.

The Options pages take their pictures from these results: see [OPTIONS.md](OPTIONS.md) (`tools/art/ui_assets.py`).
