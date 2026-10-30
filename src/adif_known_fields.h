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
 * Every standard ADIF 3.1.7 per-QSO-record field name (CALL, BAND,
 * GRIDSQUARE, ...), alphabetically sorted in plain ASCII/strcmp order,
 * NULL-terminated, no duplicates. Sourced from
 * https://www.adif.org/317/ADIF_317.htm (the same spec this build
 * targets - see ADIF_SPEC_VERSION / ADIF_SPEC_URL in adif_parser.h).
 *
 * Header-only fields (ADIF_VER, CREATED_TIMESTAMP, PROGRAMID,
 * PROGRAMVERSION, USERDEFn) are deliberately excluded - this list is for
 * suggesting a *column* (a per-record field), not a header field.
 *
 * Advisory only, not a whitelist: adif_document_check_new_column_name()
 * is still the sole gate on what can actually be inserted, so a
 * non-standard name (a custom field the spec doesn't define) remains
 * fully valid input whether or not it appears here.
 */
extern const char *const ADIF_KNOWN_FIELDS[];

G_END_DECLS
