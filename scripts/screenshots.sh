#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate the README screenshots in docs/images/ from assets/sample.adi.
# Each capture runs in its own headless wlroots session (cage), so the PNG
# contains only adifedit - nothing else on the desktop.
#
#   scripts/screenshots.sh                       # uses ./build
#   BUILD_DIR=build/debug scripts/screenshots.sh # any configured tree
#
# Needs: cage, grim  (see install-build-deps.sh)

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

build_dir=${BUILD_DIR:-build}
out_dir=docs/images
sample=assets/sample.adi
stamp=assets/stamps/example.bst

for tool in cage grim cmake; do
    command -v "$tool" >/dev/null || { echo "missing '$tool'" >&2; exit 1; }
done
[[ -f "$build_dir/CMakeCache.txt" ]] || {
    echo "no build tree at '$build_dir' - run 'cmake -S . -B build' or set BUILD_DIR" >&2
    exit 1
}

# screenshot_helper is an EXCLUDE_FROM_ALL target (the app minus its
# main(), linked against the adifeditui object library) - build it on demand.
echo "building screenshot_helper"
cmake --build "$build_dir" --target screenshot_helper >/dev/null

helper="$build_dir/screenshot_helper"
[[ -x "$helper" ]] || helper=$(find "$build_dir" -type f -name screenshot_helper -perm -u+x | head -n1)
[[ -x "$helper" ]] || { echo "screenshot_helper not found under $build_dir" >&2; exit 1; }

mkdir -p "$out_dir"

# shot NAME MODE TRIGGER_MS GRAB_DELAY
shot() {
    local name=$1 mode=$2 trigger=$3 grab=$4
    local png="$out_dir/$name.png"
    echo "capturing $png"
    # shellcheck disable=SC2016  # cage runs the body via sh -c; expand there
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
        env HELPER="$helper" MODE="$mode" SAMPLE="$sample" STAMP="$stamp" \
            TRIGGER="$trigger" GRAB="$grab" PNG="$png" GTK_THEME=Adwaita:light \
        cage -- sh -c '
            "$HELPER" "$MODE" "$SAMPLE" "$STAMP" "$TRIGGER" &
            app=$!
            sleep "$GRAB"
            grim "$PNG"
            kill "$app" 2>/dev/null
            wait "$app" 2>/dev/null || true
        ' >/dev/null 2>&1
    [[ -s "$png" ]] || { echo "  FAILED - no output" >&2; exit 1; }
    command -v oxipng >/dev/null && oxipng -o2 -q "$png" || true
}

#     name             mode      trigger  grab
shot  main-window      main      1500     4
shot  file-menu        filemenu  3000     5
shot  column-menu      colmenu   3000     5
shot  batch-stamp      stamp     3000     6
shot  find-replace     replace   3000     5

echo "done -> $out_dir/"
