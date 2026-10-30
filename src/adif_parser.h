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
 * Minimal ADIF (Amateur Data Interchange Format) reader.
 *
 * ADIF files are made of tags of the form <FIELDNAME:LENGTH[:TYPE]>VALUE
 * where VALUE is exactly LENGTH bytes (which may itself contain '<' or
 * '>' characters - that's why we advance by byte length rather than
 * scanning for a closing tag). Records are separated by a bare <EOR>
 * tag. An optional free-form header, terminated by <EOH>, may precede
 * the first record; its tags never become spreadsheet columns, but they
 * are kept in AdifParseResult.header so callers can read PROGRAMID etc.
 *
 * The file must be ASCII or UTF-8 (they are read verbatim as bytes, and
 * LENGTH is taken as a byte count). Any other encoding is rejected.
 * A file whose lengths don't line up with the data is still parsed, on a
 * best-effort basis, with AdifParseResult.warning set: past the header,
 * each field whose length doesn't land on the next tag is fitted to the
 * data that's there - the length read as a UTF-8 character count if that
 * lands, else up to the next tag - and flagged (adif_record_is_repaired).
 */

/* The ADIF specification revision this build reads and writes, and the
 * canonical single-page spec for it (Help menu). Keep the two in step. */
#define ADIF_SPEC_VERSION "3.1.7"
#define ADIF_SPEC_URL "https://www.adif.org/317/ADIF_317.htm"

#define ADIF_PARSE_ERROR (adif_parse_error_quark())
GQuark adif_parse_error_quark(void);

typedef enum {
    ADIF_PARSE_ERROR_ENCODING, /* not ASCII / UTF-8 */
} AdifParseError;

typedef struct _AdifRecord AdifRecord;

/*
 * The outcome of parsing one file:
 *   records    - a GPtrArray of AdifRecord* (one per QSO).
 *   fieldnames - a GPtrArray of char* field names (upper-cased), ordered
 *                by first appearance in the file. This is the
 *                authoritative column list.
 *   fieldtypes - a GArray of char, one per `fieldnames` entry: the
 *                column's type indicator (the upper-cased 'N' of
 *                <APP_X:2:N>), the first one any record gave it, or 0.
 *   warning    - NULL normally; an owned, user-facing sentence when the
 *                parse had to recover from malformed field lengths (naming
 *                the first few repaired fields).
 *   header     - the fields of the file's <EOH>-terminated header (look
 *                them up by upper-cased name with adif_record_get_field),
 *                or NULL if the file had no header.
 * `records` / `fieldnames` / `fieldtypes` / `header` own their contents; free the whole
 * thing with adif_parse_result_free().
 */
typedef struct {
    GPtrArray *records;
    GPtrArray *fieldnames;
    GArray *fieldtypes;
    char *warning;
    AdifRecord *header;
} AdifParseResult;

void adif_parse_result_free(AdifParseResult *result);

/*
 * Parses the ADIF file at `path`. Returns a newly-allocated
 * AdifParseResult on success, or NULL with `error` set (a read error, or
 * ADIF_PARSE_ERROR_ENCODING for a non-ASCII/UTF-8 file).
 */
AdifParseResult *adif_parse_file(const char *path, GError **error);

/*
 * Parses `length` bytes of ADIF text at `contents` (need not be
 * NUL-terminated; not retained). Same result and errors as
 * adif_parse_file(), minus the read error.
 */
AdifParseResult *adif_parse_data(const char *contents, gsize length, GError **error);

/* Looks up a field's value by upper-cased name. Returns NULL if the
 * record has no such field. */
const char *adif_record_get_field(const AdifRecord *record, const char *field_name_upper);

/* A QSO record's value for column `column` - an index into the result's
 * `fieldnames` - or NULL if the record has no such field. The fast path
 * for walking a record in column order. Always NULL for the header. */
const char *adif_record_get_value(const AdifRecord *record, guint column);

/* The header's fields in file order (a repeated one counted once, where
 * it first appeared): how many, each one's upper-cased name (look its
 * value up with adif_record_get_field), and its type indicator - the
 * upper-cased 'N' of <USERDEF1:3:N>, or 0 if the tag gave none. Zero
 * fields for a NULL header. */
guint adif_record_header_n_fields(const AdifRecord *header);
const char *adif_record_header_name_at(const AdifRecord *header, guint i);
char adif_record_header_type_at(const AdifRecord *header, guint i);

/* TRUE if the parser fitted this QSO record's field in column `column`
 * to the data because its declared length didn't line up. Always FALSE
 * for the header. */
gboolean adif_record_is_repaired(const AdifRecord *record, guint column);

G_END_DECLS
