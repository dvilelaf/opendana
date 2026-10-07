#!/usr/bin/env python3
"""Validate the content package and its mods against the JSON schemas (MJ6).

Every JSON file of the package has a schema in `schema/`:

    pack.json                  pack.schema.json
    mods/<id>/mod.json         mod.schema.json
    levels/world-NN.json       level.schema.json
    worlds.json                worlds.schema.json
    graphics/sprites/*.json    poses.schema.json
    palettes/*.json            palette.schema.json
    audio/*.json               audio.schema.json
    screens/*.json             screen.schema.json
    screens/<name>.json        <name>.schema.json when schema/ ships one
                               (interactive-setup, transfer-structure)
    screens/cinematics/<name>.json
                               <name>.schema.json when schema/ ships one
                               (post-ending), else cinematics.schema.json

A document whose basename has its own schema in `schema/` is validated by it,
so a dedicated schema cannot be silently bypassed. Cross-schema `$ref`s are
resolved by `$id` or by basename (post-ending -> cinematics.schema.json).

The CHR sheets are PNG, not JSON, so they have no JSON schema; their content is
checked byte for byte by `verify_gfx.py` and the C side of MJ2.

Besides the files pack.json lists, every JSON file on disk in the package is
validated by its directory (the loader also reads some by path, e.g.
screens/boss-background.json); a package JSON file no schema covers is an
error, so a new file cannot ship unvalidated (C4.1).

    python3 tools/content/validate_schemas.py
    python3 tools/content/validate_schemas.py --base content/base --mod mods/ejemplo

The second half of MJ6 (`--validate` rejects broken files) is the C-side
validator; this tool proves the schemas accept everything the game ships, and
its own `--self-test` proves they reject a broken document.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError:  # pragma: no cover
    print("ERROR: the 'jsonschema' module is required (pip install jsonschema)", file=sys.stderr)
    raise SystemExit(2)

# File kind -> (schema basename, pack section, list key).
KINDS = {
    "level": ("level.schema.json", "levels", None),
    "worlds": ("worlds.schema.json", "worlds", None),
    "poses": ("poses.schema.json", None, None),
    "palette": ("palette.schema.json", "palettes", None),
    "audio": ("audio.schema.json", "audio", None),
    "screen": ("screen.schema.json", "screens", None),
    "engine": ("engine.schema.json", "engine", None),
    "cinematic": ("cinematics.schema.json", "screens", None),
}


class Validator:
    def __init__(self, schemas_dir: Path) -> None:
        self.schemas_dir = schemas_dir
        self._cache: dict[str, dict] = {}
        self._store: dict[str, dict] | None = None
        self.checked = 0
        self.errors = 0
        self.seen: set[Path] = set()

    def schema(self, name: str) -> dict:
        if name not in self._cache:
            path = self.schemas_dir / name
            self._cache[name] = json.loads(path.read_text(encoding="utf-8"))
        return self._cache[name]

    def schema_store(self) -> dict[str, dict]:
        """Every schema keyed by basename and by `$id`, so a schema can `$ref`
        another one relatively (post-ending -> cinematics.schema.json)."""
        if self._store is None:
            store: dict[str, dict] = {}
            for path in sorted(self.schemas_dir.glob("*.json")):
                try:
                    schema = json.loads(path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    continue
                self._cache.setdefault(path.name, schema)
                store[path.name] = schema
                schema_id = schema.get("$id") if isinstance(schema, dict) else None
                if isinstance(schema_id, str):
                    store[schema_id] = schema
            self._store = store
        return self._store

    def validation_kwargs(self, schema: dict) -> dict:
        """Keyword arguments that let jsonschema resolve the schema's `$ref`s.

        `RefResolver` covers the installed jsonschema 4.x; the `referencing`
        registry is the equivalent on versions that dropped the resolver."""
        store = self.schema_store()
        resolver = getattr(jsonschema, "RefResolver", None)
        if resolver is not None:
            try:
                return {"resolver": resolver.from_schema(schema, store=store)}
            except Exception:  # pragma: no cover - fall back to the registry
                pass
        try:  # pragma: no cover - only on jsonschema versions without RefResolver
            from referencing import Registry, Resource
            from referencing.jsonschema import DRAFT202012

            resources = [
                (uri, Resource.from_contents(sub, default_specification=DRAFT202012))
                for uri, sub in store.items()
            ]
            return {"registry": Registry().with_resources(resources)}
        except Exception:
            return {}

    def dedicated(self, path: Path) -> str | None:
        """The schema a package document ships for itself, if any.

        A document in `screens/` (or `screens/cinematics/`) whose basename has
        its own `schema/<basename>.schema.json` is validated by it, so the
        dedicated schema cannot be bypassed: `interactive-setup`,
        `transfer-structure` and `post-ending`. Everything else keeps the
        directory's schema (`screen`/`cinematics`)."""
        parent = path.parent.name
        if parent == "screens" or (
            parent == "cinematics" and path.parent.parent.name == "screens"
        ):
            candidate = f"{path.stem}.schema.json"
            if (self.schemas_dir / candidate).is_file():
                return candidate
        return None

    def doc(self, path: Path, schema_name: str, where: str, quiet: bool = False) -> bool:
        # These versioned documents are not the old screen/engine formats.
        # Route listed files and mod overrides identically, without weakening
        # the fallback schema or silently skipping unknown package documents.
        dedicated = self.dedicated(path)
        if dedicated is not None:
            schema_name = dedicated
        elif path.parent.name == "cinematics" and path.parent.parent.name == "screens":
            schema_name = "cinematics.schema.json"
        elif path.name == "rom-data.json" and path.parent.name == "engine":
            schema_name = "rom-data.schema.json"
        elif path.name == "editor-data.json" and path.parent.name == "engine":
            schema_name = "editor-data.schema.json"
        elif path.name == "scene-data.json" and path.parent.name == "engine":
            schema_name = "scene-data.schema.json"
        elif path.name == "presentation-data.json" and path.parent.name == "engine":
            schema_name = "presentation-data.schema.json"
        if not quiet:
            self.seen.add(path.resolve())

        def fail(msg: str) -> bool:
            if not quiet:
                print(f"  FAIL {where}: {msg}")
            self.errors += 1
            return False

        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return fail("missing file")
        except json.JSONDecodeError as exc:
            return fail(f"not valid JSON ({exc})")
        try:
            schema = self.schema(schema_name)
            jsonschema.validate(instance=data, schema=schema,
                                **self.validation_kwargs(schema))
        except jsonschema.ValidationError as exc:
            loc = "/".join(str(p) for p in exc.absolute_path)
            return fail(exc.message + (f" at {loc}" if loc else ""))
        except jsonschema.exceptions.RefResolutionError as exc:  # pragma: no cover
            return fail(f"schema {schema_name} has an unresolvable $ref ({exc})")
        self.checked += 1
        return True

    def pack(self, root: Path) -> None:
        pack_path = root / "pack.json"
        if not self.doc(pack_path, "pack.schema.json", f"{root}/pack.json"):
            return
        pack = json.loads(pack_path.read_text(encoding="utf-8"))
        for section, kind in (
            ("levels", "level"),
            ("worlds", "worlds"),
            ("palettes", "palette"),
            ("audio", "audio"),
            ("screens", "screen"),
            ("engine", "engine"),
        ):
            schema_name, _, _ = KINDS[kind]
            for rel in pack.get(section, []):
                self.doc(root / rel, schema_name, f"{root}/{rel}")
        for rel in pack.get("graphics", {}).get("sprites", []):
            self.doc(root / rel, KINDS["poses"][0], f"{root}/{rel}")
        self.on_disk(root)

    def on_disk(self, root: Path) -> None:
        """Every JSON file the package ships has a schema, listed or not: the
        loader also reads files by path (screens/boss-background.json)."""
        by_dir = {
            "levels": "level", "screens": "screen", "palettes": "palette",
            "audio": "audio", "graphics/sprites": "poses", "engine": "engine",
            "screens/cinematics": "cinematic",
        }
        for path in sorted(root.rglob("*.json")):
            if path.resolve() in self.seen:
                continue
            rel = path.relative_to(root).as_posix()
            parent = path.parent.relative_to(root).as_posix()
            kind = "worlds" if rel == "worlds.json" else by_dir.get(parent)
            if kind is None:
                print(f"  FAIL {path}: no schema covers this package file")
                self.errors += 1
                continue
            self.doc(path, KINDS[kind][0], str(path))

    def mod(self, root: Path) -> None:
        mod_path = root / "mod.json"
        if not self.doc(mod_path, "mod.schema.json", f"{root}/mod.json"):
            return
        mod = json.loads(mod_path.read_text(encoding="utf-8"))
        for rel in mod.get("levels", []):
            self.doc(root / rel, KINDS["level"][0], f"{root}/{rel}")
        # A mod may override any file the base declares; validate whatever JSON
        # it actually ships, whether or not mod.json lists it.
        for rel in mod.get("palettes", []):
            self.doc(root / rel, KINDS["palette"][0], f"{root}/{rel}")
        for rel in mod.get("audio", []):
            self.doc(root / rel, KINDS["audio"][0], f"{root}/{rel}")
        for rel in mod.get("worlds", []):
            self.doc(root / rel, KINDS["worlds"][0], f"{root}/{rel}")
        for rel in mod.get("graphics", {}).get("sprites", []):
            self.doc(root / rel, KINDS["poses"][0], f"{root}/{rel}")
        for rel in mod.get("screens", []):
            self.doc(root / rel, KINDS["screen"][0], f"{root}/{rel}")
        # Files present on disk but not listed (the loader resolves by path).
        for rel, kind in (
            ("worlds.json", "worlds"),
            ("palettes/master.json", "palette"),
            ("audio/sequence.json", "audio"),
            ("graphics/sprites/poses.json", "poses"),
        ):
            if (root / rel).is_file():
                self.doc(root / rel, KINDS[kind][0], f"{root}/{rel}")
        for screen in sorted((root / "screens").glob("*.json")) if (root / "screens").is_dir() else []:
            self.doc(screen, KINDS["screen"][0], str(screen))
        for engine in sorted((root / "engine").glob("*.json")) if (root / "engine").is_dir() else []:
            self.doc(engine, KINDS["engine"][0], str(engine))
        for level in sorted((root / "levels").glob("world-*.json")):
            self.doc(level, KINDS["level"][0], str(level))
        # Covers nested cinematic overrides as well as rejecting unknown JSON.
        self.on_disk(root)


