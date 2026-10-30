/*
 * adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
 * Copyright (C) 2026 Nicholas Heath
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Force-included into every translation unit (see ADIFEDIT_C_FLAGS in
 * CMakeLists.txt). Turns a set of non-portable POSIX calls into compile
 * errors so the codebase stays buildable for Windows (MinGW) without a
 * porting pass later - use the GLib equivalents (see CONTRIBUTING.md
 * "Portability").
 *
 * #pragma GCC poison fires on a token appearing *anywhere* after it,
 * including in system headers included later - and <glib.h> / <gio/gio.h>
 * transitively drag in <unistd.h>, <pthread.h>, <pwd.h> on Linux. So we
 * pull those headers in *first*, tripping their include guards, and only
 * then poison: later transitive re-includes are no-ops, and the poison
 * bites only fresh uses in our own code.
 *
 * clang-tidy's restrict-system-includes and scripts/check-portability.sh
 * cover the headers and patterns this can't.
 */
#pragma once

/* Pre-include the headers that declare the poisoned names (see above).
 * These are exactly the headers clang-tidy's restrict-system-includes
 * bans elsewhere - that's the point, so exempt just this file. On MinGW
 * most don't exist (and nothing there declares the poisoned names), so
 * skip them and just poison. */
#ifndef _WIN32
/* NOLINTBEGIN(portability-restrict-system-includes) */
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/mman.h>
#include <unistd.h>
/* NOLINTEND(portability-restrict-system-includes) */
#endif

#pragma GCC poison \
    fork vfork \
    execl execlp execle execv execvp execvpe \
    pthread_create pthread_join pthread_detach \
    pthread_mutex_lock pthread_mutex_unlock \
    pthread_cond_wait pthread_cond_signal pthread_cond_broadcast \
    mmap munmap \
    getpwuid getpwnam getgrgid getgrnam
