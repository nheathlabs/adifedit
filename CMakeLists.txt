# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)
project(adifedit VERSION 0.0.3 LANGUAGES C)

# --- Build type ------------------------------------------------------
# Single-config generators (Make, Ninja) build exactly one type. Use the
# "debug" / "release" presets, pass -DCMAKE_BUILD_TYPE=, or fall back to
# Debug for a bare `cmake -S . -B build`.
#
#   Debug           -O0 -g            no optimisation, full gdb info
#   Release         -O3 -DNDEBUG      + LTO, no debug info
#   RelWithDebInfo  -O3 -g -DNDEBUG   optimised like Release, keeps -g
set(ADIFEDIT_BUILD_TYPES Debug Release RelWithDebInfo)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${ADIFEDIT_BUILD_TYPES})
if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE IN_LIST ADIFEDIT_BUILD_TYPES)
    message(FATAL_ERROR
        "CMAKE_BUILD_TYPE '${CMAKE_BUILD_TYPE}' is not supported; "
        "use one of: ${ADIFEDIT_BUILD_TYPES}")
endif()

# RelWithDebInfo: optimise as hard as Release.
string(REPLACE "-O2" "-O3" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")

# Link-time optimisation for the fully-optimised build.
include(CheckIPOSupported)
check_ipo_supported(RESULT ADIFEDIT_IPO_OK OUTPUT ADIFEDIT_IPO_ERR LANGUAGES C)
if(ADIFEDIT_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "LTO/IPO unavailable, Release will not use it: ${ADIFEDIT_IPO_ERR}")
endif()

# Our code targets ISO C11 with no compiler extensions.
# CMAKE_C_STANDARD_REQUIRED=ON fails configuration outright if the
# compiler cannot provide C11 (rather than silently falling back);
# CMAKE_C_EXTENSIONS=OFF asks for -std=c11, not -std=gnu11.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Warning + conformance flags for our own translation units (GTK's
# headers arrive as system includes and are exempt). -pedantic-errors
# makes any use of a post-C11 feature or a GNU extension a build failure,
# so the C11 target above is actually enforced. -Wshadow catches a
# variable (or parameter) hiding an outer one - the tree is currently
# clean under it, so it costs nothing and stays that way. The
# force-included portability_guard.h poisons a set of non-portable POSIX
# calls, keeping the tree buildable for Windows/MinGW without a porting
# pass (see CONTRIBUTING.md "Portability").
#
# Dead code is a build failure (CONTRIBUTING.md "Dead code"): the unused-*
# warnings are promoted to errors - only those, not a blanket -Werror, so
# a new compiler can't break the build over an unrelated warning.
# -Wmissing-prototypes/-declarations make a function without a header
# declaration an error, so a helper must be static, and then
# -Wunused-function sees when it goes dead. Clang also gets unreachable-
# code detection; GCC accepts -Wunreachable-code but ignores it.
# Exported functions nothing calls are invisible to a per-file compiler -
# scripts/check-dead-code.sh covers those.
set(ADIFEDIT_C_FLAGS -Wall -Wextra -Wshadow -pedantic-errors
    -Wmissing-prototypes -Wmissing-declarations -Wunused-macros
    -Werror=unused-function -Werror=unused-variable
    -Werror=unused-but-set-variable -Werror=unused-label
    -Werror=unused-value -Werror=unused-parameter -Werror=unused-macros
    -Werror=missing-prototypes -Werror=missing-declarations
    -include "${CMAKE_CURRENT_SOURCE_DIR}/src/portability_guard.h")
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND ADIFEDIT_C_FLAGS -Werror=unreachable-code-aggressive)
endif()

# Emit compile_commands.json so clang-tidy (.clang-tidy) and editor
# language servers work against the real build flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# adifcore is a static archive linked into both adifedit and the tests;
# keep everything position-independent so it drops cleanly into the PIE
# executable.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build dependencies (GTK4, pkg-config, glib-compile-resources, a C11
# compiler): on Debian/Ubuntu run scripts/install-build-deps.sh.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
# The headless core (parser, model, threaded I/O) needs only GLib/GIO.
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.74 gobject-2.0 gio-2.0)

# Compile the bundled icon into a GResource source file so the in-app
# icon works when running from the build tree (no install required).
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(gresource_xml "${CMAKE_CURRENT_SOURCE_DIR}/assets/adifedit.gresource.xml")
set(gresource_srcdir "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/hicolor")
set(gresource_srcdir_assets "${CMAKE_CURRENT_SOURCE_DIR}/assets") # style.css
set(gresource_c "${CMAKE_CURRENT_BINARY_DIR}/adifedit-resources.c")

