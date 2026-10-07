/* desktop_integration.h - opt-in freedesktop integration (C6.46), Linux only.
 *
 * Installs a launcher (opendana.desktop) and the hicolor icons in the user's
 * XDG data home so docks and menus show a themed, sharp icon instead of
 * upscaling the 32 px window icon. Nothing is written without an explicit
 * request (first-run question, Settings row or --install-desktop). On other
 * platforms every call returns OD_DESKTOP_UNSUPPORTED and does nothing.
 * Pure C11, no SDL. */
#ifndef OD_DESKTOP_INTEGRATION_H
#define OD_DESKTOP_INTEGRATION_H

#include <stddef.h>

#define OD_DESKTOP_MARKER "X-OpenDana-Generated=true"

enum {
    OD_DESKTOP_OK = 0,
    OD_DESKTOP_UNSUPPORTED,    /* not Linux                                     */
    OD_DESKTOP_NO_ICONS,       /* the package's icons were not found            */
    OD_DESKTOP_FOREIGN,        /* a .desktop of that name exists and is not ours */
    OD_DESKTOP_ERROR           /* I/O error                                      */
};

typedef struct {
    const char *data_home;     /* NULL: $XDG_DATA_HOME, else $HOME/.local/share */
    const char *exec_path;     /* NULL: $APPIMAGE, else the running binary      */
    const char *icon_src;      /* NULL: found next to the running package       */
} od_desktop_cfg;

/* 1 on Linux. */
int od_desktop_supported(void);

/* 1 when running from a package that can offer the integration: an AppImage
 * ($APPIMAGE set) or the TAR's binary (share/icons/hicolor next to bin/).
 * 0 for a development build, so developers are never asked. */
int od_desktop_packaged(const od_desktop_cfg *cfg);

/* 1 when our (marked) launcher is installed. */
int od_desktop_installed(const od_desktop_cfg *cfg);

/* Install or refresh. `msg` receives a one-line English description. */
int od_desktop_install(const od_desktop_cfg *cfg, char *msg, size_t cap);

/* Remove only what is ours: the marked launcher and the icons named
 * opendana.png / opendana.svg. A launcher without the marker is left alone
 * and the message says so. */
int od_desktop_uninstall(const od_desktop_cfg *cfg, char *msg, size_t cap);

#endif
