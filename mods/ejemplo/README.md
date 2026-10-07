# `mods/ejemplo` — the documented example mod

A minimal, complete mod. It overlays the package you imported from your ROM
(`opendana --import-rom`) and changes exactly two things, **with content of its own**
(C6.29: it ships no byte of the original game):

| File | Change |
|---|---|
| `levels/1-1.json` | a **level patch**: its single entry (`"number": 1`) replaces ROUND 1-1 with a layout designed for this mod (two rooms, two ice blocks, two fires, an ice top border) |
| `graphics/patches/player.png` | a **CHR patch**: 14 tiles drawn for this mod (a striped diamond) written over the player's tiles `0x400`…`0x40D` |

Everything else (the other 149 levels, the terrain, the palettes, the audio)
comes from the package.

`mod.json`:

```json
{
  "schema_version": 2,
  "id": "ejemplo",
  "name": "Example mod: our own ROUND 1-1 and a striped hero",
  "version": "2.0.0",
  "levels": ["levels/1-1.json"],
  "graphics": {
    "chr_patches": [{ "png": "graphics/patches/player.png", "tile": 1024 }]
  }
}
```

## The two mod formats it shows

* **Level patch.** A file in `levels` that is *not* one of the package world
  files (`levels/world-NN.json`) has the world-file shape and each of its
  entries carries `"number"` (1…150): that entry replaces the package level
  with the same number and nothing else. A number out of range, an
  `index`/`world`/`round` that disagrees with it, or the same number patched
  twice (also by two mods) is an error. A file at a package world path still
  replaces that whole world, so it must hold its 10 levels.
* **CHR patch.** `graphics.chr_patches` lists PNGs of whole 8×8 tiles, laid
  out row by row and written over the package tile image from `tile` on (0…8191).
  Every pixel is grey 0, 85, 170 or 255 (the 2-bit colour) or fully
  transparent (keep the package pixel). Anything else is an error that names
  the mod.

Every file `mod.json` declares must exist inside the mod; `..` and absolute
paths are refused.

## Run it

```sh
# windowed
build/opendana --mod mods/ejemplo

# headless snapshot of ROUND 1-1 reached from reset (route R1)
build/opendana --headless --no-audio --mod mods/ejemplo --replay tests/data/r1-input.txt \
    --frames 2401 --shot-frame 2400 --screenshot 1-1.png

# validate it (the same parsers the game runs, over your imported package)
build/opendana --validate mods/ejemplo
python3 tools/content/validate_schemas.py --mod mods/ejemplo
```

## Regenerate

All files are versioned and produced by `tools/content/make_example_mod.py`
(the layouts and the tile are written in that script). `--check` proves the
committed files are exactly what it produces:

```sh
python3 tools/content/make_example_mod.py          # rewrite the files
python3 tools/content/make_example_mod.py --check  # tests/mods-example.sh runs this
```