execute_process(
    COMMAND "${GLIB_COMPILE_RESOURCES}" --sourcedir=${gresource_srcdir}
            --sourcedir=${gresource_srcdir_assets} --generate-dependencies "${gresource_xml}"
    OUTPUT_VARIABLE gresource_deps
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" gresource_deps "${gresource_deps}")

add_custom_command(
    OUTPUT "${gresource_c}"
    COMMAND "${GLIB_COMPILE_RESOURCES}" --target=${gresource_c} --generate-source
            --sourcedir=${gresource_srcdir} --sourcedir=${gresource_srcdir_assets}
            "${gresource_xml}"
    DEPENDS "${gresource_xml}" ${gresource_deps}
    VERBATIM)

# adifedit_build_info.h (git hash + UTC build time) is regenerated on
# every build; src/build_info.c is its only consumer, so only that unit
# recompiles. GIT_EXECUTABLE is empty when git is absent -> "unknown".
find_package(Git QUIET)
set(build_info_h "${CMAKE_CURRENT_BINARY_DIR}/adifedit_build_info.h")
add_custom_target(adifedit_build_info
    BYPRODUCTS "${build_info_h}"
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${build_info_h}
            -DGIT=${GIT_EXECUTABLE} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_info.cmake
    COMMENT "Recording git hash and build time"
    VERBATIM)

# The GTK-free heart of the app: ADIF parsing, the AdifDocument model,
# and the threaded load/save. Unit-tested on its own in tests/.
add_library(adifcore STATIC
    src/adif_field.c
    src/adif_known_fields.c
    src/adif_parser.c
    src/adif_row.c
    src/adif_document.c
    src/adif_io.c
    src/batch_stamp.c
)
target_link_libraries(adifcore PUBLIC PkgConfig::GLIB)
target_include_directories(adifcore PUBLIC src)
target_compile_options(adifcore PRIVATE ${ADIFEDIT_C_FLAGS})

# The GTK layer over adifcore: the main window and its actions, the
# GtkColumnView table, the Batch Stamp and Find/Replace UI, the reusable
# dialogs, and the icon GResource. Built as a library so the executable,
# the screenshot helper, and any future GUI test link the exact same
# objects the app ships - each just adds its own main().
#
# OBJECT, not STATIC: the generated GResource unit registers itself from a
# constructor that no other code names, and a static archive would drop
# it (icons would vanish). An object library links every unit in.
add_library(adifeditui OBJECT
    src/window.c
    src/window_actions.c
    src/window_stamp.c
    src/window_search.c
    src/adif_table.c
    src/adif_table_view.c
    src/ui_dialogs.c
    src/build_info.c
    "${gresource_c}"
)
add_dependencies(adifeditui adifedit_build_info)
target_link_libraries(adifeditui PUBLIC adifcore PkgConfig::GTK4)
target_include_directories(adifeditui PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# PUBLIC: main.c (in the adifedit executable, not adifeditui itself) also
# needs this, for --version.
target_compile_definitions(adifeditui PUBLIC ADIFEDIT_VERSION="${PROJECT_VERSION}")
target_compile_options(adifeditui PRIVATE ${ADIFEDIT_C_FLAGS})

# The GResource blob is generated code: it embeds the icons as one huge
# string literal, which -pedantic-errors would reject, and defines
# include-guard and helper macros it never uses. Exempt just those.
set_source_files_properties("${gresource_c}" PROPERTIES
    COMPILE_OPTIONS "-Wno-overlength-strings;-Wno-unused-macros")

# The shipped app is just an entry point over adifeditui.
add_executable(adifedit src/main.c)
target_link_libraries(adifedit PRIVATE adifeditui)
target_compile_options(adifedit PRIVATE ${ADIFEDIT_C_FLAGS})

# Windows: a GUI program (no console window alongside it), with the app
# icon embedded for Explorer and the taskbar. See scripts/build-windows.sh.
if(WIN32)
    enable_language(RC)
    target_sources(adifedit PRIVATE assets/adifedit.rc)
    set_target_properties(adifedit PROPERTIES WIN32_EXECUTABLE ON)
endif()

# scripts/screenshot_helper.c drives the real window for the README
# screenshots (scripts/screenshots.sh): the app minus its main(), linked
# against adifeditui. Not built by a normal build, not installed.
#   cmake --build build --target screenshot_helper
add_executable(screenshot_helper EXCLUDE_FROM_ALL scripts/screenshot_helper.c)
target_link_libraries(screenshot_helper PRIVATE adifeditui)

# The parser fuzz target, tests/fuzz_parser.c - clang + libFuzzer only;
# use the `fuzz` preset (CONTRIBUTING.md "Fuzzing").
option(ADIFEDIT_FUZZ "Build the libFuzzer target fuzz_parser (clang)" OFF)

# Unit tests (ctest). Headless - they link only adifcore, no GTK - apart
# from the one view test, which runs in its own headless cage session.
enable_testing()
add_subdirectory(tests)

install(TARGETS adifedit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES COPYING DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/adifedit)

# A starter batch stamp; users add their own to ~/.local/share/adifedit/stamps/.
install(FILES assets/stamps/example.bst DESTINATION ${CMAKE_INSTALL_DATADIR}/adifedit/stamps)

# The freedesktop integration - desktop entry, MIME type, hicolor icons and
# their database refreshes - is Linux-only; Windows takes the icon from
# the .exe (assets/adifedit.rc).
if(NOT WIN32)
    # Desktop entry + the ADIF MIME type it handles.
    install(FILES io.github.nheathlabs.adifedit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES io.github.nheathlabs.adifedit.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
    # AppStream metadata, for software centres.
    install(FILES io.github.nheathlabs.adifedit.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

    # Application icon, in the hicolor icon theme.
    foreach(_sz 16 24 32 48 64 128 256 512)
        install(FILES assets/icons/hicolor/${_sz}x${_sz}/apps/io.github.nheathlabs.adifedit.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps)
    endforeach()
    install(FILES assets/icons/hicolor/scalable/apps/io.github.nheathlabs.adifedit.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

    # Package docs: a copyright file and a gzipped changelog, which Debian
    # policy (and lintian) expect under share/doc/<package>.
    find_program(GZIP_PROGRAM gzip)
    if(GZIP_PROGRAM)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/changelog.gz
            COMMAND ${GZIP_PROGRAM} -9nc ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md
                    > ${CMAKE_CURRENT_BINARY_DIR}/changelog.gz
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md
            VERBATIM)
        add_custom_target(changelog_gz ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/changelog.gz)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/changelog.gz
                DESTINATION ${CMAKE_INSTALL_DOCDIR})
    endif()
    install(FILES cmake/copyright DESTINATION ${CMAKE_INSTALL_DOCDIR})

    # Post-install database refreshes (best effort; skipped when staging into
    # a package root via DESTDIR, where the packager runs them instead).
    # \${CMAKE_INSTALL_PREFIX} is resolved at install time, so this honours a
    # `cmake --install --prefix ...` override; "${CMAKE_INSTALL_DATADIR}" is
    # the (configure-time) "share".
    find_program(GTK4_UPDATE_ICON_CACHE gtk4-update-icon-cache gtk-update-icon-cache)
    find_program(UPDATE_MIME_DATABASE update-mime-database)
    find_program(UPDATE_DESKTOP_DATABASE update-desktop-database)
    install(CODE "
        if(NOT DEFINED ENV{DESTDIR})
            set(_datadir \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}\")
            if(EXISTS \"${GTK4_UPDATE_ICON_CACHE}\" AND IS_DIRECTORY \"\${_datadir}/icons/hicolor\")
                execute_process(COMMAND \"${GTK4_UPDATE_ICON_CACHE}\" -q -t -f
                                \"\${_datadir}/icons/hicolor\" ERROR_QUIET)
            endif()
            if(EXISTS \"${UPDATE_MIME_DATABASE}\" AND IS_DIRECTORY \"\${_datadir}/mime\")
                execute_process(COMMAND \"${UPDATE_MIME_DATABASE}\" \"\${_datadir}/mime\" ERROR_QUIET)
            endif()
            if(EXISTS \"${UPDATE_DESKTOP_DATABASE}\" AND IS_DIRECTORY \"\${_datadir}/applications\")
                execute_process(COMMAND \"${UPDATE_DESKTOP_DATABASE}\" \"\${_datadir}/applications\" ERROR_QUIET)
            endif()
        endif()
    ")
endif()

# `cpack -G DEB` (from a configured build dir, after building) makes
# adifedit_VERSION_ARCH.deb; library dependencies come from dpkg-shlibdeps.
if(NOT WIN32)
    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGE_NAME adifedit)
    set(CPACK_PACKAGE_CONTACT "Nicholas Heath <nicheath@gmail.com>")
    set(CPACK_PACKAGE_HOMEPAGE_URL https://github.com/nheathlabs/adifedit)
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "View and edit ham radio ADIF logs")
    set(CPACK_PACKAGE_DESCRIPTION
        "adifedit is a GTK 4 viewer and editor for ADIF (.adi / .adif) logs:\nan editable grid with undo/redo, find and replace, and batch stamping of\nfields into every record.")
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
    set(CPACK_STRIP_FILES ON)
    set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(CPACK_DEBIAN_PACKAGE_SECTION hamradio)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    include(CPack)
endif()

# `cmake --build build --target uninstall` reverses `cmake --install`.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    COMMENT "Uninstalling files listed in install_manifest.txt")
