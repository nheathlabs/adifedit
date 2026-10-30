#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Screenshot the app running in its own headless wlroots session, so the
# capture contains only adifedit - nothing else on the desktop.
#
#   scripts/shot.sh OUT.png [FILE.adi] [SECONDS]
#
# Needs: cage, grim  (apt install cage grim)

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

export SHOT_OUT=${1:?usage: shot.sh OUT.png [FILE.adi] [SECONDS]}
export SHOT_FILE=${2:-}
export SHOT_WAIT=${3:-4}
export SHOT_BIN="$root/build/adifedit"

[[ -x "$SHOT_BIN" ]] || { echo "build first:  cmake --build build" >&2; exit 1; }
command -v cage >/dev/null || { echo "missing 'cage' (apt install cage)" >&2; exit 1; }
command -v grim >/dev/null || { echo "missing 'grim' (apt install grim)" >&2; exit 1; }

# The inner script is single-quoted on purpose: cage runs it via `sh -c`,
# and it reads the SHOT_* values from the exported environment.
# shellcheck disable=SC2016
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    cage -- sh -c '
        "$SHOT_BIN" ${SHOT_FILE:+"$SHOT_FILE"} &
        app=$!
        sleep "$SHOT_WAIT"
        grim "$SHOT_OUT"
        kill "$app" 2>/dev/null
    ' >/dev/null 2>&1

echo "wrote $SHOT_OUT"
