#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-build adifedit for 64-bit Windows and bundle it, with the GTK
# runtime it needs, into a portable zip:
#
#   build/windows/package/adifedit/                     the bundle (bin/adifedit.exe ...)
#   build/windows/adifedit-VERSION-windows-x86_64.zip   the same, zipped
#
# The bundle carries what its licences ask for: THIRD-PARTY-NOTICES.txt
# (every bundled package, its licence and its source), each package's
# licence texts in share/licenses/, and adifedit's own source in source/.
#
#   --with-sources   also download the bundled packages' source RPMs into
#                    build/windows/adifedit-VERSION-windows-x86_64-sources/
#                    (a few hundred MB). Publish that directory next to the
#                    zip: the LGPL wants the libraries' source available
#                    from wherever the binaries are.
#
# Everything lands in the "windows" preset's build tree, beside the Linux
# ones, so one `build/` holds every build's output.
#
# Runs inside the Fedora MinGW environment of ci/Containerfile - use
# scripts/windows-podman.sh from the host; CI runs this script directly.

set -euo pipefail

with_sources=0
for arg in "$@"; do
    case $arg in
        --with-sources) with_sources=1 ;;
        *) echo "usage: build-windows.sh [--with-sources]" >&2; exit 2 ;;
    esac
done

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

sysroot=/usr/x86_64-w64-mingw32/sys-root/mingw
objdump=x86_64-w64-mingw32-objdump
build=build/windows # the "windows" preset's binaryDir (CMakePresets.json)
package=$build/package
bundle=$package/adifedit

[[ -f /usr/share/mingw/toolchain-mingw64.cmake ]] || {
    echo "run this in the MinGW container: scripts/windows-podman.sh" >&2
    exit 1
}

# --- Build and install into the bundle ------------------------------

cmake --preset windows
cmake --build --preset windows
rm -rf "$package"
cmake --install "$build" --prefix "$bundle"
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")

# --- DLLs: everything adifedit.exe needs, transitively --------------

# The DLLs a PE file imports by name. System ones (kernel32, ...) aren't
# in the sysroot, which is how bundle_dlls() tells them apart.
imports() {
    "$objdump" -p "$1" | sed -n 's/^[[:space:]]*DLL Name: //p'
}

