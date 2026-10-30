#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Greps src/ and tests/ for patterns that would break a Windows / MinGW
# build - the ones that clang-tidy's restrict-system-includes and
# src/portability_guard.h can't catch (things declared in <stdlib.h>,
# hardcoded paths, GDK backend APIs, LLP64 format specifiers).
#
# A genuine, guarded exception gets a  /* portable-ok: <reason> */
# comment on the offending line. See CONTRIBUTING.md "Portability".

set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

status=0

scan() { # <description> <grep-ERE>
    local desc=$1 re=$2 hits
    # portability_guard.h is the enforcement mechanism, not enforced code:
    # it deliberately pre-includes the POSIX headers it then poisons.
    hits=$(grep -rnE "$re" src tests --include='*.c' --include='*.h' \
               --exclude='portability_guard.h' \
           | grep -v 'portable-ok' || true)
    if [ -n "$hits" ]; then
        printf '\n%s\n%s\n' "$desc" "$hits"
        status=1
    fi
}

scan "hardcoded absolute paths -> g_get_tmp_dir / g_get_home_dir / g_get_user_*_dir, g_build_filename" \
    '"(/tmp|/home|/usr|/etc|/var|/opt|/root|/mnt|/media)([/"]|$)|"~/'

scan "process / env / time calls with no portable form -> g_spawn_*, g_getenv/g_setenv, g_get_monotonic_time, g_usleep" \
    '\b(system|popen|pclose|realpath|getenv|setenv|unsetenv|putenv|gettimeofday|nanosleep)[[:space:]]*\('

scan "raw pthreads -> GThread / GTask / GMutex / GCond" \
    '\bpthread_[a-z_]+[[:space:]]*\(|#[[:space:]]*include[[:space:]]*<pthread'

scan "GDK backend-specific API -> keep the UI code backend-agnostic" \
    '#[[:space:]]*include[[:space:]]*<gdk/(x11|wayland|win32|macos)|\bgdk_(x11|wayland|win32|macos)_|\bGDK_IS_(X11|WAYLAND|WIN32|MACOS)|\bGDK_WINDOWING_'

# `long` is 32-bit on 64-bit Windows (LLP64). `%lld` / `%llu` are fine.
scan "LLP64 format specifier -> %zu for gsize, G_GINT64_FORMAT / PRId64 for 64-bit" \
    '%[-+ #0-9.*]*l[diouxX]'

if [ "$status" -ne 0 ]; then
    printf '\nNon-portable patterns found. Fix them, or - if a hit is genuinely safe\n'
    printf '(guarded by #ifdef G_OS_*, etc.) - add  /* portable-ok: reason */  to the line.\n'
    exit 1
fi

echo "portability: clean"
