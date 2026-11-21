#!/bin/sh
# Captures the Customize HUD page's preview pictures into extracted/hud-preview
# (hud.png, nohud.png, list.png): one moment of a Challenge Match, three times
# (the same seed and input tape, so the same frame): with the game's HUD,
# without it (TM2PC_HUD_OFF), with the full weapon list (TM2PC_HUD_LIST).
# Then: py tools/art/ui_assets.py --out build/port/ui
#
#   sh tools/art/hud_preview.sh SETTINGS.ini TAPE.txt [VBLANK]
#
# SETTINGS.ini: a test profile's settings (its Challenge Match as the match);
# TAPE.txt: a --tape-vblank input tape that drives the car (the one used held
# X, steered left 40 of every 400 VBlanks and fired every 30). VBLANK: the
# moment (9000: Mr. Slam ahead, targeted, on Los Angeles). Run silently.
set -e
ini=$1 tape=$2 vb=${3:-9000}
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$root/extracted/hud-preview
tmp=$(mktemp -d)
mkdir -p "$out"
cd "$root/build/port-dev"
script="-,-,-,x,-,x,-,down,x,-,x,-,down,down,down,down,x"
for mode in HUD OFF LIST; do
    mkdir -p "$tmp/$mode"
    env SDL_AUDIO_DRIVER=dummy TM2PC_SEED=7 TM2PC_SHOT_NO_VRAM=1 TM2PC_HUD_$mode=1 TM2PC_UI_SCRIPT="$script" \
        ./tm2pc.exe --settings "$ini" "../../references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue" \
        --tape-vblank "$tape" --shot-dir "$tmp/$mode" --shots "${vb}v" --frames $((vb / 3 + 300)) > /dev/null 2>&1
done
name() { case $1 in HUD) echo hud;; OFF) echo nohud;; LIST) echo list;; esac; }
for mode in HUD OFF LIST; do
    f=$(ls "$tmp/$mode"/*.bmp)
    powershell -NoProfile -c "Add-Type -AssemblyName System.Drawing; \$i=[System.Drawing.Image]::FromFile('$(cygpath -w "$f")'); \$i.Save('$(cygpath -w "$out")\\$(name $mode).png', [System.Drawing.Imaging.ImageFormat]::Png); \$i.Dispose()"
done
echo "$out: hud.png nohud.png list.png"