def _records() -> list:
    return [1, 8192, [0]]


def _minimal_v2_layouts() -> dict:
    base = {"chr_mode": 0, "background_banks": [0, 0, 0, 0], "ciram": [0] * 2048}
    return {"schema_version": 2, "id": "layouts", "map_bases": [
        {"scene_index": 2, **base}, {"scene_index": 43, **base}]}


def _minimal_v2_magicword() -> dict:
    return {
        "schema_version": 2, "id": "magicword",
        "palette_buffer": [0] * 32, "chr": [0] * 6, "song_request": 0,
        "screen": [{"op": "records", "records": [_records()]}, {"op": "yield"},
                   {"op": "records", "records": [_records()]}, {"op": "yield"},
                   {"op": "word"}],
        "question": [{"op": "records", "records": [_records()]}],
        "word": {"address": 8192, "count": 26, "group": 8, "gap_tile": 0,
                 "glyphs": [0] * 32},
        "cursor": {"y": 0, "tile": 0, "attr": 0, "x": [0, 0]},
        "cycle": {"bg_mask": 15, "bg_entries": [5, 6, 7], "bg_tables": [[0, 0, 0]] * 3,
                  "sprite_mask": 7, "sprite_entry": 19, "sprite_table": [0] * 5},
        "queue_capacity_bytes": 60,
        "post_goodnight": {"palette_buffer": [0] * 32, "brightness_bg": 15,
                           "brightness_sprite": 15,
                           "cycle": {"mask": 7, "entry": 3, "colours": [0, 0, 0, 0]}},
    }


