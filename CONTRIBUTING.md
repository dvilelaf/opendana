# Contributing to OpenDana

Thank you for helping. These rules keep the project legal, deterministic and
easy to mod. Read them before opening a pull request.

## The one rule that cannot bend

**No byte, image, text, level layout, sound or table of the original game ever
enters this repository.** That covers source, tests, documentation, mods,
screenshots and commit history.

- The ROM is a runtime input of the importer. It is never committed, never
  embedded, never copied into a fixture or a mod.
- Do not paste addresses or code of the original into the repository. The
  importer reads offsets as facts it needs, and that is all.
- Do not name the original game, its characters or its places outside the
  README, `NOTICE`, the first-run screen and the importer's error messages.
- Do not add screenshots of the original game. Screenshots of OpenDana's own
  interface, or of art you drew yourself, are welcome.
- Do not add a CPU emulator, interpreter or JIT of any kind.

If you are unsure whether something is allowed, leave it out and ask in the pull
request.

## Technical rules

- **C11**, compiled with `-std=c11`; no compiler extensions in the core.
- **Deterministic core without SDL.** `src/core/` and `src/ext/` must not include
  SDL, read the clock, use global random state or depend on the host. The same
  inputs must give the same state, tick by tick. SDL lives only in `src/app/`.
- **Simulation and presentation share one clock.** Audio and video come from the
  same simulation; a sleep is never a game clock.
- **Content is data.** New content goes into the content package (JSON and PNG
  with a schema in `schema/`), never compiled into the binary. Engine tables are
  the only exception. Changing the package format means raising
  `schema_version` and migrating `mods/example` in the same commit.
- **Extensions do not change the simulation.** The editor, save states, the
  settings and the interface sit beside the core and never alter its results.
- No tool-produced code in the product. Every routine is written and readable by
  hand.

## Style

- Small functions, one job each; `static` by default; no global mutable state
  outside clearly named singletons.
- Four-space indent, no tabs, lines under about 100 columns.
- Public names start with `od_` (macros with `OD_`); headers are in
  `include/opendana/`.
- Check every return value; free what you allocate; never trust a file you read.
- Comments say why, not what. They do not cite external sources of the
  original game.
- Every file starts with an `SPDX-License-Identifier: GPL-3.0-or-later` line.

## Release checks

Maintainers run additional provenance and content checks before each release.

## Tests that work in a public clone

No ROM and no imported content are needed for these. They are the ones CI runs.

```sh
bash build.sh
build/opendana_tests_public --category unit
bash tests/editor-ui-layout-run.sh
bash tests/ui-screens-layout-run.sh
bash tests/ui-behaviour-run.sh
bash tests/state-compat-run.sh
bash tests/cursor-hide-run.sh
bash tests/desktop-run.sh
bash tests/import-picker-child.sh
bash tests/linux-package-unit.sh
bash tests/mods-chr-contract.sh
python3 tools/content/make_example_mod.py --check
```

Tests that need your own ROM or the imported content read them only from
environment variables (`OD_ROM`, `OD_CONTENT`) and report `BLOCKED` when they are
missing; they never pass silently without their data.

Every new feature gets a test and, when it can be compared automatically, a
positive control: a case that is known to be wrong and must be detected.

## The content package and mods

- The package is a set of JSON and PNG files described by the schemas in
  `schema/`; `pack.json` lists them. It is created on the user's machine by the
  importer and is not versioned.
- A mod is a directory with a `mod.json` (schema `schema/mod.schema.json`) that
  overlays files by relative path; the last layer wins. Level patches replace
  single levels and CHR patches replace single tiles, so a mod never needs to
  ship the rest.
- Validate with `opendana --validate DIR` and
  `python3 tools/content/validate_schemas.py --mod DIR`.
- The example mod `mods/example` is produced by
  `tools/content/make_example_mod.py` (`--check` proves it is identical to the
  generator). It carries only art and levels made for this project.

The full guide is [`docs/MODDING.md`](docs/MODDING.md).

## Pull requests

- One topic per pull request, with a short description of what changed and how
  you tested it.
- Keep commits focused; use a clear imperative subject line.
- By contributing you agree that your work is released under
  `GPL-3.0-or-later`, like the rest of the code.
