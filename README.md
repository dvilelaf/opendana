# OpenDana

OpenDana is a native, moddable reimplementation of a classic NES puzzle game.
It is written in C11 with a deterministic core, runs on Linux and Windows, and
adds a built-in level editor, save states, gamepad support and a mod system.

OpenDana does not include the game. It needs **your own copy of the European
ROM**, which it imports once (see [Requirements / legal](#requirements--legal)).

## Features

- A deterministic C11 core with no SDL dependency; the SDL2 front end is a thin
  layer on top.
- A level editor and a level catalogue, both driven with the mouse.
- Save states in numbered slots, a mute key, fullscreen and gamepad support.
- Mods: a content package of JSON and PNG files that you can replace or
  overlay, with schemas and a validator.
- A command-line API for external tools: validate, play and render a level file.

## Requirements / legal

OpenDana is an independent, unofficial project. It is not affiliated with or
endorsed by the owners of the original game, and it contains **no** ROM, no
graphics, no text and no level data of that game. The names and trademarks of
the original belong to their owners (see [`NOTICE`](NOTICE)).

To play you need a legally obtained copy of the ROM of the European release of
*Solomon's Key 2 / Fire 'n Ice*. OpenDana accepts only the file whose SHA-256 is:

```
853646c50889dfb7a74c2acf7cf3b76f42d044745433f706dca33f47d826cd1d
```

Check yours with `sha256sum FILE` (Linux) or `certutil -hashfile FILE SHA256`
(Windows). Any other file is refused without changing anything.

## Install

Download a release and pick one:

| Package | How to start |
|---|---|
| Linux **TAR** (`opendana-X.Y.Z-Linux-x86_64.tar.gz`) | unpack it anywhere and run `bin/opendana`; keep `bin/` and `lib/` together |
| Linux **AppImage** | `chmod +x` the file and run it |
| Windows **ZIP** (`opendana-X.Y.Z-Windows-x86_64.zip`) | unpack it anywhere and run `bin\opendana.exe`; keep `SDL2.dll` next to it |

All packages are for 64-bit x86. Each one carries the licence, the third-party
notices and the corresponding source code. Verify a download with
`sha256sum -c SHA256SUMS`. `opendana --version` prints the version and the
commit it was built from.

## First run: import your ROM

The first start shows an import screen. Press **Choose ROM...** and pick your
file, or **drag the ROM onto the window**. If your system has no file chooser,
or as soon as you start typing, the screen shows a field for the path. You can
also import
from a terminal:

```sh
opendana --import-rom /path/to/your-rom.nes
opendana /path/to/your-rom.nes      # import, then start
```

The import is deterministic and all-or-nothing: if it fails, nothing is left
half written. It is checked against a manifest of file hashes built into the
program. The ROM is read once and never copied; afterwards you do not need it.
If a later version of OpenDana needs different content, the game tells you and
asks you to import again.

Where things are stored:

| | Linux | Windows |
|---|---|---|
| imported content | `$XDG_DATA_HOME/opendana/content/base` (default `~/.local/share/opendana/...`) | `%LOCALAPPDATA%\opendana\content\base` |
| saves, states, screenshots | `$XDG_DATA_HOME/opendana/` | `%LOCALAPPDATA%\opendana\` |
| settings (`settings.ini`) | `$XDG_CONFIG_HOME/opendana/` (default `~/.config/...`) | `%APPDATA%\opendana\` |

`--content DIR`, `--save FILE` and `--settings FILE` use other locations.

## Controls

Keyboard to NES pad:

| Key | Pad |
|---|---|
| Arrows | D-pad |
| `Z` or `Space` | A |
| `X` | B |
| `Enter` | START |
| Right `Shift` | SELECT |

Interface keys (they never reach the simulation):

| Key | Action |
|---|---|
| `Esc` | menu |
| `F1` | controls screen |
| `F2` | console RESET |
| `F5` / `F9` | save / load a state in the current slot |
| `F6` / `F7` | previous / next slot |
| `M` | mute |
| `F11` | fullscreen |
| `F12` | screenshot |

Gamepads work through SDL's game controller mapping. `opendana --help` lists
every option.

## Level editor

`opendana --editor` (or **Levels** in the menu) opens the editor: a toolbar, a
palette of materials, mouse painting (left click paints, right click erases,
middle click picks, the wheel changes material), undo and redo (`Ctrl+Z`,
`Ctrl+Y`), `Ctrl+S` to save and `F5` to test the level in the same window. The
catalogue stores each level as a JSON file and lets you play, edit, duplicate,
delete, import and export levels.

## Mods

Everything the game shows comes from a content package of JSON and PNG files
(schemas in [`schema/`](schema)). A mod is a directory with a `mod.json` that
adds or replaces some of those files; mods are layered on top of the package
and the last one wins.

```sh
opendana --mod ejemplo                     # the example mod, mods/ejemplo
opendana --mod /path/to/my-mod             # any directory with a mod.json
opendana --validate /path/to/my-mod        # check a package, a mod or a level file
opendana --play-level my-level.json --round 3
opendana --render-level my-level.json --out level.png
```

A mod never needs to contain anything of the original game, and yours must not.
Read the guide: [`docs/MODDING.md`](docs/MODDING.md).

## Building from source

You need a C11 compiler, CMake 3.16 or later and the SDL2 development files
(for example `libsdl2-dev`). Nothing is downloaded during the build.

```sh
bash build.sh                       # build/opendana
SDL2_DIR=/path/to/sdl2 bash build.sh   # use an SDL2 installed elsewhere
```

This also builds `build/libopendana_core.a` (the core, no SDL) and the unit test
runner. To check your clone:

```sh
build/opendana_tests_public --category unit
```

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the rest of the tests and the
rules for contributions, and [`docs/RELEASE.md`](docs/RELEASE.md) for how the
packages are built.

## Repository layout

```
include/opendana/   public headers of the core
src/core/           deterministic C11 core: game, scenes, flow, audio, content
src/ext/            extension layer without SDL: editor, catalogue, states, settings
src/import/         importer of the user's ROM
src/platform/       portable paths
src/render/         presentation and PNG writer
src/app/            SDL2 layer: window, input, audio, interface
schema/             JSON schemas of the content package and of mods
mods/ejemplo/       example mod
packaging/          package scripts, icon, notices
tests/, tools/      tests and tools
external/           vendored libraries with their licences
```

## Licence

The code is free software under the **GNU General Public License, version 3 or
later**: see [`LICENSE`](LICENSE). Third-party components and their licences are
in [`NOTICE`](NOTICE). The imported game content is yours, for personal use, and
is never part of this project.
