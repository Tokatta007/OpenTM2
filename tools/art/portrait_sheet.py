"""Every profile portrait, named, for reviewing and planning unlocks.

    py tools/art/portrait_sheet.py [--ui build/port/ui] [--out extracted/portraits-review]

Reads the portraits the game uses (ui/portraits.bmp, portraits_hq.bmp,
movie_portraits.bmp and .txt, from ui_assets.py) and writes:

  <NNN>_<Who>_<Kind>_<n>.png  each portrait, named as the game names it
                              ("Roadkill - Portrait 1", "Calypso - Intro 2", "Other - 5")
  index.html                  all of them, grouped by person, with their names
  portrait_unlocks.xlsx       one row per portrait: name, the unlock to decide,
                              today's rule, the picture (a link), its source

Ordered as the game's grid: the drivers in car select's order (Roadkill
first; Sweet Tooth and Minion last), each driver's three portraits then their
ending's stills; then Calypso's stills; then other people's. The output is the
game's art: keep it out of the repository.
"""
import argparse
import html
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "art"))
from clean_menus import save_png  # noqa: E402
from movie_portraits import load_bmp  # noqa: E402

DRIVERS = ["Hammerhead", "Outlaw 2", "Warthog", "Mr. Grimm", "Grasshopper", "Thumper", "Spectre",
           "Roadkill", "Twister", "Axel", "Mr. Slam", "Shadow", "Sweet Tooth", "Minion"]
KEYS = ["hammerhead", "outlaw2", "warthog", "grimm", "grasshopper", "thumper", "spectre",
        "roadkill", "twister", "axel", "slam", "shadow", "sweettooth", "minion"]
ENDING_MOVIES = ["MONSTERT", "COPCAR", "HUMVEE", "MRGRIM", "DUNE", "IMPALLA", "VETTE",
                 "ROADKILL", "INDY", "AXEL", "FRONTL", "HEARSE", "SWEET", "MINION"]
CAR_SELECT = [7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 12, 13]  # Roadkill first; the secret two last


