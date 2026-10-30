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

#include "adif_parser.h"

#include <stdlib.h>
#include <string.h>

/* clang-format off */  /* the hyphenated quark name is one token, not a subtraction */
G_DEFINE_QUARK(adif-parse-error, adif_parse_error)
/* clang-format on */

/*
 * A header record keeps its fields in a small hash table, plus their
 * names and type indicators in file order so a save can write them back
 * as they were. A QSO record
 * keeps just its values, one slot per column in `fieldnames` order, and
 * shares one name -> column index with every other record of the parse -
 * so a big log doesn't carry a hash table and a copy of every field name
 * per record, and turning a record into a row is a plain index walk.
 */
struct _AdifRecord {
    GHashTable *fields;  /* header: owned upper name -> owned value; NULL for a QSO record */
    GPtrArray *order;    /* header: owned upper names, in order of first appearance */
    GArray *types;       /* header: char per `order` entry - its type indicator
                            (upper-cased, e.g. 'N'), or 0 for none */
    GPtrArray *values;   /* QSO record: owned value (NULL = absent) per column */
    GHashTable *columns; /* QSO record: (ref) the parse's upper name -> column + 1 */
    GArray *repaired;    /* QSO record: guint columns whose length was fitted to
                            the data; NULL (the usual case) when there are none */
};

static void adif_record_free(AdifRecord *record) {
    if (record == NULL) {
        return;
    }
    g_clear_pointer(&record->fields, g_hash_table_unref);
    g_clear_pointer(&record->order, g_ptr_array_unref);
    g_clear_pointer(&record->types, g_array_unref);
    g_clear_pointer(&record->values, g_ptr_array_unref);
    g_clear_pointer(&record->columns, g_hash_table_unref);
    g_clear_pointer(&record->repaired, g_array_unref);
    g_free(record);
}

const char *adif_record_get_field(const AdifRecord *record, const char *field_name_upper) {
    if (record == NULL || field_name_upper == NULL) {
        return NULL;
    }
    if (record->fields != NULL) {
        return g_hash_table_lookup(record->fields, field_name_upper);
    }
    guint column = GPOINTER_TO_UINT(g_hash_table_lookup(record->columns, field_name_upper));
    return column > 0 ? adif_record_get_value(record, column - 1) : NULL;
}

const char *adif_record_get_value(const AdifRecord *record, guint column) {
    if (record == NULL || record->values == NULL || column >= record->values->len) {
        return NULL;
    }
    return g_ptr_array_index(record->values, column);
}

guint adif_record_header_n_fields(const AdifRecord *header) {
    return header != NULL && header->order != NULL ? header->order->len : 0;
}

const char *adif_record_header_name_at(const AdifRecord *header, guint i) {
    g_return_val_if_fail(i < adif_record_header_n_fields(header), NULL);
    return g_ptr_array_index(header->order, i);
}

char adif_record_header_type_at(const AdifRecord *header, guint i) {
    g_return_val_if_fail(i < adif_record_header_n_fields(header), 0);
    return g_array_index(header->types, char, i);
}