# Copy into bin/ each sysroot DLL the given files import, then the ones
# those import, until nothing new turns up.
bundle_dlls() {
    local queue=("$@") file dll
    while ((${#queue[@]} > 0)); do
        file=${queue[0]}
        queue=("${queue[@]:1}")
        while read -r dll; do
            if [[ -f "$sysroot/bin/$dll" && ! -f "$bundle/bin/$dll" ]]; then
                cp "$sysroot/bin/$dll" "$bundle/bin/"
                queue+=("$bundle/bin/$dll")
            fi
        done < <(imports "$file")
    done
}

# --- Runtime data GTK loads by path ---------------------------------

# GSettings schemas: GTK's file chooser aborts without its own.
schemas=share/glib-2.0/schemas
mkdir -p "$bundle/$schemas"
cp "$sysroot/$schemas"/org.gtk.gtk4.Settings.*.gschema.xml "$bundle/$schemas/"
glib-compile-schemas "$bundle/$schemas"

# gdk-pixbuf loaders, and a cache listing only the ones shipped: Fedora's
# cache names an SVG loader its packages don't install. Entries are
# blank-line-separated, their paths relative to the bundle root; the
# file has CRLF line ends, which would hide the blank lines from awk.
pixbuf=lib/gdk-pixbuf-2.0/2.10.0
mkdir -p "$bundle/$pixbuf"
cp -r "$sysroot/$pixbuf/loaders" "$bundle/$pixbuf/"
tr -d '\r' <"$sysroot/$pixbuf/loaders.cache" | awk -v root="$bundle/" 'BEGIN { RS = ""; ORS = "\n\n" }
    /^#/ { print; next }
    { path = $1; gsub(/"/, "", path); gsub(/\\\\/, "/", path)
      if ((getline line < (root path)) >= 0) { close(root path); print } }' \
    >"$bundle/$pixbuf/loaders.cache"

# Icon theme: just hicolor, the fallback every theme lookup ends in. No
# Adwaita - the few stock icons the UI uses (go-up/-down, pan-down) are
# built into GTK's own resources, and adifedit's are in its GResource.
mkdir -p "$bundle/share/icons"
cp -r "$sysroot/share/icons/hicolor" "$bundle/share/icons/"

bundle_dlls "$bundle/bin/adifedit.exe" "$bundle/$pixbuf/loaders"/*.dll

# --- Licences, notices and sources -----------------------------------

# Which Fedora package each bundled file came from: every bundled file
# that also exists in the sysroot is one of theirs, and rpm knows its
# owner. What isn't there is ours or generated from theirs.
declare -A pkg_files=()
while read -r rel; do
    [[ -f "$sysroot/$rel" ]] || continue
    pkg=$(rpm -qf --qf '%{NAME}' "$sysroot/$rel")
    pkg_files[$pkg]+="${pkg_files[$pkg]:+ }$rel"
done < <(cd "$bundle" && find . -type f -printf '%P\n' | sort)
mapfile -t packages < <(printf '%s\n' "${!pkg_files[@]}" | sort)

# Copy a package's licence texts into share/licenses/<package>/. Most are
# in /usr/share/licenses; a few packages keep them with their docs, and
# zlib only in the comment heading zlib.h. Nothing found stops the build,
# rather than ship a library without its licence.
copy_licence() {
    local pkg=$1 dest=$bundle/share/licenses/$1 src
    mkdir -p "$dest"
    for src in /usr/share/licenses/"$pkg"/*; do
        [[ -f $src ]] && cp "$src" "$dest/"
    done
    if [[ -z $(ls -A "$dest") ]]; then
        for src in /usr/share/doc/"$pkg"/*; do
            case ${src##*/} in
                COPYING* | LICENSE* | LICENCE* | readme.txt) cp "$src" "$dest/" ;;
            esac
        done
    fi
    if [[ $pkg == mingw64-zlib ]]; then
        sed -n '1,/\*\//p' "$sysroot/include/zlib.h" >"$dest/LICENSE-from-zlib.h.txt"
    fi
    if [[ -z $(ls -A "$dest") ]]; then
        echo "no licence text found for $pkg - add where it is to copy_licence()" >&2
        exit 1
    fi
}

# adifedit's own source, as committed: the GPL has it travel with the
# program. A dirty tree still builds, but says so - its source isn't HEAD.
commit=$(git rev-parse HEAD)
dirty=""
if ! git diff --quiet HEAD; then
    dirty=" (built with uncommitted changes, which the source below lacks)"
    echo "warning: uncommitted changes - source/ holds HEAD, not what was built" >&2
fi
mkdir -p "$bundle/source"
git archive --format=tar.gz --prefix="adifedit-$version/" \
    -o "$bundle/source/adifedit-$version-source.tar.gz" HEAD

declare -A srpm_urls=() # source RPM file name -> its Koji URL
{
    printf 'adifedit %s for Windows (x86_64) - licences and third-party notices\n\n' "$version"
    printf 'adifedit\n'
    printf '  Version:  %s, git commit %s%s\n' "$version" "$commit" "$dirty"
    printf '  Licence:  GPL-3.0-or-later - share/doc/adifedit/COPYING\n'
    printf '  Source:   source/adifedit-%s-source.tar.gz\n\n' "$version"
    printf 'It bundles these libraries and data files, unmodified, from the MinGW\n'
    printf 'packages of %s. Each package'"'"'s licence texts are in\n' "$(cat /etc/fedora-release)"
    printf 'share/licenses/<package>/.\n'
    for pkg in "${packages[@]}"; do
        copy_licence "$pkg"
        IFS='|' read -r evr licence srpm < <(rpm -q --qf '%{VERSION}-%{RELEASE}|%{LICENSE}|%{SOURCERPM}\n' "$pkg")
        # Koji keeps every build: packages/<source name>/<version>/<release>/src/.
        srcname=${srpm%-"$evr".src.rpm}
        url=https://kojipkgs.fedoraproject.org/packages/$srcname/${evr%-*}/${evr##*-}/src/$srpm
        srpm_urls[$srpm]=$url
        printf '\n%s %s\n' "$pkg" "$evr"
        printf '  Licence:  %s\n' "$licence"
        printf '  Source:   %s\n' "$url"
        printf '  Files:    %s\n' "${pkg_files[$pkg]}"
    done
    printf '\nSource code for the libraries\n\n'
    printf '  Each library'"'"'s complete source is the source RPM at the URL in its\n'
    printf '  Source line, on Fedora'"'"'s build system (Koji).\n'
    if ((with_sources)); then
        printf '\n  The same source RPMs are also published as a separate download next\n'
        printf '  to this zip (they are not inside it): the folder\n'
        printf '  adifedit-%s-windows-x86_64-sources/.\n' "$version"
    fi
    printf '\nCredits\n\n'
    printf '  Portions of this software are copyright (C) %s The FreeType Project\n' "$(date +%Y)"
    printf '  (www.freetype.org). All rights reserved.\n\n'
    printf '  This software is based in part on the work of the Independent JPEG Group.\n'
} >"$bundle/THIRD-PARTY-NOTICES.txt"

# --- Check and zip ---------------------------------------------------

# Every import of every shipped binary must be a shipped DLL or a system
# one - a DLL the sysroot has but the bundle lacks would fail on Windows.
missing=0
while read -r file; do
    while read -r dll; do
        if [[ -f "$sysroot/bin/$dll" && ! -f "$bundle/bin/$dll" ]]; then
            echo "missing: $dll (needed by ${file#"$bundle"/})" >&2
            missing=1
        fi
    done < <(imports "$file")
done < <(find "$bundle" -name '*.exe' -o -name '*.dll')
((missing == 0)) || exit 1

zipfile=$build/adifedit-$version-windows-x86_64.zip
rm -f "$zipfile"
(cd "$package" && zip -qr "$root/$zipfile" adifedit)
echo "wrote $zipfile"

# --- The libraries' source RPMs (--with-sources) ---------------------

# Straight from Koji, so the version is exactly the one bundled. Already
# downloaded ones are kept (a source RPM never changes); ones no longer
# bundled are removed.
if ((with_sources)); then
    sources=$build/adifedit-$version-windows-x86_64-sources
    mkdir -p "$sources"
    for srpm in "${!srpm_urls[@]}"; do
        if [[ ! -f "$sources/$srpm" ]]; then
            echo "downloading $srpm"
            curl -fsSL --retry 3 -o "$sources/$srpm.part" "${srpm_urls[$srpm]}"
            mv "$sources/$srpm.part" "$sources/$srpm"
        fi
    done
    for file in "$sources"/*; do
        name=${file##*/}
        [[ $name == SHA256SUMS || -n ${srpm_urls[$name]:-} ]] || rm -f "$file"
    done
    (cd "$sources" && sha256sum -- *.src.rpm >SHA256SUMS)
    echo "wrote $sources/"
fi
