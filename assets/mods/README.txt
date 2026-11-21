Twisted Metal 2 PC: mods
========================

A mod is a folder of data files (pictures, models, sounds, text) in the mods folder next to tm2pc.exe. Mods never
hold programs or scripts.

    mods/
      characters/<your-character>/   a new driver and car                  (works on a base car's stats: characters/_TEMPLATE)
      skins/<your-skin>/             a new look for an existing car         (works: see skins/_TEMPLATE/GUIDE.txt)
      arenas/<your-arena>/           a new map                              (checked; not loaded yet)
      tournaments/<your-pack>/       a ready-made tournament                (checked; not loaded yet)

Options > Mods lists every mod by kind with an On/Off switch (kept in tm2pc.ini, the same for every player on
this PC); a mod with a problem shows why it cannot be turned on.

Each kind has a _TEMPLATE folder with a GUIDE.txt and an example mod.ini. Copy the template, rename the copy, and
edit it. Folders whose name starts with "_" are never loaded.


mod.ini
-------
Every mod has a mod.ini with a [mod] section:

    [mod]
    id = tm1-mr-grimm            ; lowercase letters, digits and '-': the name other mods use to require this one
    uid = 3f9c2a7e5b1d4c08a6e2f0b9d7c4e1a2   ; 32 lowercase hex digits, made once, never changed (see below)
    name = Mr. Grimm (Twisted Metal 1)       ; shown in the menus
    kind = skin                  ; character, skin, arena or tournament (must match the folder it is in)
    version = 1.0
    author = your name
    game_version = 1             ; the mod format; this game reads 1
    requires =                   ; other mods' ids this one needs, separated by commas

The uid tells your mod apart from every other mod, even one with the same id: 32 random hex digits (128 bits), so
two modders never make the same one. tm2pc.exe --new-mod <kind> <id> makes the folder with a new uid filled in (or
use any tool that prints 32 random hex digits, for example in PowerShell: [guid]::NewGuid().ToString("N")). Make it
once and keep it for every version of the mod. Progress and stats are kept by uid, so a renamed mod keeps them.

Text after ';' on a line is a comment.


Checking a mod
--------------
The game checks every mod when it starts. A mod with a problem is skipped, and tm2pc.log says why, for example:

    mods: ...\mods/skins/my-skin/: missing car.tms

To check one folder while you work on it, run from a command prompt in the game's folder:

    tm2pc.exe --check-mod mods\skins\my-skin

It prints every problem it finds (or OK) and exits without starting the game. "requires" is checked only when the
game starts, since it needs the other mods.


Online
------
A server uses the host's skins and characters that are on. Joining one, the game lists the mods you do not have and asks whether to
download them (up to the limit in Online Services, 50 MB unless you change it). Each file is checked before it is kept:
only the file names a mod of its kind may have, contents that match the name (a model, a texture pack, a picture...),
exactly the server's files, nothing outside its own folder in mods. Downloaded mods stay in your mods folder.

While you are in a server, its mods are on and your others are off; leaving puts your Options > Mods switches back.