def frames(strip, n):
    w = len(strip[0]) // n
    return [[row[i * w:(i + 1) * w] for row in strip] for i in range(n)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ui", type=Path, default=ROOT / "build/port/ui")
    ap.add_argument("--out", type=Path, default=ROOT / "extracted/portraits-review")
    a = ap.parse_args()
    cs = frames(load_bmp(a.ui / "portraits.bmp"), 28)
    hq = frames(load_bmp(a.ui / "portraits_hq.bmp"), 14)
    lines = [l.split() for l in (a.ui / "movie_portraits.txt").read_text(encoding="utf-8").splitlines()
             if l and not l.startswith("#")]
    lines_all = lines  # PROMO lines: a driver's extra portraits (Portrait 4..)
    movies = frames(load_bmp(a.ui / "movie_portraits.bmp"), len(lines))

    # each movie still: who, intro or ending, and its number as the game counts it
    stills = []
    for i, (sid, who, movie) in enumerate(lines):
        if movie == "PROMO":
            continue
        owner = ENDING_MOVIES.index(movie) if movie in ENDING_MOVIES else -1
        intro = owner < 0
        k = sum(1 for (_, w, m) in lines[:i + 1]
                if w == who and (who == "other" or (m not in ENDING_MOVIES) == intro))
        stills.append({"id": sid, "who": who, "owner": owner, "intro": intro, "n": k, "img": movies[i]})

    def rule_for(owner):
        if owner == 12: return "Sweet Tooth's ending (hidden until Sweet Tooth is unlocked)"
        if owner == 13: return "Minion's ending (hidden until Minion is unlocked)"
        return f"{DRIVERS[owner]}'s ending (win the tournament on Medium or Hard)"

    rows = []  # (group, name, rule, image, source)
    for car in CAR_SELECT:
        d = DRIVERS[car]
        secret = car >= 12
        for n in range(3):
            img = hq[car] if n == 2 else cs[car * 2 + n]
            if n == 0:
                rule = f"Free once {d} is unlocked" if secret else "Free"
            else:
                rule = f"{d}'s ending (for now)"
            rows.append((d, f"{d} - Portrait {n + 1}", rule, img,
                         "info screen (BIOS)" if n == 2 else "car select (CARSEL.TPC)"))
        for k, l in enumerate([l for l in lines_all if l[2] == "PROMO" and l[1] == KEYS[car]]):
            rows.append((d, f"{d} - Portrait {4 + k}", rule_for(car), movies[lines_all.index(l)], l[0]))
        for s in stills:
            if s["who"] == KEYS[car]:
                rows.append((d, f"{d} - {'Intro' if s['intro'] else 'Ending'} {s['n']}",
                             "To be decided" if s["intro"] else rule_for(s["owner"]), s["img"], s["id"]))
    for s in stills:
        if s["who"] == "calypso":
            rows.append(("Calypso", f"Calypso - {'Intro' if s['intro'] else 'Ending'} {s['n']}",
                         "To be decided" if s["intro"] else rule_for(s["owner"]), s["img"], s["id"]))
    for s in stills:
        if s["who"] == "other":
            rows.append(("Other", f"Other - {s['n']}",
                         "To be decided" if s["intro"] else rule_for(s["owner"]), s["img"], s["id"]))

    a.out.mkdir(parents=True, exist_ok=True)
    for old in a.out.glob("*.png"):
        old.unlink()
    files = []
    for k, (_, name, _, img, _) in enumerate(rows, 1):
        f = f"{k:03d}_" + name.replace(" - ", "_").replace(". ", "_").replace(" ", "_").replace(".", "") + ".png"
        save_png(a.out / f, img)
        files.append(f)

    # the gallery
    h = ["<!doctype html><html lang='en'><head><meta charset='utf-8'><title>Profile Portraits</title>",
         "<style>body{background:#141416;color:#ddd;font:14px system-ui,sans-serif;margin:16px}"
         "h2{color:#fc6;margin:28px 0 8px}.g{display:flex;flex-wrap:wrap;gap:10px}"
         "figure{margin:0;width:120px}img{width:120px;height:240px;display:block;border:1px solid #444}"
         "figcaption{font-size:11px;margin-top:4px}small{color:#999}</style></head><body>",
         f"<h1>Profile portraits ({len(rows)})</h1><p>In the game's order. Names as the game shows them; "
         "file names in <code>portrait_unlocks.xlsx</code>.</p>"]
    group = None
    for (g, name, rule, _, src), f in zip(rows, files):
        if g != group:
            if group is not None:
                h.append("</div>")
            h.append(f"<h2>{html.escape(g)}</h2><div class='g'>")
            group = g
        h.append(f"<figure><img src='{f}' alt='{html.escape(name)}'><figcaption><b>{html.escape(name)}</b>"
                 f"<br><small>{html.escape(rule)}<br>{html.escape(src)}</small></figcaption></figure>")
    h.append("</div></body></html>")
    (a.out / "index.html").write_text("\n".join(h), encoding="utf-8")

    # the spreadsheet
    from openpyxl import Workbook
    from openpyxl.styles import Alignment, Font, PatternFill
    wb = Workbook()
    ws = wb.active
    ws.title = "Portraits"
    ws.append(["Portrait", "Unlock (to decide)", "Today's rule", "Picture", "Source"])
    for c in ws[1]:
        c.font = Font(bold=True, color="FFFFFF")
        c.fill = PatternFill("solid", fgColor="7A3E12")
    for (g, name, rule, _, src), f in zip(rows, files):
        ws.append([name, "", rule, f, src])
        cell = ws.cell(row=ws.max_row, column=4)
        cell.hyperlink = f
        cell.font = Font(color="0563C1", underline="single")
    for col, w in zip("ABCDE", (28, 48, 52, 40, 24)):
        ws.column_dimensions[col].width = w
    for row in ws.iter_rows(min_row=2, min_col=2, max_col=2):
        for c in row:
            c.alignment = Alignment(wrap_text=True)
    ws.freeze_panes = "A2"
    wb.save(a.out / "portrait_unlocks.xlsx")
    print(f"{a.out}: {len(rows)} portraits, index.html, portrait_unlocks.xlsx")


if __name__ == "__main__":
    main()
