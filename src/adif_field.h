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

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * Why a candidate ADIF field name is (or isn't) usable. A valid name is
 * letters, digits and underscores, starting with a letter or underscore,
 * and not the reserved record/header markers (EOR / EOH). Says nothing
 * about whether the field is already a column somewhere - that's the
 * caller's business (see e.g. AdifColumnNameStatus).
 */
typedef enum {
    ADIF_FIELD_NAME_OK,
    ADIF_FIELD_NAME_EMPTY,        /* nothing entered */
    ADIF_FIELD_NAME_HAS_SPACE,    /* contains whitespace */
    ADIF_FIELD_NAME_BAD_START,    /* doesn't start with a letter or underscore */
    ADIF_FIELD_NAME_RESERVED,     /* EOR or EOH */
    ADIF_FIELD_NAME_INVALID_CHAR, /* some other character outside [A-Za-z0-9_] */
} AdifFieldNameStatus;

/* Classify `name` against the rules above, most-specific reason first
 * (e.g. a name that both starts wrong and contains a space is reported
 * as HAS_SPACE - the friendlier message). */
AdifFieldNameStatus adif_field_name_check(const char *name);

/* TRUE iff adif_field_name_check(name) == ADIF_FIELD_NAME_OK. */
gboolean adif_field_name_is_valid(const char *name);

G_END_DECLS
