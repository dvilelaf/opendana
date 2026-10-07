/* platform.h - portable path/asset layer (pure C11, no SDL).
 *
 * The game ships its resources in `assets/` and is meant to run on desktop,
 * web and mobile without a ROM. This layer answers two questions that used to
 * be hard-coded in the front end:
 *
 *   1. Where are the committed assets?  -> od_platform_resolve_assets()
 *   2. Where may the game write user data (saves, config)?
 *                                       -> od_platform_user_data_dir()
 *
 * Defaults per platform:
 *
 *   Linux/BSD : assets next to the executable, then ../assets, then "assets";
 *               data under $XDG_DATA_HOME/opendana or ~/.local/share/opendana.
 *   macOS     : same asset search; data under
 *               ~/Library/Application Support/opendana.
 *   Windows   : same asset search; data under %APPDATA%\opendana.
 *   Emscripten: "/assets" (filled by --preload-file) and "/persistent/opendana".
 *   Android   : "assets" (the APK payload, read through SDL_RWops by the
 *               front end) and $OD_DATA_DIR / app-private storage.
 *   iOS       : "assets" inside the app bundle and $HOME/Documents/opendana.
 *
 * Overrides, in order of precedence:
 *   1. the explicit `cli_override` argument (the `--assets` flag);
 *   2. the OD_ASSETS environment variable;
 *   3. the platform default above.
 *
 * The user-data directory can be redirected with OD_DATA_DIR.
 */
#ifndef OD_PLATFORM_H
#define OD_PLATFORM_H

#include "opendana/common.h"

#include <stddef.h>

/* "linux", "macos", "windows", "android", "ios", "emscripten" or "unix".
 * Never NULL and never empty. */
const char *od_platform_name(void);

/* Join `dir` and `name` with '/' (accepted by fopen on every supported OS).
 * An empty or "." directory yields just `name`. Always returns `out`. */
char *od_platform_join(char *out, size_t cap, const char *dir, const char *name);

/* Directory holding the running executable, without the file name. Returns 0
 * (and writes "") on platforms where that is not meaningful (Emscripten,
 * Android, iOS); returns non-zero on success. */
int od_platform_executable_dir(char *out, size_t cap);

/* Whether `dir` looks like an opendana asset directory, i.e. contains
 * `levels.bin`. 1 if yes, 0 otherwise. */
int od_platform_asset_dir_valid(const char *dir);

/* Resolve the asset directory. `cli_override` may be NULL. The result is
 * always a non-empty path on success. Returns OD_ERR_RANGE when the buffer
 * is too small. */
od_status od_platform_resolve_assets(char *out, size_t cap, const char *cli_override);

/* Writable per-user data directory. When `create` is non-zero the directory
 * is created (including missing parents) and OD_ERR_IO is returned if that
 * fails. The resolved path is written to `out`. */
od_status od_platform_user_data_dir(char *out, size_t cap, int create);

/* od_platform_user_data_dir() joined with `name`, convenient for save
 * files. `create` has the same meaning as above. */
od_status od_platform_user_data_path(char *out, size_t cap, const char *name, int create);

#endif /* OD_PLATFORM_H */
