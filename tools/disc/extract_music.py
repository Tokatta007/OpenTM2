"""Save TM2's music tracks (the .DA files) as WAV, to listen to them.

    py tools/disc/extract_music.py "references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue"

Writes extracted/music/<NAME>.wav (44.1 kHz stereo, as on the disc). The
names are the keys of the [music] section in tm2pc.ini. The .DA files
point into the audio tracks; with one .bin per track, a disc position is
found by adding up the .bin sizes, as the runtime's cd.c does.
"""
import argparse
import re
import wave
from pathlib import Path

from cdimage import RAW, CdImage


def track_bins(cue):
    """(path, first disc sector, sector count) for each FILE in the .cue."""
    out, start = [], 0
    for m in re.finditer(r'^\s*FILE\s+"([^"]+)"', cue.read_text(encoding="utf-8", errors="replace"), re.M):
        path = cue.parent / m.group(1)
        sectors = path.stat().st_size // RAW
        out.append((path, start, sectors))
        start += sectors
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cue", type=Path)
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[2] / "extracted/music")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    bins = track_bins(args.cue)
    img = CdImage(args.cue)
    for f in img.files:
        if not f.path.upper().endswith(".DA"):
            continue
        sectors = f.size // 2048
        for path, start, count in bins:
            if start <= f.lba < start + count:
                break
        else:
            print(f"{f.path}: not in any track")
            continue
        name = Path(f.path).stem
        with open(path, "rb") as src, wave.open(str(args.out / f"{name}.wav"), "wb") as dst:
            dst.setnchannels(2)
            dst.setsampwidth(2)
            dst.setframerate(44100)
            src.seek((f.lba - start) * RAW)
            dst.writeframes(src.read(min(sectors, start + count - f.lba) * RAW))
        print(f"{name}: {sectors / 75:.0f} s")
    img.close()


if __name__ == "__main__":
    main()
