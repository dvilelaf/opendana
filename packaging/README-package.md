# OpenDana — native re-implementation of Solomon's Key 2 / Fire 'n Ice (EU)

An unofficial, from-scratch re-implementation in C11 of the NES puzzle game
*Solomon's Key 2 / Fire 'n Ice* (EU/PAL edition), with a moddable content
package. This package contains **only the program**: no ROM and no game data.
You need your own copy of the EU ROM, which the game imports **once**.

`opendana --version` (or `opendana.exe --version`) prints the version and the exact git
commit the program was built from.

## 1. Start the game

- **Linux (tar.gz):** unpack it anywhere and run `bin/opendana`. Keep `bin/` and
  `lib/` together: the bundled SDL2 is found through `lib/`.
- **Windows (zip):** unpack it anywhere and run `bin\opendana.exe`. Keep `SDL2.dll`
  next to `opendana.exe`.

Requirements: a 64-bit x86 system — Linux with X11 or Wayland, or Windows
10/11.

## 2. Import your ROM (once)

The first start shows the import screen: drag your
ROM file onto the window or choose it with the file
dialog. From a terminal:

```sh
bin/opendana --import-rom "/path/to/your-rom.nes"
```

Only the EU ROM with the expected SHA-256 is accepted; anything else is refused
without changing anything. The ROM is read once; the extracted content stays on
your computer and the ROM is not needed afterwards. Where things are kept:

| | Imported content | Saves, save states, settings |
|---|---|---|
| Linux | `$XDG_DATA_HOME/opendana/content/base` (default `~/.local/share/opendana/content/base`) | `~/.local/share/opendana/` (`save.dat`, `states/`) and `$XDG_CONFIG_HOME/opendana/settings.ini` (default `~/.config/...`) |
| Windows | `%LOCALAPPDATA%\opendana\content\base` | **known defect (C6.22 PUB-12):** without `HOME`/`XDG_DATA_HOME` they are written to the current working directory (`opendana.sav`, `opendana-settings.ini`, `slot-N.odstate`); start the game from a folder you can write to |

`--content DIR` uses another imported package, `--save FILE` another save file.

## 3. Play

The first time a game starts, a hint at the bottom of the window says
"Esc: menu | F1: controls". `F1` (or Esc, then Controls) shows the full table
at any time; Esc closes it. The keys:

| Key | Action |
|---|---|
| Arrows | move, jump |
| `Z` or `Space` | A (wand) |
| `X` | B (wand) |
| `Enter` | START |
| Right `Shift` | SELECT |
| `Esc` | menu (also the gamepad's Guide button) |
| `M` | mute |
| `F5` / `F9` | save / load a state in the current slot |
| `F6` / `F7` | previous / next slot |
| `F12` | screenshot |
| `F2` | the console's RESET button |
| `F11` | fullscreen |
| `F1` | the controls screen |

The menu (Esc) has Continue, States, Editor, Levels, Settings, Controls,
New game and Quit. New game asks first; it keeps your old save as a backup.
In the level editor: `Ctrl+Z` / `Ctrl+Y` undo and redo, `Ctrl+S` saves, `F5`
tests the level, left click paints, right click erases, the mouse wheel
changes the material and middle click picks the material under the cursor.
`--help` lists every option and these keys.

## 4. Mods

A mod is a directory laid over the imported content:

```sh
bin/opendana --mod /path/to/mymod
bin/opendana --validate /path/to/mymod
```

The format is documented in `docs/MODDING.md` inside the source archive.

## 5. Licence, notices and source code

- The game is free software under the **GNU General Public License, version 3
  or later** (`LICENSE`, next to this README). `NOTICE` is the project's
  intellectual-property statement.
- `license-bundle/` holds every third-party notice that applies to the shipped
  files (`notices/`: SDL2 and the code compiled into it, cJSON, stb; on Windows
  also the MinGW-w64 runtime members linked into the executables) and the
  **complete corresponding source code**: `sources/application.tar.xz` is the
  `git archive` of the exact commit printed by `--version`; SDL2, cJSON and stb
  sources, build and relink materials are next to it. `SOURCES.json` lists every
  component, version, licence and SHA-256; `RELINK.txt` explains how to rebuild
  the game or replace SDL2.
- Every package is checked with the project's ROM auditor before it is
  published; the audit report (`*.audit.json`) is published next to the
  package.

## 6. Reproducibility

The archive is reproducible: built from the same commit with the same SDL2
binary, toolchain and `SOURCE_DATE_EPOCH`, the packaging scripts give the same
bytes (sorted members, fixed owner, time and permissions). Verify a download
with `sha256sum -c SHA256SUMS`.

## 7. AppImage

`opendana-<version>-x86_64.AppImage` is a single executable file: make it executable and
run it. It carries the same game as the tar package and the same licence bundle, plus the
notices and sources of its runtime. The runtime statically links libfuse (LGPL-2.1),
squashfuse, zstd, zlib, mimalloc and musl; it is built reproducibly from the pinned sources
in `packaging/appimage-runtime/`, and `opendana-<version>-appimage-runtime-source.tar.xz`
(published next to the image) holds those sources and the scripts to rebuild or relink it
with a modified libfuse. If your system has no FUSE, run it with `APPIMAGE_EXTRACT_AND_RUN=1`.

*Solomon's Key* and *Fire 'n Ice* are trademarks of their respective owners.
This project is not affiliated with or endorsed by them.
