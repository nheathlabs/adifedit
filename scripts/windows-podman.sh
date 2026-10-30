#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the Windows zip from this (Linux) host: runs scripts/build-windows.sh
# in the Fedora MinGW container of ci/Containerfile, with rootless podman.
# The image is built on first use and cached after that.
#
#   scripts/windows-podman.sh                  ->  build/windows/adifedit-VERSION-windows-x86_64.zip
#   scripts/windows-podman.sh --with-sources   ... plus the libraries' source RPMs
#                                                  (see scripts/build-windows.sh)
#
# Needs: podman  (apt install podman)

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
image=adifedit-mingw

command -v podman >/dev/null || { echo "missing 'podman' (apt install podman)" >&2; exit 1; }

podman build -t "$image" -f "$root/ci/Containerfile" "$root/ci"

# keep-id: run as the calling user, so the build tree and zip are owned
# by them and git accepts the mounted repository (for the build's git hash).
podman run --rm --userns=keep-id -v "$root":/src:Z -w /src "$image" scripts/build-windows.sh "$@"