def _minimal_interactive_setup() -> dict:
    record = _records()
    return {
        "schema_version": 1, "id": "interactive-setup",
        "password": {"chr": [0] * 6, "palette_buffer": [0] * 32,
                     "field_frame": [record] * 4, "choice_panel": [record] * 8,
                     "field_cursor_x": [0] * 24, "palette_cursor_oam_yx": [[0, 0]] * 35},
        "editor": {"chr": [0] * 6, "palette_buffer": [0] * 32,
                   "initial_layout": [record] * 27, "toolbar_layout": [record] * 8,
                   "entry_cinematic": 29},
    }


def _minimal_transfer_structure() -> dict:
    def atlas(tiles: int) -> dict:
        return {"tile_storage_low": 0, "attribute_storage_low": 0,
                "tiles": [[0] * 16] * tiles, "attributes": [0] * tiles}
    return {
        "schema_version": 1, "id": "transfer-structure",
        "destinations": [0] * 12, "atlases": [atlas(239), atlas(132)],
        "images": [{"atlas_id": 0, "columns": 1, "rows": 1, "header_storage_low": 0,
                    "metatiles": [0]}] * 31,
    }


def _minimal_post_ending() -> dict:
    step = {"op": "step", "args": [], "bank": 9, "addr": 32768}
    text = {"op": "line", "args": []}
    return {
        "schema_version": 1, "id": "post-ending",
        "source": {"rom_sha256": "0" * 64, "engine": "e", "scripts": "s"},
        "scenes": [{"index": 7, "program": [step] * 387, "text": [text] * 785}],
        "poses": [[[0]]] * 208, "animations": [[]] * 46, "bands": [[0]] * 14,
        "cycles": [{"values": [0] * 64, "indices": [0] * 8}] * 20,
        "velocities": [[0, 0, 0, 0]] * 27,
        "bg_bank_sets": [[0, 0, 0, 0]] * 256,
        "band_addresses": [0] * 12, "colours": [[0, 0, 0]] * 256,
        "text_addresses": [8192] * 4,
        "image_cycles": [[], [], [], [0, 0, 0, 0], [0, 0, 0, 0], [], [], []],
    }


