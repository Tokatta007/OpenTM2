# The online stats for the website

The master servers (`tools/master`) count online matches. The website shows only the **public stats file** each
master writes, `tm2_stats.json`. It never reads the master's own database (`tm2_master.db`), which holds every
player's private key.

## What is counted

- Online matches only, each counted **once two machines report the same result** (one machine alone can't add to
  the stats).
- Modded games aren't counted. Vanilla Plus is counted (the user's rule).
- Guests (players without a player ID) aren't counted; the other players in the match still are.
- Per match: every player's car, whether they won, their kills, deaths and damage dealt.

## Where the file is

The file sits next to the database, and the master rewrites it every time it saves (at most every 30 seconds, and
only when something changed; also when the master stops):

| master | file |
|---|---|
| Droplet (systemd service `tm2-master`) | `/var/lib/tm2-master/tm2_stats.json` |
| NAS (Docker, volume `tm2-master-data`) | `/data/tm2_stats.json` inside the container |

Each master counts only the matches reported to it. Players use every master on their list, so the masters'
numbers overlap rather than add up. The website should show **one master's file** (the droplet's is the main one).
Merging them is a later decision.

Getting it to the website is the website's choice. The simplest is a static file server on the droplet (for example
Caddy or nginx) serving just this one file over HTTPS with `Access-Control-Allow-Origin: *`, which the site fetches
from the browser. A scheduled copy to the site's host also works. The master opens no web port itself.

## The format (version 1)

```json
{
  "format": 1,
  "updated": 1792733084,
  "matches": 1,
  "kills": 4,
  "deaths": 3,
  "damage": 200,
  "cars": [
    {"car": 4, "name": "Grasshopper", "matches": 1, "wins": 0, "kills": 1, "deaths": 2, "damage": 100},
    {"car": 7, "name": "Roadkill", "matches": 1, "wins": 1, "kills": 3, "deaths": 1, "damage": 100}
  ]
}
```

| field | meaning |
|---|---|
| `format` | 1. Raised if a field ever changes meaning; new fields can appear in the same version. |
| `updated` | When the file was written (Unix time, seconds). |
| `matches` | Online matches counted. |
| `kills`, `deaths`, `damage` | Totals over every counted player of every counted match (damage in health points dealt to other cars). |
| `cars` | One entry per car that has been driven in a counted match, in car-select order. A car nobody picked yet is missing. |
| `cars[].car` | The car's index (car-select order, below). |
| `cars[].name` | Its name. |
| `cars[].matches` | Matches a player drove it in (two players on the same car in one match count twice). |
| `cars[].wins` | Matches its driver won. |
| `cars[].kills`, `deaths`, `damage` | Its drivers' totals. |

Car indices: 0 Hammerhead, 1 Outlaw 2, 2 Warthog, 3 Mr. Grimm, 4 Grasshopper, 5 Thumper, 6 Spectre, 7 Roadkill,
8 Twister, 9 Axel, 10 Mr. Slam, 11 Shadow, 12 Sweet Tooth, 13 Minion, 14 Dark Tooth.

Useful figures the site can work out itself: win rate (`wins / matches`), kills per match, K/D (`kills / deaths`),
pick rate (`matches / total matches`).

## Not in the file (yet)

Per-player stats exist in the master's database (by player ID) for a possible opt-in leaderboard later (the user's
plan: no usernames on the site at first). Publishing any of them needs the user's go-ahead and an opt-in setting in
the game first.
