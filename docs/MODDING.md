# MODDING: how to make a mod for OpenDana

This guide is for content authors. You do **not** need to read or rebuild the C
code to make a mod: everything the game shows comes from a **content package**
made of JSON and PNG files, and a mod is a directory that adds or replaces some
of those files.

**The package is not in this repository.** It is created on your machine when
you import your own ROM once (`opendana --import-rom FILE`, or the first-run screen;
see the README). It lives in your user folder (`.../opendana/content/base`).
Below, `content/base` means *that imported package*. A mod never needs to
contain a byte of the original game, and the repository's example mods do not.

The documented example is [`mods/example`](../mods/example): it gives level 1
a layout of its own and the player character a striped sprite of its own. Read it side by side
with this guide.

> **CLI.** `--validate`, `--play-level`, `--render-level FILE --out PNG` and
> `--screenshot` are in the product (`build/opendana --help`). They are the API for an
> external level editor. The game also has a built-in mouse level editor and a
> level catalogue (see the README).

---

## 1. The content package in one minute

```
content/base/
  pack.json                     the manifest (lists every file)
  levels/world-01.json … -15.json   the 150 levels, 10 per file
  worlds.json                   per-world cell graphics and palette
  palettes/master.json          the RGB master palettes
  graphics/chr/chr-00…07.png    the CHR tile sheets (grey 8-bit PNG: 0/85/170/255)
  graphics/sprites/poses.json   the player character's metasprites and animation tables
  audio/sequence.json           the music and SFX stream
  screens/*.json                one file per screen: the final arena, the retry
                                screen, the world map, the menu backdrop, the password
                                and good-night scenes, the per-world cinematics
```

A level's objects (`objects`, including the moving flames) are played exactly
as the level file writes them, so a mod may move, add or remove them.

A **mod** is a directory with a `mod.json` plus the files it adds or replaces:

```
mods/example/
  mod.json
  levels/1-1.json                  a level patch: replaces level 1 only
  graphics/patches/player.png      a CHR patch: replaces 14 tiles only
```

The game builds a **layered VFS**: layer 0 is `content/base`, then each `--mod`
in order. When two layers have the same relative path, **the last one wins**.
A mod can therefore replace a whole file by path (a world file, a palette, a
CHR sheet), but it can also change **part** of the package without
shipping the rest: a **level patch** replaces single levels and a **CHR patch**
replaces single tiles (§3 and §4). Those two are the formats to prefer, because
they keep every other level and tile of the package untouched.

Every file a mod declares must exist inside the mod (no `..`, no absolute path)
and must be one the package reads; otherwise the game and `--validate` both
reject the mod and say which mod and why.

Every JSON file has a schema in [`schema/`](../schema). The schemas are the
contract; the example mod validates against them (see §7).

---

## 2. Run the game with a mod

```sh
# windowed
build/opendana --mod mods/example

# the same mod by its ID (found in mods/<id> beside the package, any CWD)
build/opendana --mod example

# headless: write one PNG of the mod's level 1 (graphics overlay + its level file)
build/opendana --headless --mod example --play-level mods/example/levels/1-1.json \
    --round 1 --screenshot 1-1-mod.png

# several mods, later ones win
build/opendana --mod mods/example --mod mods/other
```

Useful options:

| Option | What it does |
|---|---|
| `--content DIR` | use another base package (default: the one imported in your user folder; also `OD_CONTENT`) |
| `--mod DIR` or `--mod ID` | overlay a mod directory, or the mod `mods/<ID>` (repeatable, last wins) |
| `--play-level FILE --round N` | play one world file / one round, the editor loop; the first tick is the round's materialisation frame, as in the real flow |
| `--validate PATH` | check a pack, a mod or a level file and exit |
| `--render-level FILE --out PNG` | render a level file to a 256×240 PNG thumbnail |
| `--headless --screenshot F` | no window; write one frame to `F` |

The game binary has no level-jump option: the full game reaches every level from
power-on with the buttons. Use `--play-level` to try a level file directly.

---

## 3. Change a level: the level patch

Levels live in `levels/world-NN.json`, ten per file (`NN` = 01…15), and the shape
is in [`schema/level.schema.json`](../schema/level.schema.json). A mod does not
have to copy a world file. A file in the mod's `levels` list that is **not** a
package world file (`levels/world-NN.json`) is a **level patch**: it has the
world-file shape, each entry carries an explicit `"number"` (1…150), and that
entry replaces the package level with the same number and nothing else.
`mods/example/levels/1-1.json` is a complete one:

