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

#include "build_info.h"

/* Regenerated on every build by cmake/gen_build_info.cmake; lives in the
 * build directory. Isolating it here keeps the rebuild to this one small
 * unit rather than everything that wants the version string. */
#include "adifedit_build_info.h"

const char *adifedit_git_hash(void) {
    return ADIFEDIT_GIT_HASH;
}

const char *adifedit_build_time(void) {
    return ADIFEDIT_BUILD_TIME;
}
