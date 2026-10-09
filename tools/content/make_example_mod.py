#!/usr/bin/env python3
"""Build the documented example mods from OUR OWN content (C6.29).

`mods/example` and `content/mods/test-1-1` are deltas over the package the
player imports from their ROM. They contain no byte of the original game:

  * `levels/1-1.json` (example) and `levels/patch-1-1.json` (test-1-1) are
    level patches: world-shaped files whose single entry (`number` 1) replaces
    ROUND 1-1 only. The layouts below were designed for these mods; nothing is
    copied from the package. The other 149 levels stay the package's.
  * `graphics/patches/player.png` (example) is a `graphics.chr_patches` entry:
    14 tiles drawn here (a striped diamond), written over Hero's tiles
    0x400..0x40D. Unlike tools/content/make_mod_chr.py, nothing is derived
    from the imported package, so the PNG can be versioned and the mod works
    in a clean clone.

Run it to regenerate the files, or with --check to prove the committed files
are exactly what this script produces (tests/mods-example.sh does).

    python3 tools/content/make_example_mod.py [--root .] [--check]
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

HERO_FIRST_TILE = 0x400
HERO_TILES = 14

# Our 1-1 for mods/example: two rooms. Push the ice right into the first fire,
# drop through the hole at x=13 and push the lower ice left into the second.
EXAMPLE_GRID = [
    "2222222222222222",
    "1111111111111111",
    "1111111111111111",
    "1100000000000011",
    "1100000000000011",
    "1100000000000011",
    "1100500000e00011",
    "1111111111111011",
    "1100000000000011",
    "1100000000000011",
    "1100000000000011",
    "1100e00000005011",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
]
EXAMPLE_PLAYER = {"x": 3, "y": 6}
EXAMPLE_OBJECTS = [
    {"type": 5, "positions": [[4, 6], [12, 11]]},
    {"type": 14, "positions": [[10, 6], [4, 11]]},
]

# Our 1-1 for content/mods/test-1-1: one corridor, one ice, one fire.
TEST_GRID = [
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
    "1100000000000011",
    "1105000000000e11",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
    "1111111111111111",
]
TEST_PLAYER = {"x": 2, "y": 7}
TEST_OBJECTS = [
    {"type": 5, "positions": [[3, 7]]},
    {"type": 14, "positions": [[13, 7]]},
]

# One 8x8 tile of the patch, 2-bit values (0 = the sprite's transparent colour).
DIAMOND = [
    "00033000",
    "00311300",
    "03122130",
    "31233213",
    "31233213",
    "03122130",
    "00311300",
    "00033000",
]


def level_patch(patch_id: str, grid, player, objects) -> dict:
    fires = sum(row.count("e") for row in grid)
    return {
        "schema_version": 1,
        "id": patch_id,
        "world": 1,
        "grid_width": 16,
        "grid_height": 15,
        "levels": [{
            "index": 0,
            "number": 1,
            "world": 1,
            "round": 1,
            "name": "1-1",
            "player": player,
            "metadata": fires,   # the level byte is the fire count
            "grid": grid,
            "objects": objects,
        }],
    }


def png_grey_alpha(width: int, height: int, pixels: list[tuple[int, int]]) -> bytes:
    """8-bit grey+alpha PNG (colour type 4), filter 0, deterministic."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            g, a = pixels[y * width + x]
            raw += bytes((g, a))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 4, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def hero_patch_png() -> bytes:
    width, height = HERO_TILES * 8, 8
    pixels = []
    for y in range(height):
        for x in range(width):
            v = int(DIAMOND[y][x % 8])
            pixels.append((v * 85, 255))
    return png_grey_alpha(width, height, pixels)


def dumps(doc: dict) -> bytes:
    return (json.dumps(doc, indent=2) + "\n").encode()


def outputs(root: Path) -> dict[Path, bytes]:
    ej = root / "mods" / "example"
    t11 = root / "content" / "mods" / "test-1-1"
    return {
        ej / "mod.json": dumps({
            "schema_version": 2,
            "id": "example",
            "name": "Example mod: our own ROUND 1-1 and a striped hero",
            "version": "2.0.0",
            "levels": ["levels/1-1.json"],
            "graphics": {
                "chr_patches": [{"png": "graphics/patches/player.png", "tile": HERO_FIRST_TILE}],
            },
        }),
        ej / "levels" / "1-1.json": dumps(level_patch(
            "example-1-1", EXAMPLE_GRID, EXAMPLE_PLAYER, EXAMPLE_OBJECTS)),
        ej / "graphics" / "patches" / "player.png": hero_patch_png(),
        t11 / "mod.json": dumps({
            "schema_version": 2,
            "id": "test-1-1",
            "name": "C2.1 test mod: our own ROUND 1-1 (level patch)",
            "version": "2.0.0",
            "levels": ["levels/patch-1-1.json"],
        }),
        t11 / "levels" / "patch-1-1.json": dumps(level_patch(
            "test-1-1", TEST_GRID, TEST_PLAYER, TEST_OBJECTS)),
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument("--check", action="store_true",
                    help="fail unless the committed files equal the generated ones")
    args = ap.parse_args(argv)
    root = Path(args.root)
    bad = 0
    for path, data in outputs(root).items():
        if args.check:
            if not path.is_file() or path.read_bytes() != data:
                print(f"DIFFERS: {path.relative_to(root)}")
                bad += 1
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            print(f"wrote {path.relative_to(root)} ({len(data)} B)")
    if args.check:
        print("example mods: " + ("OK, identical to the generator" if not bad
                                  else f"{bad} file(s) differ"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