```json
{
  "schema_version": 1,
  "id": "example-1-1",
  "world": 1,
  "grid_width": 16,
  "grid_height": 15,
  "levels": [
    {
      "index": 0,
      "number": 1,
      "world": 1,
      "round": 1,
      "name": "1-1",
      "player": { "x": 3, "y": 6 },
      "metadata": 2,
      "grid": [
        "2222222222222222",
        "1111111111111111",
        "… 15 rows of 16 hex digits …"
      ],
      "objects": [
        { "type": 5,  "positions": [[4,6],[12,11]] },
        { "type": 14, "positions": [[10,6],[4,11]] }
      ]
    }
  ]
}
```

* `number` — the level, 1…150. `index`, `world` and `round` must agree with it
  (`index` = `number - 1`, 10 rounds per world); a disagreement is an error.
  Patching the same number twice, in one mod or across two mods, is an error too.
* `grid` — **15 rows × 16 columns**, one hex digit per cell: the final cell type
  (`0x0`…`0xF`). The names are in `src/core/level.h`; the ones you will use most:
  `0` empty floor, `1` solid wall, `2`…`5` ice, `0xE` fire.
* `player` — the player's start cell `(x, y)`, `x` 0…15, `y` 0…14. **Exactly one
  start per level**, and the level needs at least one fire.
* `objects` — runs of `{"type": T, "positions": [[x,y], …]}`, in the order the
  game plays them. The game rebuilds the level from `grid` **and** the object
  stream, so when you add or move a block, a fire or a monster, update both.
* `metadata` — a byte the game keeps verbatim.

A level patch can hold as many entries as you like, each with its own `number`.
Because the other levels are not in the mod, they stay exactly as the package
has them.

**Replacing a whole world** still works: a file at a package world path
(`levels/world-03.json`) replaces that world entirely, so it must contain all its
10 levels. Use it only when you really change all ten.

The easiest way to author a level is the game's own editor
(`build/opendana --editor`): it saves the level as a JSON file in the catalogue, and
that file has the same shape (give its entry the `number` to patch before using
it as a mod).

---

## 4. Change the player character: the CHR patch

Characters are **CHR tiles**: 8×8 images of 4 colours, 8192 of them in the
package (sheets `graphics/chr/chr-NN.png`, 1024 tiles each). **The player character is in tiles
`0x400`…`0x40D`** (the first 14 tiles of the second sheet). Index `0` of the
sprite palette is transparent.

A mod does **not** need to ship a sheet. It declares a **CHR patch** in
`mod.json`:

```json
"graphics": {
  "chr_patches": [ { "png": "graphics/patches/player.png", "tile": 1024 } ]
}
```

* The PNG holds **whole 8×8 tiles**, laid out row by row, so its width and
  height are multiples of 8. the example's PNG under `mods/example/graphics/patches/` is
  112×8: 14 tiles in a row.
* The tiles are written over the package CHR starting at global tile `tile`
  (0…8191); the patch may not run past tile 8191.
