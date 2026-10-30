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
 * A "batch stamp": a plain-text `.bst` file whose first content line is a
 * name and whose remaining lines are ADIF field names. Applying it prompts
 * for a value per field and writes those values into every (non-blank)
 * record.
 *
 * Blank lines and lines starting with '#' are ignored. Field names are
 * upper-cased and de-duplicated. A line that isn't a valid ADIF field
 * name goes to `bad` rather than `fields`, so the stamp still appears but
 * the user is told to fix it before it can run.
 */
typedef struct {
    char *id;          /* file name without the ".bst" suffix */
    char *path;        /* absolute path to the file */
    char *title;       /* first content line - the label shown in the menu */
    GPtrArray *fields; /* char*, valid ADIF field names, upper-cased, in order */
    GPtrArray *bad;    /* char*, one "line N: ..." message per unusable line */
} AdifStamp;

#define ADIF_STAMP_ERROR (adif_stamp_error_quark())
GQuark adif_stamp_error_quark(void);

typedef enum {
    ADIF_STAMP_ERROR_NO_TITLE, /* nothing on the first content line */
    ADIF_STAMP_ERROR_NO_FIELDS, /* a title but no field lines at all */
} AdifStampError;

/* Load one `.bst` file. NULL + `error` on an unreadable / structurally
 * invalid file (bad *field names* are not an error - see `bad`). */
AdifStamp *adif_stamp_load(const char *path, GError **error);
void adif_stamp_free(AdifStamp *stamp);

/* Every `.bst` under <system data dir>/adifedit/stamps and
 * <user data dir>/adifedit/stamps, the user directory winning on `id`
 * collision, sorted by title (case-insensitive). */
GPtrArray *adif_stamp_list(void);
void adif_stamp_list_free(GPtrArray *stamps);

/* The user's writable stamps directory, created if it doesn't exist.
 * Caller frees. */
char *adif_stamp_user_dir(void);

G_END_DECLS