gboolean adif_record_is_repaired(const AdifRecord *record, guint column) {
    if (record == NULL || record->repaired == NULL) {
        return FALSE;
    }
    for (guint i = 0; i < record->repaired->len; i++) {
        if (g_array_index(record->repaired, guint, i) == column) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Flag (or unflag) `column` as repaired - a repeated field's later value
 * wins, and so does its flag. */
static void record_set_repaired(AdifRecord *record, guint column, gboolean repaired) {
    if (adif_record_is_repaired(record, column) == repaired) {
        return;
    }
    if (repaired) {
        if (record->repaired == NULL) {
            record->repaired = g_array_new(FALSE, FALSE, sizeof(guint));
        }
        g_array_append_val(record->repaired, column);
        return;
    }
    if (record->repaired == NULL) {
        return;
    }
    for (guint i = 0; i < record->repaired->len; i++) {
        if (g_array_index(record->repaired, guint, i) == column) {
            g_array_remove_index_fast(record->repaired, i);
            return;
        }
    }
}

void adif_parse_result_free(AdifParseResult *result) {
    if (result == NULL) {
        return;
    }
    g_clear_pointer(&result->records, g_ptr_array_unref);
    g_clear_pointer(&result->fieldnames, g_ptr_array_unref);
    g_clear_pointer(&result->fieldtypes, g_array_unref);
    g_clear_pointer(&result->warning, g_free);
    g_clear_pointer(&result->header, adif_record_free);
    g_free(result);
}

/* The fields of the record being read, in file order, until <EOR> or
 * <EOH> says what they are. Both arrays own their strings. */
typedef struct {
    GPtrArray *names;  /* upper-cased */
    GPtrArray *values;
    GArray *repaired;  /* gboolean per field: its length was fitted to the data */
    GArray *types;     /* char per field: its type indicator (upper-cased), or 0 */
} PendingFields;

static void pending_clear(PendingFields *pending) {
    g_ptr_array_set_size(pending->names, 0);
    g_ptr_array_set_size(pending->values, 0);
    g_array_set_size(pending->repaired, 0);
    g_array_set_size(pending->types, 0);
}

/* <EOH>: the pending fields are the header. A repeated field keeps the
 * later value (and type), in the place where it first appeared. */
static AdifRecord *pending_to_header(PendingFields *pending) {
    AdifRecord *header = g_new0(AdifRecord, 1);
    header->fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    header->order = g_ptr_array_new_with_free_func(g_free);
    header->types = g_array_new(FALSE, FALSE, sizeof(char));
    for (guint i = 0; i < pending->names->len; i++) {
        const char *name = g_ptr_array_index(pending->names, i);
        char type = g_array_index(pending->types, char, i);
        guint at = 0;
        if (g_ptr_array_find_with_equal_func(header->order, name, g_str_equal, &at)) {
            g_array_index(header->types, char, at) = type;
        } else {
            g_ptr_array_add(header->order, g_strdup(name));
            g_array_append_val(header->types, type);
        }
        /* Move both strings into the table; the NULLs left behind are
         * freed harmlessly by pending_clear(). */
        g_hash_table_replace(header->fields, g_ptr_array_index(pending->names, i),
                             g_ptr_array_index(pending->values, i));
        g_ptr_array_index(pending->names, i) = NULL;
        g_ptr_array_index(pending->values, i) = NULL;
    }
    pending_clear(pending);
    return header;
}

/* <EOR> (or a trailing record with no <EOR>): the pending fields are a
 * QSO. A name not seen in any earlier QSO becomes the next column, so
 * `fieldnames` stays in order of first appearance; a repeated field in
 * one record keeps the later value. A column takes the first type
 * indicator any record gives it. */
static AdifRecord *pending_to_record(PendingFields *pending, AdifParseResult *result,
                                     GHashTable *columns) {
    GPtrArray *fieldnames = result->fieldnames;
    AdifRecord *record = g_new0(AdifRecord, 1);
    record->columns = g_hash_table_ref(columns);
    record->values = g_ptr_array_new_full(fieldnames->len, g_free);
    for (guint i = 0; i < pending->names->len; i++) {
        const char *name = g_ptr_array_index(pending->names, i);
        guint column = GPOINTER_TO_UINT(g_hash_table_lookup(columns, name));
        if (column == 0) {
            g_ptr_array_add(fieldnames, g_strdup(name));
            char none = '\0';
            g_array_append_val(result->fieldtypes, none);
            column = fieldnames->len;
            g_hash_table_insert(columns, g_strdup(name), GUINT_TO_POINTER(column));
        }
        char *column_type = &g_array_index(result->fieldtypes, char, column - 1);
        if (*column_type == '\0') {
            *column_type = g_array_index(pending->types, char, i);
        }
        if (record->values->len < column) {
            g_ptr_array_set_size(record->values, (gint)column); /* new slots are NULL */
        }
        g_free(g_ptr_array_index(record->values, column - 1));
        g_ptr_array_index(record->values, column - 1) = g_ptr_array_index(pending->values, i);
        g_ptr_array_index(pending->values, i) = NULL; /* moved into the record */
        record_set_repaired(record, column - 1, g_array_index(pending->repaired, gboolean, i));
    }
    pending_clear(pending);
    return record;
}

/* One <NAME:LENGTH[:TYPE]> tag, pointing into the file buffer - nothing
 * copied. */
typedef struct {
    const char *name;   /* not NUL-terminated; see name_len */
    gsize name_len;
    const char *length; /* just past the first ':', or NULL if there is none */
} AdifTag;

/* Read the tag whose text runs from `start` up to (not including) the '>'
 * at `gt`. */
static AdifTag read_tag(const char *start, const char *gt) {
    const char *colon = memchr(start, ':', (size_t)(gt - start));
    AdifTag tag = {
        .name = start,
        .name_len = (gsize)((colon != NULL ? colon : gt) - start),
        .length = colon != NULL ? colon + 1 : NULL,
    };
    return tag;
}

/* Is the tag's name `word` (upper case), ignoring case? */
static gboolean tag_is(const AdifTag *tag, const char *word) {
    return tag->name_len == strlen(word)
           && g_ascii_strncasecmp(tag->name, word, tag->name_len) == 0;
}

/* Any non-blank byte in [p, end)? */
static gboolean has_text(const char *p, const char *end) {
    for (; p < end; p++) {
        if (!g_ascii_isspace(*p)) {
            return TRUE;
        }
    }
    return FALSE;
}

/* One parse's state between tags. */
typedef struct {
    AdifParseResult *result; /* being filled: records, fieldnames, header */
    GHashTable *columns;     /* upper name -> its index in result->fieldnames + 1;
                                shared (by ref) with every QSO record for
                                adif_record_get_field() */
    PendingFields pending;   /* the current record's fields. Only once <EOR> or
                                <EOH> arrives do we know whether they're a QSO
                                (their names become columns) or the header (they
                                never do). */
    gboolean past_header;    /* once the header (if any) is behind us, non-blank
                                bytes falling *between* tags mean a field length
                                didn't match its data - char counts vs byte
                                counts, or a damaged file ... */
    gboolean suspect;        /* ... and this gets set */
    guint repaired;          /* fields whose length was fitted to the data */
    GPtrArray *repair_examples; /* the first few, as "record N FIELD" */
} Parse;

/* How many repaired fields the warning names before "...". */
#define REPAIR_EXAMPLES 5

/* Is [name, name + len) a field name the ADIF spec allows? Printable
 * ASCII, none of , : < > { }, and no space at either end. Lenient about
 * everything else (case, '-', '.', inner spaces), as the spec is. */
static gboolean tag_name_ok(const char *name, gsize len) {
    if (len == 0 || name[0] == ' ' || name[len - 1] == ' ') {
        return FALSE;
    }
    for (gsize i = 0; i < len; i++) {
        if (name[i] < ' ' || name[i] > '~' || strchr(",:<>{}", name[i]) != NULL) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Does a real tag start at `lt` (a '<')? <NAME:LENGTH[:TYPE]> with a
 * non-empty, printable NAME, or a bare <EOR> / <EOH>. Stricter than the
 * tokeniser on purpose: a stray '<' inside a value ("a <b> c") must not
 * look like one when deciding where a mis-sized value really ends. Any
 * name counts, even one tag_name_ok() refuses: it is still where the
 * value before it ends. */
static gboolean tag_at(const char *lt, const char *end) {
    const char *q = lt + 1;
    const char *name = q;
    while (q < end && *q >= ' ' && *q <= '~' && *q != ':' && *q != '>' && *q != '<') {
        q++;
    }
    gsize name_len = (gsize)(q - name);
    if (q >= end) {
        return FALSE;
    }
    if (*q == '>') {
        return name_len == 3
               && (g_ascii_strncasecmp(name, "EOR", 3) == 0
                   || g_ascii_strncasecmp(name, "EOH", 3) == 0);
    }
    if (*q != ':' || name_len == 0) {
        return FALSE;
    }
    const char *digits = ++q;
    while (q < end && g_ascii_isdigit(*q)) {
        q++;
    }
    if (q == digits || q >= end) {
        return FALSE;
    }
    if (*q == ':' && q + 2 < end && g_ascii_isalpha(q[1])) {
        q += 2; /* the type indicator */
    }
    return q < end && *q == '>';
}

/* Does a value ending at `q` leave only whitespace before the next real
 * tag (or the end of the file)? */
static gboolean lands_cleanly(const char *q, const char *end) {
    while (q < end && g_ascii_isspace(*q)) {
        q++;
    }
    return q >= end || (*q == '<' && tag_at(q, end));
}

/* The first real tag in [from, to), or `to` if there is none. */
static const char *next_tag(const char *from, const char *to, const char *end) {
    for (const char *lt = from; lt < to; lt++) {
        lt = memchr(lt, '<', (size_t)(to - lt));
        if (lt == NULL) {
            break;
        }
        if (tag_at(lt, end)) {
            return lt;
        }
    }
    return to;
}

/* The length of a value starting at `p` whose declared `declared_len`
 * (already clamped to the file) doesn't land on the next tag. Try the
 * length as a UTF-8 character count - the usual cause - then fall back
 * to the data that's actually there: cut at a tag the declared span runs
 * into, or extend up to the next tag. Trailing whitespace the fallback
 * picks up is the gap before that tag, not data. */
static size_t fit_value(const char *p, const char *end, long declared, size_t declared_len) {
    const char *q = p;
    for (long i = 0; i < declared && q < end; i++) {
        q = g_utf8_next_char(q);
    }
    q = MIN(q, end);
    if (lands_cleanly(q, end)) {
        return (size_t)(q - p);
    }

    const char *span_end = p + declared_len;
    const char *floor = p;
    const char *stop = next_tag(p, span_end, end);
    if (stop == span_end) { /* too short: extend */
        floor = span_end;
        stop = next_tag(span_end, end, end);
    }
    while (stop > floor && g_ascii_isspace(stop[-1])) {
        stop--;
    }
    return (size_t)(stop - p);
}

/* Read `tag`'s value, which starts at `p`, into the pending record, and
 * return where the next tag search resumes. A length that runs past the
 * end of the file is cut short there and marks the parse suspect. Past
 * the header, a value whose length doesn't land on the next tag is
 * fitted to the data (fit_value) and flagged as repaired. (strtol stops
 * at the tag's closing '>', so the length needn't be copied out first.) */
static const char *take_field(Parse *parse, const AdifTag *tag, const char *p, const char *end) {
    char *after = NULL;
    long declared = MAX(strtol(tag->length, &after, 10), 0);
    /* "<NAME:LEN:T>": the type indicator, kept for the header. `after` is
     * inside the tag (at the ':' or its '>'), so after[1] is too. */
    char type = '\0';
    if (*after == ':' && g_ascii_isalpha(after[1])) {
        type = g_ascii_toupper(after[1]);
    }
    size_t field_len = MIN((size_t)declared, (size_t)(end - p));
    if (field_len < (size_t)declared) {
        parse->suspect = TRUE;
    }

    char *name = g_ascii_strup(tag->name, (gssize)tag->name_len);
    gboolean repaired = parse->past_header && !lands_cleanly(p + field_len, end);
    if (repaired) {
        field_len = fit_value(p, end, declared, field_len);
        parse->suspect = TRUE;
        parse->repaired++;
        if (parse->repair_examples->len < REPAIR_EXAMPLES) {
            g_ptr_array_add(parse->repair_examples,
                            g_strdup_printf("record %u %s", parse->result->records->len + 1, name));
        }
    }
    /* A byte length that ends inside a multi-byte character (possible
     * before the header, where nothing is fitted) would leave the value -
     * and a saved copy - invalid UTF-8. The file as a whole is valid, so
     * only that last character is cut: drop it. */
    const char *valid_end = NULL;
    if (!g_utf8_validate_len(p, field_len, &valid_end)) {
        field_len = (size_t)(valid_end - p);
        parse->suspect = TRUE;
    }
    g_ptr_array_add(parse->pending.names, name);
    g_ptr_array_add(parse->pending.values, g_strndup(p, field_len));
    g_array_append_val(parse->pending.repaired, repaired);
    g_array_append_val(parse->pending.types, type);
    return p + field_len;
}

/* A field whose name the spec doesn't allow (tag_name_ok): adifedit
 * couldn't write it back as valid ADIF, so it is left out and the file
 * marked suspect, which keeps the original from being saved over. Its
 * value is skipped if its length lands on the next tag; otherwise the
 * value is left to read as stray text. */
static const char *skip_bad_field(Parse *parse, const AdifTag *tag, const char *p,
                                  const char *end) {
    parse->suspect = TRUE;
    long declared = MAX(strtol(tag->length, NULL, 10), 0);
    size_t skip = MIN((size_t)declared, (size_t)(end - p));
    return lands_cleanly(p + skip, end) ? p + skip : p;
}

/* Act on one tag; `p` is just past its '>'. Returns where the next tag
 * search resumes. */
static const char *handle_tag(Parse *parse, const AdifTag *tag, const char *p, const char *end) {
    AdifParseResult *result = parse->result;
    if (tag_is(tag, "EOR")) {
        g_ptr_array_add(result->records,
                        pending_to_record(&parse->pending, result, parse->columns));
    } else if (tag_is(tag, "EOH")) {
        /* Header-only field names never become columns, but the fields
         * themselves are kept so callers can read PROGRAMID etc. A second
         * <EOH> (malformed) replaces the first. */
        g_clear_pointer(&result->header, adif_record_free);
        result->header = pending_to_header(&parse->pending);
        parse->past_header = TRUE;
    } else if (tag->length != NULL && tag_name_ok(tag->name, tag->name_len)) {
        return take_field(parse, tag, p, end);
    } else if (tag->length != NULL) {
        return skip_bad_field(parse, tag, p, end);
    }
    /* else: no length given (or an empty tag) - no value can be read
     * reliably, so skip it */
    return p;
}

/* Tokenise the whole file into `parse`. */
static void parse_contents(Parse *parse, const char *contents, gsize length) {
    const char *p = contents;
    const char *end = contents + length;
    parse->past_header = (length > 0 && contents[0] == '<'); /* no header */

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (lt == NULL) {
            break;
        }
        if (parse->past_header && !parse->suspect) {
            parse->suspect = has_text(p, lt);
        }
        const char *gt = memchr(lt, '>', (size_t)(end - lt));
        if (gt == NULL) {
            break; /* malformed trailing tag: keep what we have so far */
        }
        const char *inner = memchr(lt + 1, '<', (size_t)(gt - lt - 1));
        if (inner != NULL) {
            /* "<stray <CALL:4>": a tag can't hold a '<', so the first one
             * was text - which is only expected in the header. */
            parse->suspect = parse->suspect || parse->past_header;
            p = inner;
            continue;
        }
        AdifTag tag = read_tag(lt + 1, gt);
        p = handle_tag(parse, &tag, gt + 1, end);
    }

    if (parse->pending.names->len > 0) {
        /* Trailing record with no terminating <EOR>: still keep it. */
        g_ptr_array_add(parse->result->records,
                        pending_to_record(&parse->pending, parse->result, parse->columns));
    }
}

AdifParseResult *adif_parse_data(const char *contents, gsize length, GError **error) {
    g_return_val_if_fail(contents != NULL || length == 0, NULL);

    if (length > 0 && !g_utf8_validate_len(contents, length, NULL)) {
        g_set_error_literal(
            error, ADIF_PARSE_ERROR, ADIF_PARSE_ERROR_ENCODING,
            "This file is not plain ASCII/UTF-8 text, so it can't be read as ADIF. "
            "If it is an older log, re-export it from your logging software as UTF-8.");
        return NULL;
    }

    AdifParseResult *result = g_new0(AdifParseResult, 1);
    result->records = g_ptr_array_new_with_free_func((GDestroyNotify)adif_record_free);
    result->fieldnames = g_ptr_array_new_with_free_func(g_free);
    result->fieldtypes = g_array_new(FALSE, FALSE, sizeof(char));
    Parse parse = {
        .result = result,
        .columns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        .pending = {
            .names = g_ptr_array_new_with_free_func(g_free),
            .values = g_ptr_array_new_with_free_func(g_free),
            .repaired = g_array_new(FALSE, FALSE, sizeof(gboolean)),
            .types = g_array_new(FALSE, FALSE, sizeof(char)),
        },
        .repair_examples = g_ptr_array_new_with_free_func(g_free),
    };

    parse_contents(&parse, contents, length);

    g_ptr_array_unref(parse.pending.names);
    g_ptr_array_unref(parse.pending.values);
    g_array_unref(parse.pending.repaired);
    g_array_unref(parse.pending.types);
    g_hash_table_unref(parse.columns); /* the records keep their refs */

    if (parse.repaired > 0) {
        g_ptr_array_add(parse.repair_examples, NULL);
        char *examples = g_strjoinv(", ", (char **)parse.repair_examples->pdata);
        result->warning = g_strdup_printf(
            "%u field length%s in this file didn't match the data - adifedit fitted %s to "
            "the data that's there (%s%s) and highlighted %s in yellow. The file may use "
            "character counts instead of byte counts, or be damaged - review the records "
            "before saving.",
            parse.repaired, parse.repaired == 1 ? "" : "s", parse.repaired == 1 ? "it" : "them",
            examples, parse.repaired > REPAIR_EXAMPLES ? ", ..." : "",
            parse.repaired == 1 ? "that cell" : "those cells");
        g_free(examples);
    } else if (parse.suspect) {
        result->warning = g_strdup(
            "Some field lengths in this file did not line up with the data. It may use "
            "character counts instead of byte counts, or be damaged - adifedit loaded it "
            "on a best-effort basis, so review the records before saving.");
    }
    g_ptr_array_unref(parse.repair_examples);
    return result;
}

AdifParseResult *adif_parse_file(const char *path, GError **error) {
    g_return_val_if_fail(path != NULL, NULL);

    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, error)) {
        return NULL;
    }
    AdifParseResult *result = adif_parse_data(contents, length, error);
    g_free(contents);
    return result;
}
