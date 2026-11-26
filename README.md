<p align="center">
  <a href="https://youtu.be/QdJrzCkjSc8"><img src="https://img.youtube.com/vi/QdJrzCkjSc8/maxresdefault.jpg" alt="OpenTM2 release trailer" width="640"></a><br>
</p>
<h3 align="center"><a href="https://discord.gg/SNBEBt9geQ">💬 Join the Discord</a></h3>
<p align="center">
  <a href="https://youtu.be/QdJrzCkjSc8">▶ Watch the release trailer</a> · <a href="https://opentm2.pages.dev">Website: opentm2.pages.dev</a>
</p>

<p align="center"><a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a></p>

A PC port of **Twisted Metal 2** (PlayStation, 1996) for Windows. The game's own code is recompiled from your copy of
the game and runs on a native PC runtime, with online play, split-screen and many optional improvements.

OpenTM2 includes no game files. You need your own copy of Twisted Metal 2 for the PlayStation (USA).

## Features

- Online play over the internet or LAN with rollback netcode, a server browser, friends and playlists.
- Up to four players in split-screen, and up to 16 cars in a match.
- Challenge Matches with custom game types (Deathmatch, Juggernaut, Gun Game, Hunted, King of the Hill, teams),
  custom and versus tournaments, and Survival.
- Widescreen, high resolutions and a customizable HUD.
- **Vanilla Plus:** fixes and improvements to the original game, each one switchable
  ([docs/VANILLA_PLUS.md](docs/VANILLA_PLUS.md)). The original preset plays as the PlayStation game does.
- **[Balance Changes](BALANCE_CHANGES.md):** every change to weapons and characters, update by update. Always on
  online, so everyone plays by the same rules.
- Mods: new arenas, characters and skins. The guides are in the game's `mods` folder and in [assets/mods](assets/mods)
  (start with [README.txt](assets/mods/README.txt)); [docs/MODDING.md](docs/MODDING.md) has the design notes.

## Playing

1. Download the latest release from [Releases](https://github.com/Tokatta007/OpenTM2/releases) and unzip it anywhere.
2. Start `tm2pc.exe`. The first time, a setup window asks for your game discs:
   - **The PlayStation disc (USA), needed:** a disc image as a `.cue` file with its `.bin` file(s). Extract the `.cue`
     and all its `.bin` files into one folder, then choose the `.cue`.
   - **The 1997 PC version, optional, but highly recommended:** its disc image (`.iso`, `.cue`/`.bin`, `.mdf`/`.mds` or `.img`/`.ccd`) or installed folder. With it, the menus and arena textures are
     sharper.
3. OpenTM2 makes its menu pictures from your discs (nothing made from them leaves your PC) and starts the game.

New versions are offered when you start the game (`check = off` under `[update]` in `tm2pc.ini` turns that off).
With [RecompLauncher](https://recomplauncher.com), the launcher asks for your discs, runs the setup and installs the
updates itself.

## Building from source

You need Windows, CMake, Ninja, Python 3, and an llvm-mingw clang. Clone with the submodules:

```sh
git clone --recurse-submodules https://github.com/Tokatta007/OpenTM2.git
```

**The recompiled build** (from the retail executable): put your own `SCUS_943.06` from the PlayStation disc at
`references/TM2-Decomp/bin/SCUS_94306R.EXE`, then:

```sh
cmake -S . -B build/port -G Ninja -DCMAKE_C_COMPILER=<llvm-mingw clang>
cmake --build build/port
```

**The release build (hybrid):** the game's code comes from the
[TM2 decompilation](https://github.com/Tokatta007/TM2-Decomp)'s source with OpenTM2's changes in C (`decomp/`). Set up
and build the decomp first (its README: the game files and its toolchain), then:

```bat
python tools/decomp/wc.py sync
python tools/decomp/wc.py build
set OUT=%CD%/build/decomp-wc/out
cmake -S . -B build/port -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=<llvm-mingw clang> ^
  -DTM2_EXE=%OUT%/TM2_SOURCE_PORT.EXE -DTM2_ADDRMAP=%OUT%/TM2_SOURCE_PORT/addrmap.tsv ^
  -DTM2_INVENTORY=%OUT%/TM2_SOURCE_PORT/recomp_inventory.json -DTM2_PORT_JSON=%OUT%/TM2_SOURCE_PORT.json
cmake --build build/port
```

Details: [docs/RUNTIME.md](docs/RUNTIME.md) (the runtime), [docs/HYBRID.md](docs/HYBRID.md) (the hybrid build),
[docs/RELEASE.md](docs/RELEASE.md) (releases and the updater).

## Layout

| Path | Contents |
|---|---|
| `src/` | The PC runtime, menus, netcode, Vanilla Plus and the game's settings |
| `tools/recomp/` | The recompiler (MIPS to C) and its tools |
| `decomp/` | OpenTM2's changes to the decomp's C, for the hybrid build |
| `references/TM2-Decomp/` | The [TM2 decompilation](https://github.com/Tokatta007/TM2-Decomp) (submodule) |
| `third_party/` | PSY-Z (with our patches in `third_party/patches/`), Opus, and single-file libraries |
| `assets/` | Fonts, mode symbols and the mod templates |
| `tools/` | Asset, test, network-simulation and master-server tools |
| `docs/` | Design documents |

## Related projects

- [TM2-Decomp](https://github.com/Tokatta007/TM2-Decomp): the Twisted Metal 2 decompilation OpenTM2 builds on.
- [Rogue Trip: Vacation 2012 partial decompilation](https://github.com/Tokatta007/Rogue-Trip-Vacation-2012-Decomp):
  the same engine family.

## License

OpenTM2's own code is under the MIT License ([LICENSE](LICENSE)). Twisted Metal 2, its code and content are © their
respective owners; no game files are included and no license to the game is granted. Third-party code keeps its own
license. The menu lettering (`assets/fonts/flexure.atlas`) is used by permission of T26: it isn't covered by the MIT
License, may be used only in OpenTM2 and its derivatives, and no Flexure font file may be added
([its notice](assets/fonts/Flexure-NOTICE.txt)).