def _minimal_v1_layouts() -> dict:
    return {
        "schema_version": 1, "id": "layouts", "source": "x",
        "title": {"words": [], "palettes": [{"tick": 0, "palette": [0] * 32}],
                  "sprite_banks_before": [0, 0], "sprite_banks_from": 0,
                  "end": {"tick": 0, "words": []},
                  "colour": {"entry": 0, "table": [0, 0, 0]}},
        "menu": {"words": [], "palette": [0] * 32},
        "menu_close": {"words": [], "palette": [0] * 32, "frames": 1},
        "map": {"words": [], "palette": [0] * 32},
    }


def _minimal_v1_magicword() -> dict:
    return {
        "schema_version": 1, "id": "magicword", "source": "x",
        "palette": [0] * 32, "bg_banks": [0] * 4, "sprite_banks": [0] * 2, "mask": 0,
        "queue_capacity": 1, "program_start": 0, "palette_age": 0,
        "song": {"id": 0, "age": 0}, "screen": [{"op": "yield"}],
        "question": [{"op": "yield"}],
        "word": {"address": 0, "count": 0, "group": 1, "gap_tile": 0, "glyphs": [0] * 32},
        "cursor": {"y": 0, "tile": 0, "attr": 0, "x": [0, 0]},
        "cycle": {"bg_mask": 0, "bg_entries": [0, 0, 0], "bg_tables": [[0, 0, 0]] * 3,
                  "sprite_mask": 0, "sprite_entry": 0, "sprite_table": [0] * 5},
        "night": {"duration": 1, "cues": [], "palette": [0] * 32, "palette_tick": 0,
                  "cycle": {"mask": 0, "entry": 0, "table": [0] * 4}},
    }


