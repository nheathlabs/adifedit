#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rasterise assets/adifedit.svg into the PNG sizes used by the hicolor
# icon theme and the bundled GResource. Run it after editing the SVG.
#
# Renderer, tried in order: rsvg-convert, inkscape, resvg, or a GdkPixbuf
# fallback via python3 - every machine that can build adifedit already has
# one of these (GTK4 pulls in librsvg + gdk-pixbuf).

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
svg="$root/assets/adifedit.svg"
hicolor="$root/assets/icons/hicolor"
sizes=(16 24 32 48 64 128 256 512)

[[ -f "$svg" ]] || { echo "error: $svg not found" >&2; exit 1; }

# Pick a renderer. rsvg-convert (from librsvg2-bin, installed by
# scripts/install-build-deps.sh) is preferred - it is the same engine GTK
# uses for SVGs at runtime. The GdkPixbuf fallback needs nothing beyond
# what GTK4 already pulls in, so the script still works on a machine that
# only installed the runtime deps.
#   render <svg> <w> <h> <out.png>
py_render='
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
src, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
GdkPixbuf.Pixbuf.new_from_file_at_size(src, w, h).savev(out, "png", [], [])
'
if command -v rsvg-convert >/dev/null 2>&1; then
    renderer="rsvg-convert"
    render() { rsvg-convert -w "$2" -h "$3" -o "$4" -- "$1"; }
elif python3 -c 'import gi; gi.require_version("GdkPixbuf","2.0"); from gi.repository import GdkPixbuf' >/dev/null 2>&1; then
    renderer="python3 + GdkPixbuf"
    render() { python3 -c "$py_render" "$1" "$2" "$3" "$4"; }
else
    echo "error: no SVG rasteriser found; install librsvg2-bin (or python3-gi)" >&2
    exit 1
fi
echo "renderer: $renderer"

for s in "${sizes[@]}"; do
    apps="$hicolor/${s}x${s}/apps"
    mkdir -p "$apps"
    render "$svg" "$s" "$s" "$apps/io.github.nheathlabs.adifedit.png"
    cp -- "$apps/io.github.nheathlabs.adifedit.png" "$root/assets/adifedit-${s}.png"
    printf '  %sx%s\n' "$s" "$s"
done

mkdir -p "$hicolor/scalable/apps"
cp -- "$svg" "$hicolor/scalable/apps/io.github.nheathlabs.adifedit.svg"

# Optional lossless recompression.
pngs=("$hicolor"/*x*/apps/io.github.nheathlabs.adifedit.png "$root"/assets/adifedit-*.png)
if command -v oxipng >/dev/null 2>&1; then
    oxipng -q -o max --strip safe -- "${pngs[@]}"
elif command -v optipng >/dev/null 2>&1; then
    optipng -quiet -o5 -- "${pngs[@]}"
fi

echo "done: assets/icons/hicolor, assets/adifedit-<N>.png"
