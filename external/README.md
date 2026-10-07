# external/ - vendored dependencies

Third-party code lives here, vendored (not fetched at build time), with its
license and version pinned so the build is reproducible and offline.

## cJSON

| | |
|---|---|
| Files | `cJSON/cJSON.c`, `cJSON/cJSON.h` |
| Version | **v1.7.18** |
| Origin | <https://github.com/DaveGamble/cJSON>, tag `v1.7.18` |
| License | MIT (see `cJSON/LICENSE`) |
| SHA-256 | `cJSON.c` `75c51de8fa40ac9d7a99319c6330719bd692eb81c0a869265f3d4c682533f9b9` |
| | `cJSON.h` `0578cc29132912edbc88f83207a8fc76e5db3db0605497e909a9384ef3cc474b` |
| Used by | `src/core/content.c` (content package loader) |

Only the JSON parser is used; no network, no filesystem access beyond what
`content.c` passes in. `CMakeLists.txt` adds `cJSON.c` to `opendana_core` and the
`cJSON/` directory to its private include path, so no public header exposes it.

To update: download the two files from the pinned tag, replace them, update the
version and hashes above, and re-run `bash tests/run.sh`.

## stb_image

| | |
|---|---|
| File | `stb/stb_image.h` |
| Version | **v2.30** |
| Origin | <https://github.com/nothings/stb>, `stb_image.h` version 2.30 (upstream has no release tags; the file is identified by its version line and the SHA-256 below) |
| License | Public domain (dual MIT, see the "ALTERNATIVE A - MIT License" block in the header) |
| SHA-256 | `stb_image.h` `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` |
| Used by | `src/core/tile.c` (`od_tile_chr_from_png`, CHR PNG sheets) |

Only the PNG decoder is compiled (`src/core/stb_image_impl.c` defines
`STBI_ONLY_PNG` and `STBI_NO_STDIO`); the loader reads the in-memory buffer the
content VFS hands it. `CMakeLists.txt` adds the `stb/` directory to the private
include path of `opendana_core`.

## stb_image_write

| | |
|---|---|
| File | `stb/stb_image_write.h` |
| Version | **v1.16** |
| Origin | <https://github.com/nothings/stb> (tag `v1.16`) |
| License | Public domain (dual MIT) |
| SHA-256 | `stb_image_write.h` `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05` |
| Used by | `src/import/extract_graphics.c`: the importer writes the eight 256x256 CHR sheets |

`src/import/stb_image_write_impl.c` compiles the implementation once. The
sheets are written as grayscale PNGs whose four levels (0/85/170/255) decode
back to the exact 2-bit CHR tiles through `od_tile_chr_from_png`, so the
package round trip is lossless even though the committed sheets use an indexed
4-colour PNG. No PNG is read from the ROM; `stb_image_write` only writes.

To update: download the single header from the pinned version, replace it,
update the version and hash above, and re-run `bash tests/run.sh`.

## stb_truetype

| | |
|---|---|
| File | `stb/stb_truetype.h` |
| Version | **v1.26** |
| Origin | <https://github.com/nothings/stb>, commit `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760` (2024-07-15); raw file at `https://raw.githubusercontent.com/nothings/stb/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/stb_truetype.h` |
| License | Public domain (dual MIT / Unlicense; the packages elect MIT, see `packaging/notices/stb_truetype.txt`) |
| SHA-256 | `stb_truetype.h` `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab` (5079 lines) |
| Used by | `src/app/ui_ttf.c`: rasterises the Inter glyphs of the first-run screen |

`src/app/ui_ttf.c` is the one translation unit that defines
`STB_TRUETYPE_IMPLEMENTATION`; only the glyph-bitmap path is used. The fonts it
reads are packaged data (`assets/fonts/`, Inter 4.001, SIL OFL 1.1, see
`assets/fonts/OFL.txt`), loaded at run time from `<prefix>/share/opendana/fonts`
and never compiled into the binary.

To update: download the single header from the pinned commit, replace it,
update the version, commit and hash above, and re-run `bash tests/run.sh`.
