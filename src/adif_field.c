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

#include "adif_field.h"

AdifFieldNameStatus adif_field_name_check(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return ADIF_FIELD_NAME_EMPTY;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (g_ascii_isspace(*p)) {
            return ADIF_FIELD_NAME_HAS_SPACE;
        }
    }
    if (!g_ascii_isalpha(name[0]) && name[0] != '_') {
        return ADIF_FIELD_NAME_BAD_START;
    }
    if (g_ascii_strcasecmp(name, "EOR") == 0 || g_ascii_strcasecmp(name, "EOH") == 0) {
        return ADIF_FIELD_NAME_RESERVED;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_') {
            return ADIF_FIELD_NAME_INVALID_CHAR;
        }
    }
    return ADIF_FIELD_NAME_OK;
}

gboolean adif_field_name_is_valid(const char *name) {
    return adif_field_name_check(name) == ADIF_FIELD_NAME_OK;
}
