#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs everything needed to build adifedit, plus the developer
# tooling. Detects the package manager (apt / dnf / pacman) and installs
# the matching set:
#
#   * a C11 compiler (any current gcc / clang) and CMake >= 3.25
#   * pkg-config and the GTK 4 development files (>= 4.12)
#   * glib-compile-resources  (bundles the app icon into the binary)
#   * rsvg-convert            (scripts/render-icons.sh)
#   * clang-format, clang-tidy, shellcheck  (the pre-push checks; see
#     CONTRIBUTING.md)
#   * cage, grim              (scripts/shot.sh and scripts/screenshots.sh -
#                              headless screenshots of the running app)
#   * podman                  (scripts/windows-podman.sh - the Windows zip,
#                              cross-built in a rootless container)

set -euo pipefail

if command -v apt-get >/dev/null 2>&1; then
    mgr=apt
elif command -v dnf >/dev/null 2>&1; then
    mgr=dnf
elif command -v pacman >/dev/null 2>&1; then
    mgr=pacman
else
    cat >&2 <<'EOF'
No supported package manager (apt / dnf / pacman) found.

Install these by hand, then build with CMake:
  - a C11 compiler (any current gcc or clang)
  - cmake >= 3.25, pkg-config / pkgconf
  - GTK 4 development files (>= 4.12)
  - glib-compile-resources
  - rsvg-convert (from librsvg)
  - clang-format, clang-tidy, shellcheck (optional; for the pre-push checks)
  - clang (optional; for the fuzz preset)
  - appstreamcli, desktop-file-validate (optional; the metadata ctests)
  - cage, grim (optional; for scripts/shot.sh and scripts/screenshots.sh)
  - podman (optional; for scripts/windows-podman.sh, the Windows build)
EOF
    exit 1
fi

echo "==> Package manager: ${mgr}"

case "${mgr}" in
apt)
    sudo apt-get update
    sudo apt-get install -y \
        build-essential gcc cmake pkg-config \
        libgtk-4-dev libglib2.0-dev-bin librsvg2-bin \
        clang clang-format clang-tidy shellcheck \
        appstream desktop-file-utils \
        cage grim podman
    ;;
dnf)
    sudo dnf install -y \
        gcc cmake pkgconf-pkg-config \
        gtk4-devel glib2-devel librsvg2-tools \
        clang clang-tools-extra ShellCheck \
        appstream desktop-file-utils \
        cage grim podman
    ;;
pacman)
    # Refresh the sync db separately so `-S` doesn't do a partial upgrade.
    sudo pacman -Sy
    sudo pacman -S --needed \
        base-devel cmake pkgconf \
        gtk4 glib2 librsvg \
        clang shellcheck \
        appstream desktop-file-utils \
        cage grim podman
    ;;
esac

# C11 (2011) is old enough that any current gcc/clang has it; if a truly
# ancient toolchain slips through, CMAKE_C_STANDARD_REQUIRED fails the
# configure step with a clear message. Just report what we found.
echo "==> C compiler"
cc=""
for c in "${CC:-}" gcc cc clang; do
    if [ -n "${c}" ] && command -v "${c}" >/dev/null 2>&1; then
        cc="${c}"
        break
    fi
done
if [ -n "${cc}" ]; then
    echo "    ${cc}: $("${cc}" --version 2>/dev/null | head -1)"
else
    echo "    WARNING: none found on PATH (looked for \$CC, gcc, cc, clang)"
fi

cat <<'EOF'
==> Done. Build with:
      cmake --preset debug     &&  cmake --build --preset debug     # ./build/debug/adifedit
      cmake --preset release   &&  cmake --build --preset release   # ./build/release/adifedit
    or without presets:
      cmake -S . -B build      &&  cmake --build build              # ./build/adifedit (Debug)
    Install / uninstall system-wide (from a build dir):
      sudo cmake --install build/release --strip
      sudo cmake --build build/release --target uninstall
    Windows zip (cross-built in a container):
      scripts/windows-podman.sh                                     # ./build/windows/*.zip
EOF
