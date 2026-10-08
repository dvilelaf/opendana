# Releasing OpenDana

How the Linux (TAR, AppImage) and Windows (ZIP) packages are built, checked and
published. The version comes from `CMakeLists.txt` (`project(... VERSION x.y.z)`).
macOS: section 7 (universal `.dmg`, ad hoc signed, built on a macOS host).

Every package is built from a **source archive of one git commit**, so the
binary, its source and the commit it prints (`opendana --version`) always
agree.

## 0. Inputs

The scripts assume no machine-specific path and download nothing. If an input is
missing they stop and say which one. Inputs are named by environment variables;
the pinned identities (hashes) are in `packaging/pins.sh`.

| Variable | What it is |
|---|---|
| `OD_ROM` | your copy of the EU ROM, read only: the auditor needs it (SHA-256 `853646c5...`) |
| `SDL2_SRC` | a git tree of SDL that contains the pinned commit (SDL 2.32.10) |
| `SDL2_DIR` | (Linux) optional: an SDL2 prefix whose `lib/libSDL2-2.0.so.0` has the pinned SHA-256. If unset, `packaging/build-sdl2.sh --target linux` builds it from `SDL2_SRC` into `build/deps/` |
| `MINGW_SDL2_DIR` | (Windows) optional: a MinGW SDL2 prefix whose `bin/SDL2.dll` has the pinned SHA-256. If unset, `packaging/build-sdl2.sh --target windows` builds it into `build/deps/` |
| `OD_SOURCE_COMMIT` | optional: commit to package (default `HEAD`, which must be clean) |
| `SOURCE_DATE_EPOCH` | optional: fixed timestamp for the archives (default: the commit's date) |

Example, with paths of your own:

```sh
export OD_ROM=/path/to/your-rom.nes
export SDL2_SRC=/path/to/SDL            # git tree containing the pinned commit
# SDL2_DIR and MINGW_SDL2_DIR are optional (built automatically, see below)
```

SDL2 is built from the same source archive that ships as `sources/SDL2.tar.xz`,
offline, with neutral paths (`-ffile-prefix-map`), a fixed `SOURCE_DATE_EPOCH`, no
PE time stamp and stripped:

```sh
SDL2_SRC=/path/to/SDL bash packaging/build-sdl2.sh --target linux     # build/deps/sdl2-2.32.10-linux
SDL2_SRC=/path/to/SDL bash packaging/build-sdl2.sh --target windows   # build/deps/sdl2-2.32.10-windows
```

Two builds give the same SHA-256 (the one in `packaging/pins.sh`). The package
binaries (`opendana`, `opendana.exe`, the SDL libraries) are installed stripped.

Host tools: `git`, `cmake`, a C compiler, `tar`, `gzip`, `xz`, `python3`,
`readelf`, `strip`, `ninja` (or `make`). For Windows, MinGW-w64 (`x86_64-w64-mingw32-gcc` from the Debian or
Ubuntu packages named in `packaging/notices/windows/mingw-w64-runtime.txt`; the
script requires those versions because the notice was reviewed for them) and,
optionally, `wine64`. For the AppImage, `mksquashfs` and, if the runtime has to be
built, Docker.

## 1. Build

```sh
bash packaging/build-linux.sh        # TAR
bash packaging/build-linux.sh --mode appimage   # TAR + AppImage (section 4)
bash packaging/build-windows.sh      # REQUIRE_WINE64=1 demands the test under wine64
```

Outputs go to `build/dist*/`. A failed run never touches earlier outputs.

| File | Contents |
|---|---|
| `opendana-X.Y.Z-Linux-x86_64.tar.gz` | `bin/opendana`, `lib/libSDL2-2.0.so.0`, `share/doc/opendana/` with the README, LICENSE, NOTICE and `license-bundle/` |
| `opendana-X.Y.Z-Windows-x86_64.zip` | `bin/opendana.exe`, `bin/SDL2.dll`, the same `share/doc/opendana/` |
| `*.audit.json` | the ROM auditor's report for that package |
| `SHA256SUMS` | hashes of the package and of its report |

`license-bundle/` holds the notices of every third-party component that ships in
the package, the complete corresponding source (`sources/`), `RELINK.txt`,
`SOURCES.json`, `BUILD-IDENTITY.json` and its own `SHA256SUMS`.

## 2. What the scripts do

1. **Corresponding source, generated and never committed**
   (`packaging/make-sources.sh`): the application source is the `git archive` of
   the commit restricted to the paths in `packaging/application-source.list`;
   SDL, cJSON and stb come from the pinned sources.
2. **Build from that archive**, not from the working tree. `opendana --version`
   prints the version and the commit; the script checks it.
3. **Licence gate** (`packaging/license-bundle.sh`): component versions are
   proven in the source, notices are not empty, binaries are tied to their pinned
   hashes, and the source archive must be the one of the commit the binary
   reports.
4. **Reproducible archives**: sorted members, fixed owner, time and permissions.
5. **ROM auditor** (`packaging/audit-package.sh`) over the whole package,
   including the content of every source archive. It fails on any window of
   your ROM that is not explained (headers, padding, low entropy, present in a
   third-party binary), on any ROM image and on any dump format.
6. Windows also runs `tests/windows-package.sh` (closed inventory, x86-64 PE,
   pinned DLL, no MinGW DLLs, notices, hashes) and the `--version` test under
   `wine64`.

## 3. Check before publishing

```sh
bash tests/linux-package-unit.sh                 # no inputs needed
bash tests/linux-package-sources.sh
bash tests/linux-package-inventory.sh build/dist/opendana-X.Y.Z-Linux-x86_64.tar.gz <commit>
bash tests/windows-package-controls.sh           # needs MINGW_SDL2_DIR
bash tests/package-leaks.sh <tar.gz> <zip> [AppImage]   # paths, user/host, e-mails, names, PE time stamps
( cd build/dist && sha256sum -c SHA256SUMS )
( cd build/dist-win && sha256sum -c SHA256SUMS )
```

Then unpack each package in a clean user account, import your ROM, start the
game and play a few levels. Look at the screenshot, do not only trust the
hashes.

## 4. The AppImage

`bash packaging/build-linux.sh --mode appimage` produces, next to the TAR:

| File | Content |
|---|---|
| `opendana-<ver>-x86_64.AppImage` | our runtime + a reproducible SquashFS (`-all-root -no-xattrs`, fixed times, one processor, gzip). No test marker |
| `opendana-<ver>-x86_64.AppImage.audit.json` | ROM-auditor report over the AppImage tree, the runtime and the runtime source archive |
| `opendana-<ver>-appimage-runtime-source.tar.xz` | LGPL-2.1 source and relink offer: upstream tarballs of every runtime component, `sources.lock`, `toolchain.lock`, `Dockerfile`, `build.sh`, `container-build.sh`, notices and `RELINK.txt` |

The **runtime is built, never copied**: `packaging/appimage-runtime/build.sh`
compiles it inside an Alpine 3.21.8 container pinned by digest (`toolchain.lock`: exact
`apk` versions, clang 19.1.4, musl 1.2.5-r11) **without network**, with every linked
library built from the sources of `sources.lock` (type2-runtime `8f39b89`, libfuse 3.15.0,
squashfuse 0.5.2, zstd 1.5.6, zlib 1.3.2, mimalloc 2.1.7; musl comes from the `musl-dev`
package). Deterministic flags: fixed `SOURCE_DATE_EPOCH`, `-ffile-prefix-map`,
`--build-id=none`, `strip`. **Two clean builds give the same SHA-256**, pinned in
`packaging/pins.sh` (`OD_PIN_APPIMAGE_RUNTIME_SHA256`); a runtime with another hash is
never used. To change a source or the compiler: `bash packaging/appimage-runtime/build.sh
--twice`, review `packaging/notices/appimage/`, update the pin.

Extra inputs: `OD_RUNTIME_SOURCES` (default `build/appimage-runtime/sources`: the
tarballs of the lock; `OD_RUNTIME_FETCH=1` downloads the missing ones from the lock's
URLs, the only download of the script, each file checked by SHA-256) and the optional
`OD_APPIMAGE_RUNTIME` (an already built runtime, checked against the pin). Without it the
script uses `build/appimage-runtime/out/runtime-x86_64` if it matches the pin, or builds it.

The licence gate (`license-bundle.sh --mode appimage`) requires, on top of the TAR set, the
seven runtime components (`type2-runtime`, `libfuse`, `squashfuse`, `zstd`, `zlib`,
`mimalloc`, `musl`) with notice, version proved inside its own source archive and
corresponding source; the runtime bound by SHA-256; libfuse declared static with the full
LGPL-2.1 text in its notice; and a `RELINK.txt` that explains how to rebuild the runtime with
another libfuse. The old `--mode appimage-test` is retired: this mode replaces it.

Checks:

```sh
python3 tests/linux-package-appimage-gate.py        # gate controls, no inputs (also in --unit)
bash tests/linux-package-appimage-runtime-build.sh  # two runtime builds = pin (Docker)
bash tests/linux-package-appimage-smoke.sh build/dist/opendana-<ver>-x86_64.AppImage <commit>
bash tests/linux-package-appimage-xvfb.sh build/dist/opendana-<ver>-x86_64.AppImage
```

The smoke test compares the runtime at the head of the image with the pin, runs `--version`
(also through a real FUSE mount when the host allows it), checks the runtime notices inside
the image, imports your own game file read in place and plays 600 frames headless.

## 5. Publish

1. A clean tree at the commit you want to publish.
2. Both build scripts green.
3. Section 3 green, and the packages played.
4. Publish together, per package: the archive, its `*.audit.json` and
   `SHA256SUMS`.
5. Release notes: version, commit, platforms tested and known limits (section 6).

## 6. Known limits

- Windows was tested under wine64, not on a real Windows machine. Non-ASCII ROM
  paths and a missing `HOME` are known limitations on Windows; the package README
  says so.
- The auditor checks bytes, versions and the presence of notices; it does not
  certify legal compliance.
- macOS: the arm64 slice is built but not run (it needs an Apple Silicon Mac); see section 7.
- The zstd source in the AppImage licence bundle and runtime source archive omits `tests/`
  (links and compressed test inputs the archive gate and the auditor cannot accept) and has
  `build/` renamed `build-files/`; `lib/`, the only part compiled, is untouched. Alpine's
  patches to musl 1.2.5-r11 are not reproduced; the upstream 1.2.5 tarball is shipped as the
  reference (MIT).

## 7. macOS (C6.70)

The macOS package is `opendana-X.Y.Z-macOS-universal.dmg`: `OpenDana.app` with the same
x86_64 + arm64 binary slices, the SDL2 shared library built from the SAME pinned source as the
Linux and Windows ones, the licence bundle and the corresponding sources inside the app, ad hoc
code signed (`codesign -s -`, not notarized: right click, Open, the first time).

How the pieces are made (the maintainers' driver does all of this on a macOS build host and is not
part of this tree, because it names that host):

1. `packaging/make-sources.sh --commit <sha> --platform macos --out DIR` makes the corresponding
   sources (`git archive` of the commit, ROM audit of the source, `SDL2.tar.xz`, notices/macos,
   relink/macos). The app is built FROM that source archive, never from the working tree.
2. On macOS (Xcode, CMake 3.30):
   `bash packaging/build-sdl2.sh --target macos --source-archive SDL2.tar.xz` builds
   `libSDL2-2.0.0.dylib` (universal, stripped, unsigned) and checks it against
   `OD_PIN_MACOS_SDL2_SHA256` in `packaging/pins.sh` (two clean builds give the same bytes);
   then
   `cmake -S <source archive> -B build -DCMAKE_BUILD_TYPE=Release -DOD_PACKAGE=ON -DOD_BUILD_TESTS=OFF
   "-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
   -DSDL2_DIR=<sdl2 prefix>/lib/cmake/SDL2` and `cmake --install build --component Runtime --strip`.
3. `packaging/write-manifest.sh --platform macos` and `packaging/license-bundle.sh --mode macos` bind
   the UNSIGNED binaries to the sources; the bundle goes to `Contents/Resources/doc/license-bundle`.
4. The app is assembled (`Contents/MacOS/opendana`, `Contents/Frameworks/libSDL2-2.0.0.dylib`,
   the fonts and icon the first-run screen loads under `Contents/share/opendana`, `opendana.icns`),
   signed ad hoc, and `hdiutil create -format UDZO` makes the `.dmg`.
5. `python3 tests/macos-package.py OpenDana.app <sha> <sdl2 pin>` checks the closed inventory, the
   universal Mach-O files, the run path and the bundle; `tests/macos-package-controls.py` is its
   positive and negative control (level 1). The ROM auditor and the leak scan run over the contents
   of the mounted `.dmg`.

Level 1 runs on macOS too (`tests/run.sh --unit` with a build dir that has the SDL2 above): the
tests that expect XDG folders expect `~/Library/Application Support` there, the three `--wrap`
tests rename the wrapped function at compile time (ld64 has no `--wrap`), and two checks are
Linux-only and say so: the freedesktop launcher (macOS answers "Not supported" and writes nothing)
and the SIGKILL case of the ROM chooser (macOS has no PDEATHSIG).

Not covered: the arm64 slice is built but needs an Apple Silicon Mac to be run; the `.dmg` file
itself is not byte-reproducible (hdiutil stamps it), its contents are.
