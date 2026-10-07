#!/usr/bin/env python3
"""Build the C2.2 graphics test mod `content/mods/test-gfx`.

The mod is a minimal overlay that proves a mod can replace content at runtime
(MJ6): it changes

  * the EU PAL master palette (a blue tint and a magenta entry), and
  * the CHR tiles of Hero (0x400..0x40D) in the second sheet,

without owning the rest of the package. `tests/test_compose_content.c` loads it
with the layered VFS and renders a side-by-side PNG.

C6.18: the CHR is **derived from the imported package** and never versioned
(see the content tooling notes); the palette tint is committed. Running this
script needs `content/base` (the package `opendana --import-rom` writes).

    python3 tools/content/make_test_mod.py [--root .] [--base content/base]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_mod_chr  # noqa: E402

CHR_REL = "graphics/chr/chr-01.png"


def tint(c: int) -> int:
    """Swap red and blue and raise the blue channel: clearly not the vanilla."""
    r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
    return (b << 16) | (g << 8) | min(255, r + 64)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".", type=Path)
    ap.add_argument("--base", default=None, type=Path,
                    help="imported package (default: <root>/content/base)")
    args = ap.parse_args()
    root: Path = args.root
    base = args.base or (root / "content" / "base")
    out = root / "content" / "mods" / "test-gfx"
    out.mkdir(parents=True, exist_ok=True)
    (out / "palettes").mkdir(parents=True, exist_ok=True)
    if not (base / "pack.json").is_file():
        print(f"BLOCKED: no package at '{base}' "
              "(run `opendana --import-rom` first)", file=sys.stderr)
        return 2

    # 1) Palette: tint the EU PAL master palette (committed).
    pal = json.loads((base / "palettes/master.json").read_text())
    for entry in pal["master"]:
        if entry["id"] == "pal":
            entry["colors"] = [tint(c) for c in entry["colors"]]
    (out / "palettes/master.json").write_text(json.dumps(pal, indent=1) + "\n")

    # 2) CHR: rebuild the sheet holding Hero with a solid highlight.
    make_mod_chr.build_chr_sheet(base, out, "solid")
    name = CHR_REL

    mod = {
        "schema_version": 2,
        "id": "test-gfx",
        "name": "C2.2 graphics test mod",
        "version": "1.0.0",
        "levels": [],
    }
    (out / "mod.json").write_text(json.dumps(mod, indent=2) + "\n")
    print(f"wrote {out} (palette + {name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