* Every pixel is **grey 0 / 85 / 170 / 255** (the four colours, darkest to
  lightest) or **fully transparent** (keep the package's pixel there).
* Nothing else in the CHR changes. Loading the mod again, or stacking it with
  another mod, gives the same tiles.

So the example repaints the player with a striped diamond of its own and leaves every
other tile untouched. Which tiles belong to the player is data, not magic:
`src/core/compose.c` reads the pose tables in `graphics/sprites/poses.json`, and
the world-1 sprite bank is `poses.json`'s `world_sprite_banks[0]`.

> **Do not distribute the original graphics.** A full CHR sheet keeps the
> original in every tile you do not repaint, and the repository's auditor
> (`tools/audit/rom_free.py`) flags it. Use a patch with art you drew yourself.
> A mod that replaces whole sheets (`graphics.chr`) still loads, but then it is
> yours to make sure it contains nothing taken from the original.

---

## 5. Change a palette

`palettes/master.json` holds the master palettes, each with **64 RGB entries**
written as `0x00RRGGBB`. The game plays the **`pal`** palette. A mod can tint it by shipping its own
`palettes/master.json`:

```json
{
  "schema_version": 1,
  "id": "master",
  "master": [
    { "id": "pal", "name": "PAL 2C07 (modded)", "colors": [ … 64 ints … ] }
  ]
}
```

The example mod does not change the palette; `content/mods/test-gfx` (a small test mod) does.

---

## 6. Advanced: per-world graphics

`worlds.json` maps each cell type of each world to CHR tiles and a background
palette. Change it only if you need new terrain art; the schema is
[`schema/worlds.schema.json`](../schema/worlds.schema.json). The CHR tile
numbers in it are global tile indices into the sheets.

What a mod **cannot** change: the engine tables (the character's programs, the flame
and boss tables). They are mechanics, not content, and they travel with the imported package
(`engine/rom-data.json`), not in a mod. Adding worlds or levels beyond the 150
is not supported.

---

## 7. Validate, render and play

Always validate before you play. Two validators, one for the manifest/levels
(C, the product) and one for every JSON file against its schema (Python):

```sh
build/opendana --validate mods/my-mod
python3 tools/content/validate_schemas.py --mod mods/my-mod
```

`--validate` checks the **semantics** (grid size, valid cell types, exactly one
player start, at least one fire, object types) and also runs the same parsers
the game runs (`schema_version`, declared files, level and CHR patches, palettes,
screens, audio), so a mod it accepts is a mod the game loads. It exits non-zero
with a message that names the mod and the reason.
`validate_schemas.py` checks the **shape** of every file. To see a level
without running the game:

```sh
build/opendana --headless --render-level mods/my-mod/levels/1-1.json \
    --round 1 --out mi-1-1.png
```

The thumbnail is exactly the first playable frame of that level: field plus the player
at the start cell (bytes identical to the first `--play-level` frame).

To play it in context:

```sh
build/opendana --mod mods/my-mod --play-level mods/my-mod/levels/1-1.json --round 1
```

or, in the full game, play through the menu to that level.

---

## 8. A mod from scratch, step by step

```sh
# 1. the directory layout
mkdir -p mods/my-mod/levels mods/my-mod/graphics/patches

# 2. a level patch: copy mods/example/levels/1-1.json (or save a level from the
#    game's editor) and change what you like; keep "number": 1 to replace 1-1
cp mods/example/levels/1-1.json mods/my-mod/levels/1-1.json

# 3. (optional) your own player tiles: a PNG of whole 8x8 tiles, grey 0/85/170/255
#    or transparent, e.g. 14 tiles in a 112x8 image; start from the example
cp mods/example/graphics/patches/*.png mods/my-mod/graphics/patches/player.png

# 4. write mod.json
cat > mods/my-mod/mod.json <<'JSON'
{
  "schema_version": 1,
  "id": "my-mod",
  "name": "My first mod",
  "version": "1.0.0",
  "levels": ["levels/1-1.json"],
  "graphics": { "chr_patches": [ { "png": "graphics/patches/player.png", "tile": 1024 } ] }
}
JSON

# 5. validate and look at it
build/opendana --validate mods/my-mod
uv run --no-project python tools/content/validate_schemas.py --mod mods/my-mod
build/opendana --headless --mod mods/my-mod --play-level mods/my-mod/levels/1-1.json \
    --round 1 --screenshot mi-1-1.png

# 6. play
build/opendana --mod mods/my-mod
```

The rules for `mod.json` are
[`schema/mod.schema.json`](../schema/mod.schema.json): `schema_version` (1),
`id` and `version` are required; every list may be empty. The lists document
what the mod ships. The loader resolves the files that exist in the mod
directory and **rejects a declared file that is missing**, a path
with `..` or an absolute path, and a resource the package never reads. A mod
that only changes `palettes/master.json` lists it under `palettes`.

---

## 9. Compatibility

* Every package file carries `schema_version` (currently `1`). A future version
  bump migrates `mods/example` in the same release; a mod
  written for version 1 keeps working until then.
* Mods override whole files, by relative path, last layer wins.
* The example `mods/example` is generated from data written in
  `tools/content/make_example_mod.py` (`--check` verifies it) and validated by
  the test suite. It ships both of its changes (the level patch and the CHR
  patch) with content of its own, so it works in a clean checkout.
* The patch formats (`levels` patches by `number`, `graphics.chr_patches`) are
  additive and keep `schema_version` 1; mods written before them load as before.

---

## 10. Reference: file → schema

| File | Schema |
|---|---|
| `pack.json` | [`schema/pack.schema.json`](../schema/pack.schema.json) |
| `mods/<id>/mod.json` | [`schema/mod.schema.json`](../schema/mod.schema.json) |
| `levels/world-NN.json` | [`schema/level.schema.json`](../schema/level.schema.json) |
| `worlds.json` | [`schema/worlds.schema.json`](../schema/worlds.schema.json) |
| `graphics/sprites/*.json` | [`schema/poses.schema.json`](../schema/poses.schema.json) |
| `palettes/*.json` | [`schema/palette.schema.json`](../schema/palette.schema.json) |
| `audio/*.json` | [`schema/audio.schema.json`](../schema/audio.schema.json) |
| `screens/*.json` | [`schema/screen.schema.json`](../schema/screen.schema.json) |
| `graphics/chr/*.png` | no JSON schema (grey PNG, four levels) |
