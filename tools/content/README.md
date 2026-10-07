# tools/content - content package tooling

Offline tools that turn the extracted binary assets into the moddable content
package and back. They never touch the ROM and never run at game time.

## Export (assets -> content/base)

```sh
python3 tools/content/export_pack.py
```

Reads `assets/levels.bin` and `assets/enemies.bin` and writes:

```
content/base/pack.json
content/base/levels/world-01.json ... world-15.json
```

Each `world-NN.json` holds ten levels: hex grid, Hero, metadata and the object
record in its original order.

## Verify (content/base -> assets, MJ2)

```sh
python3 tools/content/verify_pack.py
```

Rebuilds `levels.bin` and `enemies.bin` from the JSON and checks them byte for
byte against `assets/`, plus every per-level grid digest in
`assets/levels.ref.txt`. This is the independent counterpart of the C loader in
`src/core/content.c`.

Expected output:

```
pack content/base: 150 levels
  levels.bin: byte-identical (40851 bytes, fnv1a64 ...)
  enemies.bin: byte-identical (4701 bytes, fnv1a64 ...)
  levels.ref.txt: 150 digests, 0 mismatches
verify: OK
```

## Graphics and audio (assets -> content/base, C2.2)

```sh
python3 tools/content/export_gfx.py
```

Reads the extracted graphics and audio (`assets/tiles.chr`,
`assets/anim/eu_poses.h`, `assets/anim/eu_effects.h`,
`assets/world/world_gfx*.json`, the master-palette headers,
`assets/audio/sequence.hex`, `assets/screens/boss-arena.inc` and
`assets/screens/retry.h`) and writes:

```
content/base/graphics/chr/chr-NN.png     indexed 4-colour CHR sheets
content/base/graphics/sprites/poses.json metasprites and animation tables
content/base/worlds.json                 per-world cell graphics
content/base/palettes/master.json        the three master palettes
content/base/audio/sequence.json         the sparse sequencer stream
content/base/screens/boss-arena.json     ROUND 10-10 arena grid
content/base/screens/retry.json          TRY AGAIN / NEW GAME artwork
```

and extends `pack.json` with the `graphics`, `worlds`, `palettes`, `audio` and
`screens` sections.

## Verify graphics and audio (content/base -> assets, MJ2)

```sh
python3 tools/content/verify_gfx.py
```

Rebuilds the CHR from the PNG sheets and compares every section byte for byte
against the authoritative sources. The C counterpart is
`tests/test_compose_content.c` (also in `tests/run.sh`).

## Engine tables (C5.6)

```sh
python3 tools/content/export_engine_tables.py --verify
```

The three engine data sets that used to be compiled into the binary
(`src/core/tables/*.inc`) now ship as `content/base/engine/*.json`:

```
content/base/engine/enemy-tables.json   flame animation and pose tables
content/base/engine/hero-programs.json  Hero's bank 4/5 action programs
content/base/engine/boss-tables.json    Boss combat scripts and metasprites
```

`src/core/content_tables.c` parses them at startup into a `od_engine_tables`
that `enemy.c`, `game.c` and `objects.c` read; a mod can override any of the
three by relative path. `export_engine_tables.py` regenerates the JSON from the
compiled `.inc` form (the one-shot C5.6 conversion) and `--verify` checks the
canonical byte image of the shipped JSON against the SHA-256 measured before
the move. The C counterpart is `tests/test_engine_tables.c`, which also proves
`build/opendana` no longer contains a single copy of the three tables.

## Derive a mod CHR from the imported package (C6.18)

```sh
python3 tools/content/make_mod_chr.py --base content/base \
    --out mods/ejemplo --style hatch     # or --style solid
python3 tools/content/make_mod_chr.py --self-test
```

Rebuilds the sheet holding Hero (`graphics/chr/chr-01.png`, tiles `$400..$40D`)
from the user's imported package and applies only the Hero transform. The
repository ships **no** mod CHR: a full derived sheet would version the original
CHR and `tools/audit/rom_free.py` flags it (`docs/PLAN-V2.md` §7.5). The
generated path is ignored; `tests/mods-chr-contract.sh` checks the contract.

## Test mod (MJ6)

```sh
python3 tools/content/make_test_mod.py
```

Regenerates `content/mods/test-gfx/`: an overlay that changes the EU PAL master
palette (committed) and Hero's CHR tiles (derived with `make_mod_chr.py`).
`tests/test_compose_content.c` loads it through the layered VFS and writes
`mod-base.png` / `mod-changed.png` (under the test output directory).

## Example mod (C4.1, MJ6)

```sh
python3 tools/content/make_example_mod.py
```

Regenerates `mods/ejemplo/`: the documented example from `docs/MODDING.md`. It
changes ROUND 1-1's top border and repaints Hero's CHR tiles (derived with
`make_mod_chr.py`). `tests/test_mod.c` loads it through the VFS, renders both
frames to the test output directory and `tests/run.sh` checks that the modded frame differs.

## Validate every file against its schema (C4.1, MJ6)

```sh
python3 tools/content/validate_schemas.py --self-test
python3 tools/content/validate_schemas.py --mod mods/ejemplo
```

Validates `content/base` and every mod (`content/mods/*` and `mods/*`) against
the JSON schemas in `schema/`. `--self-test` is the positive control: a broken
level document must be rejected.

## Layout of the package

See `schema/` for every file's schema and `docs/MODDING.md` for the authoring
guide. A mod is a directory with the same relative paths plus a `mod.json`; the
game overlays it with `--mod DIR` (last layer wins per file).