def self_test(schemas_dir: Path, tmp: Path) -> bool:
    """Positive control: the schemas accept the shapes the package ships and
    reject broken ones.

    Runs every check on a throwaway validator so the intentional failures do
    not pollute the package's error count. Covers the level schema, the v2
    `screen` branches, the dedicated screen schemas (routing) and post-ending's
    relative `$ref` into cinematics.schema.json."""
    checks: list[bool] = []

    def run(name: str, doc: dict, schema_name: str) -> bool:
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(doc), encoding="utf-8")
        return Validator(schemas_dir).doc(path, schema_name, str(path), quiet=True)

    def accepts(name: str, doc: dict, schema_name: str) -> None:
        checks.append(run(name, doc, schema_name))

    def rejects(name: str, doc: dict, schema_name: str) -> None:
        checks.append(not run(name, doc, schema_name))

    # level: a 1-row grid where the schema requires 15.
    rejects("broken-level.json", {
        "schema_version": 1, "id": "broken", "world": 1,
        "grid_width": 16, "grid_height": 15,
        "levels": [{
            "index": 0, "number": 1, "world": 1, "round": 1, "name": "1-1",
            "player": {"x": 1, "y": 1}, "metadata": 0,
            "grid": ["1111111111111111"],  # 1 row, not 15
            "objects": [{"type": 14, "positions": [[2, 2]]}],
        }],
    }, "level.schema.json")

    # screen v2 branches accept what extract_cinematics.c emits and reject a
    # wrong constant; the v1 branches still accept v1 documents.
    accepts("screens/layouts.json", _minimal_v2_layouts(), "screen.schema.json")
    broken = _minimal_v2_layouts()
    broken["map_bases"][0]["scene_index"] = 3
    rejects("screens/layouts.json", broken, "screen.schema.json")
    accepts("screens/layouts.json", _minimal_v1_layouts(), "screen.schema.json")
    accepts("screens/magicword.json", _minimal_v2_magicword(), "screen.schema.json")
    broken = _minimal_v2_magicword()
    broken["queue_capacity_bytes"] = 61
    rejects("screens/magicword.json", broken, "screen.schema.json")
    accepts("screens/magicword.json", _minimal_v1_magicword(), "screen.schema.json")

    # A document with its own schema must be routed to it, not to the fallback.
    accepts("screens/interactive-setup.json", _minimal_interactive_setup(),
            "screen.schema.json")
    broken = _minimal_interactive_setup()
    broken["password"]["palette_buffer"] = [0] * 31
    rejects("screens/interactive-setup.json", broken, "screen.schema.json")
    accepts("screens/transfer-structure.json", _minimal_transfer_structure(),
            "screen.schema.json")
    broken = _minimal_transfer_structure()
    broken["destinations"] = [0] * 11
    rejects("screens/transfer-structure.json", broken, "screen.schema.json")

    # post-ending is routed to its schema and its cinematics.schema.json
    # `$ref`s resolve.
    accepts("screens/cinematics/post-ending.json", _minimal_post_ending(),
            "cinematics.schema.json")
    broken = _minimal_post_ending()
    broken["source"]["rom_sha256"] = "bad"
    rejects("screens/cinematics/post-ending.json", broken, "cinematics.schema.json")

    return all(checks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schemas", default="schema", type=Path)
    parser.add_argument("--base", default="content/base", type=Path)
    parser.add_argument("--mod", action="append", default=None, type=Path,
                        help="mod directory to validate (repeatable); default mods/ejemplo")
    parser.add_argument("--self-test", action="store_true",
                        help="also check that the shipped shapes are accepted and "
                             "broken documents rejected")
    args = parser.parse_args()

    v = Validator(args.schemas)
    print(f"schemas in {args.schemas}")
    v.pack(args.base)
    print(f"  pack {args.base}: validated")

    mods = args.mod
    if mods is None:
        mods = [Path("mods/ejemplo")]
        extra = sorted(Path("content/mods").glob("*/mod.json"))
        mods += [p.parent for p in extra]
        top = sorted(Path("mods").glob("*/mod.json"))
        mods += [p.parent for p in top if p.parent.name != "ejemplo"]
    for mod in mods:
        if (mod / "mod.json").is_file():
            v.mod(mod)
            print(f"  mod {mod}: validated")

    if args.self_test:
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            control = self_test(args.schemas, Path(d))
        if control:
            print("  self-test: package shapes accepted and broken documents rejected")
        else:
            print("  self-test: FAILED (a shape was rejected or a broken document accepted)")
            v.errors += 1

    print(f"validate-schemas: {v.checked} documents, {v.errors} error(s)")
    if v.errors:
        print("validate-schemas: FAILED")
        return 1
    print("validate-schemas: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
