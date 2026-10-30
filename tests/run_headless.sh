#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run a view test in its own headless wlroots session, so it never opens
# a window on the desktop and runs the same with no display at all.
#
#   tests/run_headless.sh TEST_BINARY
#
# Exits 77 (ctest "skipped") without cage, else with the test's status.

set -euo pipefail

test_bin=${1:?usage: run_headless.sh TEST_BINARY}
command -v cage >/dev/null || { echo "skipped: needs 'cage' (apt install cage)"; exit 77; }

# cage doesn't pass its client's exit status on; collect it through a file.
status_file=$(mktemp)
trap 'rm -f "$status_file"' EXIT
export TEST_BIN=$test_bin STATUS_FILE=$status_file

# shellcheck disable=SC2016
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    cage -- sh -c '"$TEST_BIN"; echo $? >"$STATUS_FILE"' 2>/dev/null

status=$(cat "$status_file")
exit "${status:-1}"
