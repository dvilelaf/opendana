cmake_minimum_required(VERSION 3.16)
project(opendana VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options. The defaults reproduce the historic Linux build exactly.
# ---------------------------------------------------------------------------
option(OD_BUILD_APP   "Build the SDL2 front end (opendana)" ON)
option(OD_BUILD_TESTS "Build the content-free public unit test runner" ON)
option(OD_STATIC      "Prefer static linking (SDL2-static / MSVC static CRT)" OFF)
option(OD_EMSCRIPTEN  "Build for Emscripten (web)" OFF)
option(OD_ANDROID     "Build for Android (shared library entry point)" OFF)
option(OD_IOS         "Build for iOS" OFF)
# C5.11/C6.30: install a self-contained runtime bundle (rpath to the bundled
# SDL2, README, LICENSE and NOTICE). Packages are assembled only by the
# packaging scripts. The default build is unchanged.
option(OD_PACKAGE "Install a self-contained runtime bundle (used by packaging/)" OFF)
# C6.30: no build option can install or embed the extracted `assets/` dump or
# any other material of the original game (docs/PLAN-V2.md §9.2). The former
# switches are refused loudly so a stale cache cannot ship original material.
foreach(_od_retired IN ITEMS OD_PACKAGE_ASSETS OD_INSTALL_ASSETS)
  if(${_od_retired})
    message(FATAL_ERROR "${_od_retired} was removed (C6.30): packages never contain "
                        "original material; the player imports the ROM at runtime.")
  endif()
endforeach()
# C6.30: the commit printed by `opendana --version`. Empty = automatic: the commit
# that `git archive` stamped into packaging/source-commit.txt (source archives),
# else `git rev-parse HEAD` (+"-dirty" with uncommitted tracked changes), else
# "unknown". The packaging gate compares it with the shipped source archive.
set(OD_GIT_COMMIT "" CACHE STRING "Commit embedded in --version (empty = automatic)")
set(OD_EMSCRIPTEN_SHELL "" CACHE FILEPATH
    "HTML shell for the Emscripten build (defaults to src/platform/web/shell.html)")
# C5.11 macOS: path to the official universal SDL2.framework (used instead of a
# CMake SDL2 config package when building the .app on the Tahoe VM).
set(OD_SDL2_FRAMEWORK "" CACHE PATH "Path to SDL2.framework (macOS packaging)")

# ---------------------------------------------------------------------------
# Target platform detection. Every branch is guarded so a plain Linux build is
# byte-for-byte the same as before.
# ---------------------------------------------------------------------------
if(OD_EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(OD_TARGET_EMSCRIPTEN TRUE)
endif()
if(OD_ANDROID OR ANDROID)
  set(OD_TARGET_ANDROID TRUE)
endif()
if(OD_IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS)
  set(OD_TARGET_IOS TRUE)
endif()

if(APPLE)
  # C6.70: the sources ask for POSIX 2008 (_POSIX_C_SOURCE) and Darwin then hides st_mtimespec,
  # O_NOFOLLOW and mkdtemp unless this is defined as well.
  add_compile_definitions(_DARWIN_C_SOURCE)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(OD_STATIC AND MSVC)
  # Static CRT, so the .exe needs no MSVC redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(OD_TARGET_ANDROID)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# opendana_core - pure C11 model + platform layer, no SDL. Buildable and testable
# on its own on every platform, including ones without a display.
# ---------------------------------------------------------------------------
file(GLOB OD_CORE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/import/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.c")
# Vendored, license-tracked dependencies. cJSON parses the content package
# (external/cJSON, MIT, version pinned in external/README.md) and stb_image
# decodes the indexed CHR PNG sheets (external/stb, public domain v2.30).
list(APPEND OD_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/external/cJSON/cJSON.c")
add_library(opendana_core STATIC ${OD_CORE_SOURCES})
add_library(opendana::core ALIAS opendana_core)
set_target_properties(opendana_core PROPERTIES EXPORT_NAME core)
target_include_directories(opendana_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/assets>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(opendana_core PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/cJSON>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/stb>")

# stb_image decodes the PNG sheets and uses the C math library.
if(UNIX AND NOT APPLE)
  target_link_libraries(opendana_core PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# opendana_ext - pure C11 extension layer (PLAN-V2 sections 2.1/3): settings,
# input map, states, editor model and catalog. No SDL, with its own tests, so
# every agent of the V2 wave can build and check it without a window.
# ---------------------------------------------------------------------------
file(GLOB OD_EXT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ext/*.c")
add_library(opendana_ext STATIC ${OD_EXT_SOURCES})
add_library(opendana::ext ALIAS opendana_ext)
set_target_properties(opendana_ext PROPERTIES EXPORT_NAME ext)
target_link_libraries(opendana_ext PUBLIC opendana_core)
target_include_directories(opendana_ext PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# ---------------------------------------------------------------------------
# opendana_tests - core test suite. Needs no SDL, so it runs everywhere. The pure
# interface strings are compiled in directly (they have no SDL dependency).
# ---------------------------------------------------------------------------
# Optional hook for an extra test runner: with -DOD_BUILD_PRIVATE_TESTS=ON the
# file tests/lab-tests.cmake is included if it exists, and the directory given
# in OD_PRIVATE_TEST_VIEW is passed to it. Without them the only test target is
# the public unit runner below, which needs nothing of the original game.
option(OD_BUILD_PRIVATE_TESTS "Build an extra test runner defined by tests/lab-tests.cmake" OFF)
set(OD_PRIVATE_TEST_VIEW "" CACHE PATH "Directory read by the extra test runner")
if(OD_BUILD_TESTS AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/public-tests-cases.inc")
  # C6.30: the corresponding-source archive of a package carries no tests/.
  message(STATUS "tests/ not present (source archive): the test runner is not built")
  set(OD_BUILD_TESTS OFF)
endif()
if(OD_BUILD_TESTS)
  # The unit sources are the `unit` rows of the dependency inventories that
  # are present: the public one always, the lab one only in the lab.
  file(GLOB OD_CASE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/tests/*-tests-cases.inc")
  set(OD_UNIT_ROWS "")
  foreach(case_file IN LISTS OD_CASE_FILES)
    file(STRINGS "${case_file}" file_rows REGEX "\"unit\"")
    list(APPEND OD_UNIT_ROWS ${file_rows})
  endforeach()
  set(OD_UNIT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.c")
  foreach(row IN LISTS OD_UNIT_ROWS)
    string(REGEX MATCH "\{\"([^\"]+)\"" matched "${row}")
    if(NOT matched)
      message(FATAL_ERROR "Invalid unit inventory row: ${row}")
    endif()
    list(APPEND OD_UNIT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/${CMAKE_MATCH_1}")
  endforeach()
  list(REMOVE_DUPLICATES OD_UNIT_SOURCES)
  set(OD_TEST_UI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/render/png.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ui_strings.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ui_font.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ui_editor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ui_catalog.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ui_import.c")
  add_executable(opendana_tests_public ${OD_UNIT_SOURCES} ${OD_TEST_UI_SOURCES})
  target_compile_definitions(opendana_tests_public PRIVATE OD_PUBLIC_TESTS_ONLY _XOPEN_SOURCE=700)
  set(OD_TEST_TARGETS opendana_tests_public)
  if(OD_BUILD_PRIVATE_TESTS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/lab-tests.cmake")
      message(FATAL_ERROR "OD_BUILD_PRIVATE_TESTS=ON needs tests/lab-tests.cmake and "
        "-DOD_PRIVATE_TEST_VIEW=<dir>; tests/lab-tests.cmake is not present.")
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/tests/lab-tests.cmake")
  endif()
  if(TARGET opendana_tests)
    list(APPEND OD_TEST_TARGETS opendana_tests)
  endif()
  foreach(test_target IN LISTS OD_TEST_TARGETS)
    target_link_libraries(${test_target} PRIVATE opendana_core opendana_ext)
    target_include_directories(${test_target} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/import"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
      "${CMAKE_CURRENT_SOURCE_DIR}/external")
  endforeach()
  # Public unit tests read only public fixtures of this tree.
  target_compile_definitions(opendana_tests_public PRIVATE OD_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# SDL2 discovery for the front end only. Three paths, all optional:
#   * emcc            -> SDL2 comes from the Emscripten port (-sUSE_SDL=2);
#   * SDL2 CMake pkg  -> SDL2::SDL2 / SDL2::SDL2-static (all desktop OSes);
#   * pkg-config      -> PkgConfig::SDL2PC (BSD/distro packages).
# When none is available the core and tests still build.
# ---------------------------------------------------------------------------
set(OD_SDL_AVAILABLE FALSE)
set(OD_SDL_TARGET "")
set(OD_SDL_INCLUDE_DIRS "")
set(OD_SDL_LIBRARIES "")

if(OD_BUILD_APP)
  if(OD_TARGET_EMSCRIPTEN)
    set(OD_SDL_AVAILABLE TRUE)
  elseif(APPLE AND OD_SDL2_FRAMEWORK)
    # C5.11: link an SDL2.framework directly (-DOD_SDL2_FRAMEWORK=<path>), without going
    # through the CMake package that the official framework also carries. The .app puts it under Contents/Frameworks, which
    # the packaged binary reaches through `@executable_path/../Frameworks`.
    set(SDL2_FOUND TRUE)
    set(SDL2_INCLUDE_DIRS "${OD_SDL2_FRAMEWORK}/Headers")
    # C6.70: a .framework path is a library CMake links with -framework (the old
    # "-framework" "SDL2" list became -lSDL2) and this branch never marked SDL available,
    # so the front end was not built from it.
    set(SDL2_LIBRARIES "${OD_SDL2_FRAMEWORK}")
    set(OD_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
    set(OD_SDL_LIBRARIES ${SDL2_LIBRARIES})
    set(OD_SDL_AVAILABLE TRUE)
    # The framework's own headers include <SDL2/...>: -F makes that spelling resolve.
    add_compile_options("-F${OD_SDL2_FRAMEWORK}/..")
  else()
    find_package(SDL2 CONFIG QUIET)
    if(NOT SDL2_FOUND)
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(SDL2PC QUIET IMPORTED_TARGET sdl2)
        if(SDL2PC_FOUND)
          set(SDL2_FOUND TRUE)
        endif()
      endif()
    endif()

    if(OD_STATIC AND TARGET SDL2::SDL2-static)
      set(OD_SDL_TARGET SDL2::SDL2-static)
    elseif(TARGET SDL2::SDL2)
      set(OD_SDL_TARGET SDL2::SDL2)
    elseif(TARGET PkgConfig::SDL2PC)
      set(OD_SDL_TARGET PkgConfig::SDL2PC)
    elseif(SDL2_FOUND)
      set(OD_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
      set(OD_SDL_LIBRARIES ${SDL2_LIBRARIES})
    endif()

    if(OD_SDL_TARGET OR SDL2_FOUND)
      set(OD_SDL_AVAILABLE TRUE)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Version string of the front ends: project version plus the source commit.
# ---------------------------------------------------------------------------
set(OD_SOURCE_COMMIT "${OD_GIT_COMMIT}")
if(NOT OD_SOURCE_COMMIT)
  set(_od_stamp "${CMAKE_CURRENT_SOURCE_DIR}/packaging/source-commit.txt")
  if(EXISTS "${_od_stamp}")
    file(STRINGS "${_od_stamp}" _od_stamped LIMIT_COUNT 1)
    string(LENGTH "${_od_stamped}" _od_stamp_len)
    if(_od_stamped MATCHES "^[0-9a-f]+$" AND _od_stamp_len EQUAL 40)
      set(OD_SOURCE_COMMIT "${_od_stamped}")
    endif()
  endif()
endif()
if(NOT OD_SOURCE_COMMIT)
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse HEAD
                    OUTPUT_VARIABLE _od_head OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _od_head_rc ERROR_QUIET)
    if(_od_head_rc EQUAL 0 AND _od_head MATCHES "^[0-9a-f]+$")
      execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                              status --porcelain --untracked-files=no
                      OUTPUT_VARIABLE _od_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      set(OD_SOURCE_COMMIT "${_od_head}")
      if(_od_dirty)
        string(APPEND OD_SOURCE_COMMIT "-dirty")
      endif()
    endif()
  endif()
endif()
if(NOT OD_SOURCE_COMMIT)
  set(OD_SOURCE_COMMIT "unknown")
endif()
set(OD_VERSION_STRING "${PROJECT_VERSION} (commit ${OD_SOURCE_COMMIT})")
message(STATUS "opendana --version: ${OD_VERSION_STRING}")

# ---------------------------------------------------------------------------
# opendana - SDL2 front end (only this target links SDL).
# ---------------------------------------------------------------------------
if(OD_BUILD_APP AND OD_SDL_AVAILABLE)
  file(GLOB OD_APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.c")
  file(GLOB OD_RENDER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/render/*.c")

  if(OD_TARGET_ANDROID)
    # Android has no main(); SDL provides the entry point via a shared library.
    add_library(opendana SHARED ${OD_APP_SOURCES} ${OD_RENDER_SOURCES})
    set_target_properties(opendana PROPERTIES PREFIX "")
  else()
    set(OD_WIN_ICON_RC "")
    if(WIN32)
      # C6.42: embed packaging/icon/opendana.ico as the .exe icon resource.
      enable_language(RC)
      set(OD_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/opendana.ico")
      # C6.51: version resource (VERSIONINFO) and manifest (per-monitor DPI,
      # UTF-8 code page, asInvoker) in the same resource script.
      set(OD_MANIFEST_FILE "${CMAKE_CURRENT_BINARY_DIR}/opendana.manifest")
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/opendana.manifest.in"
                     "${OD_MANIFEST_FILE}" @ONLY)
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/opendana.rc.in"
                     "${CMAKE_CURRENT_BINARY_DIR}/opendana.rc" @ONLY)
      set(OD_WIN_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/opendana.rc")
      set_source_files_properties("${OD_WIN_ICON_RC}" PROPERTIES OBJECT_DEPENDS "${OD_ICON_ICO};${OD_MANIFEST_FILE}")
    endif()
    add_executable(opendana ${OD_APP_SOURCES} ${OD_RENDER_SOURCES} ${OD_WIN_ICON_RC})
  endif()

  target_link_libraries(opendana PRIVATE opendana_core opendana_ext)
  # Static SDL2main refers to SDL2 symbols: put it before SDL2 for one-pass
  # linkers (MinGW). Linux/macOS desktop do not need this entry-point glue.
  if((WIN32 OR OD_TARGET_ANDROID OR OD_TARGET_IOS) AND TARGET SDL2::SDL2main)
    target_link_libraries(opendana PRIVATE SDL2::SDL2main)
  endif()
  if(OD_SDL_TARGET)
    target_link_libraries(opendana PRIVATE ${OD_SDL_TARGET})
  else()
    target_include_directories(opendana PRIVATE ${OD_SDL_INCLUDE_DIRS})
    target_link_libraries(opendana PRIVATE ${OD_SDL_LIBRARIES})
  endif()
  # C6.48: stb_truetype (fonts) and stb_image declarations (the icon) for the first-run view.
  target_include_directories(opendana PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/stb")
  target_compile_definitions(opendana PRIVATE OD_VERSION="${OD_VERSION_STRING}")
  if(WIN32)
    # C6.21: the first-run ROM picker uses the Win32 common file dialog.
    target_link_libraries(opendana PRIVATE comdlg32 ole32)
  endif()

  # C5.11 packaging: the installed binary finds the bundled SDL2 through a
  # relative rpath, so the package runs without touching LD_LIBRARY_PATH.
  if(OD_PACKAGE)
    if(APPLE AND NOT OD_TARGET_IOS)
      set_target_properties(opendana PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
    elseif(UNIX)
      set_target_properties(opendana PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib")
    endif()
  endif()

  # opendana-dev: the same front end with the development options compiled in
  # (scene shortcuts such as --level/--boss/--ending/--map used by the harness
  # and the measurements). The shipped opendana does not
  # accept them: every scene is reached from reset with buttons.
  if(NOT OD_TARGET_ANDROID AND NOT OD_TARGET_EMSCRIPTEN AND NOT OD_TARGET_IOS)
    add_executable(opendana-dev ${OD_APP_SOURCES} ${OD_RENDER_SOURCES})
    target_link_libraries(opendana-dev PRIVATE opendana_core opendana_ext)
    if(WIN32 AND TARGET SDL2::SDL2main)
      target_link_libraries(opendana-dev PRIVATE SDL2::SDL2main)
    endif()
    if(OD_SDL_TARGET)
      target_link_libraries(opendana-dev PRIVATE ${OD_SDL_TARGET})
    else()
      target_include_directories(opendana-dev PRIVATE ${OD_SDL_INCLUDE_DIRS})
      target_link_libraries(opendana-dev PRIVATE ${OD_SDL_LIBRARIES})
    endif()
    target_include_directories(opendana-dev PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/stb")
    target_compile_definitions(opendana-dev PRIVATE OD_VERSION="${OD_VERSION_STRING}"
                               OD_DEV_TOOLS=1)
    if(WIN32)
      # C6.21: the first-run ROM picker uses the Win32 common file dialog.
      target_link_libraries(opendana-dev PRIVATE comdlg32 ole32)
    endif()
  endif()

  if(OD_TARGET_ANDROID)
    find_library(OD_ANDROID_LOG log)
    if(OD_ANDROID_LOG)
      target_link_libraries(opendana PRIVATE ${OD_ANDROID_LOG})
    endif()
  endif()

  if(OD_TARGET_EMSCRIPTEN)
    set_target_properties(opendana PROPERTIES SUFFIX ".html")
    set(_od_shell "${OD_EMSCRIPTEN_SHELL}")
    if(NOT _od_shell)
      set(_od_shell "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/shell.html")
    endif()
    # C6.30: nothing is preloaded. The former `--preload-file assets` embedded
    # the extracted dump of the original game into the page; the web build
    # must obtain content from the player's own import like every other target.
    target_compile_options(opendana PRIVATE "-sUSE_SDL=2")
    target_link_options(opendana PRIVATE
      "-sUSE_SDL=2"
      "-sALLOW_MEMORY_GROWTH=1"
      "--shell-file" "${_od_shell}")
  endif()
elseif(OD_BUILD_APP)
  message(WARNING "SDL2 not found: opendana_core/opendana_tests are built, opendana is skipped. "
                  "Pass -DCMAKE_PREFIX_PATH=<sdl2-install> to build it.")
endif()

# ---------------------------------------------------------------------------
# Tests (CTest).
# ---------------------------------------------------------------------------
if(OD_BUILD_TESTS)
  enable_testing()
  # Private/content comparators run through the explicit suite gates, not an
  # implicit default CTest run in public clones without original resources.
  add_test(NAME public_unit COMMAND opendana_tests_public --category unit)
endif()

# ---------------------------------------------------------------------------
# Install / export. Emscripten ships a single self-contained .html, so it is
# deliberately skipped there.
# ---------------------------------------------------------------------------
if(NOT OD_TARGET_EMSCRIPTEN)
  set(OD_CMAKECONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/opendana")
  # The developer install (headers, core library, CMake package) is not part
  # of a published game bundle; skip it entirely under -DOD_PACKAGE=ON.
  if(NOT OD_PACKAGE)
    install(TARGETS opendana_core EXPORT opendanaCoreTargets
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Development
      INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT Development)
    install(EXPORT opendanaCoreTargets
            NAMESPACE opendana::
            DESTINATION "${OD_CMAKECONFIG_DIR}"
            COMPONENT Development)
    # Minimal package config so downstream projects can `find_package(opendana)`.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/opendanaConfig.cmake"
      "include(\"\${CMAKE_CURRENT_LIST_DIR}/opendanaCoreTargets.cmake\")\n")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/opendanaConfig.cmake"
            DESTINATION "${OD_CMAKECONFIG_DIR}" COMPONENT Development)
  endif()
  if(TARGET opendana)
    install(TARGETS opendana
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime)
  endif()

  # -------------------------------------------------------------------------
  # C5.11 runtime bundle. Everything below lands in the `Runtime` CPack
  # component only; the developer install (headers, core library, CMake
  # package) stays out of the published package.
  # -------------------------------------------------------------------------
  if(OD_PACKAGE AND TARGET opendana)
    # The user README, the GPL text and the project NOTICE always travel with
    # the game. Third-party notices and corresponding sources are added by the
    # packaging scripts through packaging/license-bundle.sh (C6.30).
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/README-package.md"
            DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT Runtime RENAME README.md)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
            DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT Runtime)
    # C6.48: the first-run screen loads Inter (SIL OFL 1.1, with its licence)
    # and the 128 px icon at run time from <prefix>/share/opendana/.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf"
                  "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-SemiBold.ttf"
                  "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL.txt"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/opendana/fonts" COMPONENT Runtime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/png/opendana-128.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/opendana/ui" COMPONENT Runtime)
    # C6.42: freedesktop hicolor icons (Linux tar); Windows has the .exe resource.
    if(UNIX AND NOT APPLE)
      foreach(_n 16 24 32 48 64 128 256 512)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/png/opendana-${_n}.png"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_n}x${_n}/apps"
                COMPONENT Runtime RENAME opendana.png)
      endforeach()
      # C6.44: the vector icon, so desktops pick their own size (TAR and AppImage).
      install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/opendana.svg"
              DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
              COMPONENT Runtime)
    endif()

    # Bundle SDL2 so the package does not depend on the host. On macOS the
    # official SDL2.framework is placed inside the .app by packaging/build-macos.sh.
    if(TARGET SDL2::SDL2 AND NOT APPLE)
      if(WIN32)
        # On MinGW the SDL2 target is the import library; the DLL itself is
        # passed by packaging/build-windows.sh as OD_SDL2_DLL.
        set(_sdl2_dll "${OD_SDL2_DLL}")
        if(NOT _sdl2_dll)
          set(_sdl2_dll "$<TARGET_FILE:SDL2::SDL2>")
        endif()
        install(FILES "${_sdl2_dll}"
                DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
                RENAME SDL2.dll)
      else()
        install(FILES $<TARGET_FILE:SDL2::SDL2>
                DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime
                RENAME libSDL2-2.0.so.0)
      endif()
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# CPack. C6.30 retired the Linux TGZ and the Windows ZIP generators and C6.70 the macOS
# DragNDrop one: they produced the published file names without the licence gate or the ROM
# auditor. The packages come only from packaging/build-linux.sh, packaging/build-windows.sh
# and the macOS driver (OpenDana.app + .dmg made with hdiutil).
# ---------------------------------------------------------------------------
