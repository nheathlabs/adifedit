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

/* Headless unit tests for AdifDocument - no GTK, no display. */

#include <string.h>

#include <glib/gstdio.h>

#include "adif_document.h"
#include "adif_field.h"
#include "adif_parser.h"
#include "adif_row.h"

/* --- helpers ------------------------------------------------------- */

/* A document with the given NULL-terminated column names and no rows. */
static AdifDocument *doc_with_columns(const char *const *names) {
    AdifDocument *doc = adif_document_new();
    for (guint i = 0; names[i] != NULL; i++) {
        adif_document_insert_column(doc, i, names[i]);
    }
    adif_document_set_modified(doc, FALSE);
    return doc;
}

static guint di_at(AdifDocument *doc, guint position) {
    return adif_document_column_data_index_at(doc, position);
}

/* Write `len` bytes to a fresh temp .adi file; returns its path (caller
 * g_remove()s and g_free()s it). */
static char *write_tmp_adi(const void *data, gsize len) {
    char *path = NULL;
    gint fd = g_file_open_tmp("adifedit-XXXXXX.adi", &path, NULL);
    g_assert_cmpint(fd, !=, -1);
    g_close(fd, NULL);
    g_assert_true(g_file_set_contents(path, data, (gssize)len, NULL));
    return path;
}

/* --- tests ------------------------------------------------------- */

static void test_empty(void) {
    AdifDocument *doc = adif_document_new();

    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 0);
    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 0);
    g_assert_false(adif_document_get_modified(doc));

    char *adif = adif_document_to_adif(doc, NULL, NULL);
    g_assert_nonnull(strstr(adif, "<EOH>"));
    g_assert_null(strstr(adif, "<EOR>"));
    g_free(adif);

    g_object_unref(doc);
}

static void test_build_and_read(void) {
    const char *const cols[] = { "CALL", "BAND", NULL };
    AdifDocument *doc = doc_with_columns(cols);

    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 2);
    g_assert_cmpstr(adif_document_column_name_at(doc, 0), ==, "CALL");
    g_assert_cmpstr(adif_document_column_name_at(doc, 1), ==, "BAND");

    guint call = di_at(doc, 0);
    guint band = di_at(doc, 1);
    g_assert_cmpstr(adif_document_column_name(doc, call), ==, "CALL");
    g_assert_cmpuint(adif_document_column_position(doc, band), ==, 1);

    adif_document_insert_row(doc, 0);
    adif_document_insert_row(doc, 1);
    adif_document_set_value(doc, 0, call, "KJ5LMR");
    adif_document_set_value(doc, 0, band, "20m");
    adif_document_set_value(doc, 1, call, "W1AW");

    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 2);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "KJ5LMR");
    g_assert_cmpstr(adif_document_get_value(doc, 1, call), ==, "W1AW");
    g_assert_cmpstr(adif_document_get_value(doc, 1, band), ==, "");

    g_object_unref(doc);
}

static void test_column_insert_positions(void) {
    const char *const cols[] = { "CALL", "MODE", NULL };
    AdifDocument *doc = doc_with_columns(cols);

    adif_document_insert_column(doc, 0, "band");           /* front, lower-case */
    adif_document_insert_column(doc, 99, "COMMENT");       /* clamps to the end */
    adif_document_insert_column(doc, 2, "NAME");           /* middle */

    const char *order[] = { "BAND", "CALL", "NAME", "MODE", "COMMENT" };
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, G_N_ELEMENTS(order));
    for (guint i = 0; i < G_N_ELEMENTS(order); i++) {
        g_assert_cmpstr(adif_document_column_name_at(doc, i), ==, order[i]);
    }

    g_object_unref(doc);
}

static void test_delete_column_keeps_other_values(void) {
    const char *const cols[] = { "CALL", "SCRATCH", "BAND", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    guint scratch = di_at(doc, 1);
    guint band = di_at(doc, 2);

    adif_document_insert_row(doc, 0);
    adif_document_set_value(doc, 0, call, "W1AW");
    adif_document_set_value(doc, 0, scratch, "junk");
    adif_document_set_value(doc, 0, band, "40m");

    g_assert_true(adif_document_delete_column(doc, scratch));
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 2);

    /* CALL and BAND keep their values - they are addressed by stable
     * data-index, not by position. */
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "W1AW");
    g_assert_cmpstr(adif_document_get_value(doc, 0, band), ==, "40m");
    g_assert_cmpuint(adif_document_column_position(doc, band), ==, 1);

    g_object_unref(doc);
}

static void test_delete_last_column_refused(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);

    g_assert_false(adif_document_delete_column(doc, di_at(doc, 0)));
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 1);

    g_object_unref(doc);
}

static void test_fill_column(void) {
    const char *const cols[] = { "CALL", "STATION_CALLSIGN", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint sc = di_at(doc, 1);

    for (guint i = 0; i < 3; i++) {
        adif_document_insert_row(doc, i);
    }
    adif_document_fill_column(doc, sc, "N0CALL");

    for (guint i = 0; i < 3; i++) {
        g_assert_cmpstr(adif_document_get_value(doc, i, sc), ==, "N0CALL");
    }

    g_object_unref(doc);
}

/* The sliced fill (column_write with skip_blank FALSE) writes blank rows
 * too, across more than one step, and undoes as one step. */
static void test_fill_column_step_is_incremental(void) {
    const char *const cols[] = { "CALL", "NOTE", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint note = di_at(doc, 1);
    const guint n = 300; /* > the step's internal clock-check interval */
    for (guint i = 0; i < n; i++) {
        adif_document_insert_row(doc, i); /* every row blank */
    }

    AdifColumnWrite *write = adif_document_column_write_begin(doc, note, "ALL", FALSE);
    g_assert_nonnull(write);
    g_assert_true(adif_document_column_write_step(write, g_get_monotonic_time()));
    g_assert_false(
        adif_document_column_write_step(write, g_get_monotonic_time() + G_TIME_SPAN_SECOND));
    adif_document_column_write_finish(write);

    for (guint i = 0; i < n; i++) {
        g_assert_cmpstr(adif_document_get_value(doc, i, note), ==, "ALL");
    }
    g_assert_cmpstr(adif_document_undo_label(doc), ==, "Fill Column");
    adif_document_undo(doc);
    for (guint i = 0; i < n; i++) {
        g_assert_cmpstr(adif_document_get_value(doc, i, note), ==, "");
    }

    g_object_unref(doc);
}

static guint history_change_count;
static void on_history_changed(AdifDocument *doc, gpointer data) {
    (void)doc;
    (void)data;
    history_change_count++;
}

/* Replace All is one undo step, and tells the undo/redo UI so once -
 * not once per replaced cell. */
static void test_replace_all_signals_history_once(void) {
    const char *const cols[] = { "MODE", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint mode = di_at(doc, 0);
    for (guint i = 0; i < 50; i++) {
        adif_document_insert_row(doc, i);
        adif_document_set_value(doc, i, mode, "FT8");
    }

    history_change_count = 0;
    g_signal_connect(doc, "history-changed", G_CALLBACK(on_history_changed), NULL);
    AdifQuery q = { .query = "FT8", .replacement = "FT4" };
    g_assert_cmpuint(adif_document_replace_all(doc, &q), ==, 50);
    g_assert_cmpuint(history_change_count, ==, 1);

    adif_document_undo(doc);
    g_assert_cmpstr(adif_document_get_value(doc, 49, mode), ==, "FT8");

    g_object_unref(doc);
}

static void test_row_insert_delete(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);

    adif_document_insert_row(doc, 0);
    adif_document_set_value(doc, 0, call, "A");
    adif_document_insert_row(doc, 0);        /* new blank row at the front */
    adif_document_set_value(doc, 0, call, "B");
    adif_document_insert_row(doc, 99);       /* clamps to the end */
    adif_document_set_value(doc, 2, call, "C");

    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 3);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "B");
    g_assert_cmpstr(adif_document_get_value(doc, 1, call), ==, "A");
    g_assert_cmpstr(adif_document_get_value(doc, 2, call), ==, "C");

    adif_document_delete_row(doc, 1);       /* drop "A" */
    adif_document_delete_row(doc, 50);      /* out of range: ignored */
    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 2);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "B");
    g_assert_cmpstr(adif_document_get_value(doc, 1, call), ==, "C");

    g_object_unref(doc);
}

static void test_modified_flag(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    adif_document_insert_row(doc, 0);
    adif_document_set_modified(doc, FALSE);

    g_assert_false(adif_document_get_modified(doc));

    adif_document_set_value(doc, 0, call, "W1AW");
    g_assert_true(adif_document_get_modified(doc));

    adif_document_set_modified(doc, FALSE);
    adif_document_set_value(doc, 0, call, "W1AW");   /* same value: no-op */
    g_assert_false(adif_document_get_modified(doc));

    adif_document_insert_column(doc, 0, "BAND");
    g_assert_true(adif_document_get_modified(doc));

    g_object_unref(doc);
}

static void test_new_column_name_check(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);

    g_assert_cmpint(adif_document_check_new_column_name(doc, ""), ==, ADIF_COLUMN_NAME_EMPTY);
    g_assert_cmpint(adif_document_check_new_column_name(doc, "2COLD"), ==,
                    ADIF_COLUMN_NAME_INVALID);
    g_assert_cmpint(adif_document_check_new_column_name(doc, "MY NOTE"), ==,
                    ADIF_COLUMN_NAME_INVALID);
    g_assert_cmpint(adif_document_check_new_column_name(doc, "EOR"), ==, ADIF_COLUMN_NAME_INVALID);
    g_assert_cmpint(adif_document_check_new_column_name(doc, "call"), ==,
                    ADIF_COLUMN_NAME_DUPLICATE);
    g_assert_cmpint(adif_document_check_new_column_name(doc, "APP_ADIFEDIT_X"), ==,
                    ADIF_COLUMN_NAME_OK);

    g_object_unref(doc);
}

static void test_serialise_skips_empty(void) {
    const char *const cols[] = { "CALL", "COMMENT", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);

    adif_document_insert_row(doc, 0);
    adif_document_set_value(doc, 0, call, "W1AW");   /* COMMENT left blank */

    char *adif = adif_document_to_adif(doc, NULL, NULL);
    g_assert_nonnull(strstr(adif, "<CALL:4>W1AW"));
    g_assert_null(strstr(adif, "COMMENT"));
    g_assert_nonnull(strstr(adif, "<EOR>"));
    g_free(adif);

    g_object_unref(doc);
}

/* Number of non-overlapping occurrences of `needle` in `haystack`. */
static guint count_occurrences(const char *haystack, const char *needle) {
    guint n = 0;
    for (const char *p = haystack; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
        n++;
    }
    return n;
}

static void test_serialise_skips_empty_rows(void) {
    const char *const cols[] = { "CALL", "BAND", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);

    adif_document_insert_row(doc, 0);
    adif_document_set_value(doc, 0, call, "W1AW");
    adif_document_insert_row(doc, 1);   /* left entirely blank */
    adif_document_insert_row(doc, 2);
    adif_document_set_value(doc, 2, call, "K1XX");

    char *adif = adif_document_to_adif(doc, NULL, NULL);
    g_assert_cmpuint(count_occurrences(adif, "<EOR>"), ==, 2);   /* not 3 */
    g_assert_nonnull(strstr(adif, "<CALL:4>W1AW"));
    g_assert_nonnull(strstr(adif, "<CALL:4>K1XX"));
    g_free(adif);

    g_object_unref(doc);
}

static void test_parse_roundtrip(void) {
    const char *fixture = "Generated by the test\n"
                          "<PROGRAMID:4>test<EOH>\n"
                          "<CALL:6>KJ5LMR<BAND:3>20m<MODE:3>SSB<EOR>\n"
                          "<CALL:4>W1AW<BAND:3>40m<EOR>\n";

    char *path = NULL;
    gint fd = g_file_open_tmp("adifedit-XXXXXX.adi", &path, NULL);
    g_assert_cmpint(fd, !=, -1);
    g_close(fd, NULL);
    g_assert_true(g_file_set_contents(path, fixture, -1, NULL));

    GError *error = NULL;
    AdifParseResult *parsed = adif_parse_file(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(parsed);

    AdifDocument *doc = adif_document_new();
    adif_document_load_parse(doc, parsed);
    adif_parse_result_free(parsed);

    g_assert_false(adif_document_get_modified(doc));   /* a fresh load is clean */
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 3);
    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 2);
    g_assert_cmpstr(adif_document_column_name_at(doc, 0), ==, "CALL");
    g_assert_cmpstr(adif_document_get_value(doc, 0, di_at(doc, 2)), ==, "SSB");
    g_assert_cmpstr(adif_document_get_value(doc, 1, di_at(doc, 2)), ==, "");

    /* Serialise, re-parse, and confirm the values survive the round trip. */
    char *out = adif_document_to_adif(doc, NULL, NULL);
    char *out_path = NULL;
    gint out_fd = g_file_open_tmp("adifedit-XXXXXX.adi", &out_path, NULL);
    g_assert_cmpint(out_fd, !=, -1);
    g_close(out_fd, NULL);
    g_assert_true(g_file_set_contents(out_path, out, -1, NULL));

    AdifParseResult *reparsed = adif_parse_file(out_path, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(reparsed->records->len, ==, 2);
    g_assert_cmpstr(adif_record_get_field(g_ptr_array_index(reparsed->records, 0), "CALL"), ==,
                    "KJ5LMR");
    g_assert_cmpstr(adif_record_get_field(g_ptr_array_index(reparsed->records, 1), "BAND"), ==,
                    "40m");
    g_assert_null(reparsed->warning);   /* our own output is well-formed */
    adif_parse_result_free(reparsed);

    g_free(out);
    g_remove(path);
    g_remove(out_path);
    g_free(path);
    g_free(out_path);
    g_object_unref(doc);
}

/* Load a file, serialise it, load that, serialise again - returns the
 * final ADIF text. Helper for the header-origin round-trip tests. */
static char *reload_and_serialise(const char *adif_in, const char *version, const char *hash) {
    char *path = write_tmp_adi(adif_in, strlen(adif_in));
    GError *error = NULL;
    AdifParseResult *parsed = adif_parse_file(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(parsed);

    AdifDocument *doc = adif_document_new();
    adif_document_load_parse(doc, parsed);
    adif_parse_result_free(parsed);

    char *out = adif_document_to_adif(doc, version, hash);
    g_object_unref(doc);
    g_remove(path);
    g_free(path);
    return out;
}

static void test_header_records_origin(void) {
    char *out = reload_and_serialise(
        "<ADIF_VER:5>3.1.4<PROGRAMID:4>N1MM<PROGRAMVERSION:8>1.0.9340<EOH>\n"
        "<CALL:4>W1AW<EOR>\n",
        "0.0.1", "deadbee");

    g_assert_nonnull(strstr(out, "<PROGRAMID:8>adifedit\n"));
    g_assert_nonnull(strstr(out, "<PROGRAMVERSION:5>0.0.1\n"));
    g_assert_nonnull(strstr(out, "<APP_ADIFEDIT_PROGRAMHASH:7>deadbee\n"));
    g_assert_nonnull(strstr(out, "<APP_ADIFEDIT_ORIG_PROG:13>N1MM 1.0.9340\n"));

    /* Round-trip: the origin survives and is NOT rewritten to adifedit. */
    char *out2 = reload_and_serialise(out, "9.9.9", "cafef00");
    g_assert_nonnull(strstr(out2, "<APP_ADIFEDIT_ORIG_PROG:13>N1MM 1.0.9340\n"));
    g_assert_null(strstr(out2, "adifedit 9.9.9"));

    g_free(out);
    g_free(out2);
}

static void test_header_no_origin_when_absent(void) {
    /* A headerless file: no origin to record, and NULL args omit the
     * optional header lines entirely. */
    char *out = reload_and_serialise("<CALL:4>W1AW<EOR>\n", "0.0.1", NULL);

    g_assert_nonnull(strstr(out, "<PROGRAMVERSION:5>0.0.1\n"));
    g_assert_null(strstr(out, "APP_ADIFEDIT_PROGRAMHASH"));
    g_assert_null(strstr(out, "APP_ADIFEDIT_ORIG_PROG"));

    /* Re-saving adifedit's own output doesn't record adifedit as the
     * origin either. */
    char *out2 = reload_and_serialise(out, "0.0.2", NULL);
    g_assert_null(strstr(out2, "APP_ADIFEDIT_ORIG_PROG"));

    g_free(out2);
    g_free(out);
}

static void test_record_type_indicators_kept(void) {
    /* A field's type indicator survives a save - the first one the file
     * gives its column - and comes back with the column on undo. */
    static const char adi[] = "<EOH>\n<CALL:4>W1AW<APP_X_PTS:2:n>12<FREQ:6:N>14.074<EOR>\n"
                              "<CALL:5>G4XYZ<APP_X_PTS:1:S>3<EOR>\n";
    AdifParseResult *r = adif_parse_data(adi, strlen(adi), NULL);
    AdifDocument *doc = adif_document_new();
    adif_document_load_parse(doc, r);
    adif_parse_result_free(r);

    char *out = adif_document_to_adif(doc, NULL, NULL);
    g_assert_nonnull(strstr(out, "<CALL:4>W1AW<APP_X_PTS:2:N>12<FREQ:6:N>14.074<EOR>"));
    g_assert_nonnull(strstr(out, "<CALL:5>G4XYZ<APP_X_PTS:1:N>3<EOR>")); /* first type wins */
    g_free(out);

    guint di = 0;
    g_assert_true(adif_document_find_column(doc, "APP_X_PTS", &di));
    g_assert_true(adif_document_delete_column(doc, di));
    adif_document_undo(doc);
    out = adif_document_to_adif(doc, NULL, NULL);
    g_assert_nonnull(strstr(out, "<APP_X_PTS:2:N>12"));
    g_free(out);

    g_object_unref(doc);
}

static void test_header_blank_origin_not_recorded(void) {
    /* A PROGRAMVERSION of just a space names no program: no empty
     * APP_ADIFEDIT_ORIG_PROG, which would vanish on the next load. */
    char *out = reload_and_serialise("<PROGRAMVERSION:1> <EOH>\n<CALL:4>W1AW<EOR>\n", "0.0.1",
                                     NULL);
    g_assert_null(strstr(out, "APP_ADIFEDIT_ORIG_PROG"));
    g_free(out);
}

static void test_header_other_fields_kept(void) {
    /* Everything else in the header comes back, in order and with its
     * type indicator - a USERDEF definition the records depend on, another
     * program's APP_ field - while adifedit's own fields aren't doubled
     * and CREATED_TIMESTAMP becomes the save time. */
    char *out = reload_and_serialise(
        "<ADIF_VER:5>3.1.4<CREATED_TIMESTAMP:15>20240101 000000<PROGRAMID:4>N1MM"
        "<USERDEF1:8:n>SHOESIZE<APP_LOTW_LASTQSORX:19>2024-03-15 13:15:00<EOH>\n"
        "<CALL:4>W1AW<SHOESIZE:2>11<EOR>\n",
        "0.0.1", NULL);

    const char *userdef = strstr(out, "<USERDEF1:8:N>SHOESIZE\n");
    const char *app = strstr(out, "<APP_LOTW_LASTQSORX:19>2024-03-15 13:15:00\n");
    const char *created = strstr(out, "<CREATED_TIMESTAMP:15>");
    g_assert_nonnull(userdef);
    g_assert_nonnull(app);
    g_assert_nonnull(created);
    g_assert_true(created < userdef && userdef < app); /* file order */
    g_assert_null(strstr(out, "20240101 000000"));    /* refreshed, not copied */
    g_assert_nonnull(strstr(out, "<SHOESIZE:2>11"));

    /* adifedit's own header fields appear once each. */
    const char *first = strstr(out, "<PROGRAMID:");
    g_assert_nonnull(first);
    g_assert_null(strstr(first + 1, "<PROGRAMID:"));
    first = strstr(out, "<ADIF_VER:");
    g_assert_null(strstr(first + 1, "<ADIF_VER:"));

    g_free(out);
}

static void test_header_unknown_hash_is_written(void) {
    char *out = reload_and_serialise("<CALL:4>W1AW<EOR>\n", "0.0.1", "unknown");
    g_assert_nonnull(strstr(out, "<APP_ADIFEDIT_PROGRAMHASH:7>unknown\n"));
    g_free(out);
}

static void test_reset_empties_the_document(void) {
    char *path = write_tmp_adi("<ADIF_VER:5>3.1.4<PROGRAMID:4>N1MM<EOH>\n"
                               "<CALL:4>W1AW<BAND:3>20m<EOR>\n",
                               -1);
    GError *error = NULL;
    AdifParseResult *parsed = adif_parse_file(path, &error);
    g_assert_no_error(error);
    AdifDocument *doc = adif_document_new();
    adif_document_load_parse(doc, parsed);
    adif_parse_result_free(parsed);

    guint call = di_at(doc, 0);
    adif_document_set_value(doc, 0, call, "K1XX"); /* now modified, has history */
    g_assert_true(adif_document_get_modified(doc));

    adif_document_reset(doc);

    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 0);
    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 0);
    g_assert_false(adif_document_get_modified(doc));
    g_assert_false(adif_document_can_undo(doc));
    g_assert_false(adif_document_can_redo(doc));

    /* And a serialise from the reset state is just an empty header. */
    char *out = adif_document_to_adif(doc, NULL, NULL);
    g_assert_nonnull(strstr(out, "<EOH>"));
    g_assert_null(strstr(out, "<EOR>"));
    g_assert_null(strstr(out, "APP_ADIFEDIT_ORIG_PROG")); /* the old origin is gone */
    g_free(out);

    g_remove(path);
    g_free(path);
    g_object_unref(doc);
}

static void test_parse_rejects_non_utf8(void) {
    /* Latin-1: the lone 0xF6 (o-umlaut) is not a valid UTF-8 sequence. */
    static const char bytes[] = "<ADIF_VER:5>3.1.4<EOH>\n<CALL:5>OH2XY<NAME:5>Bj\366rn<EOR>\n";
    char *path = write_tmp_adi(bytes, sizeof(bytes) - 1);

    GError *error = NULL;
    AdifParseResult *r = adif_parse_file(path, &error);
    g_assert_null(r);
    g_assert_error(error, ADIF_PARSE_ERROR, ADIF_PARSE_ERROR_ENCODING);
    g_clear_error(&error);

    g_remove(path);
    g_free(path);
}

static void test_parse_flags_length_mismatch(void) {
    /* <CALL:2> claims two bytes; "W1AW" would leave "AW" stranded before
     * <BAND - so the value is fitted to the data instead. */
    static const char adi[] = "<ADIF_VER:5>3.1.4<EOH>\n<CALL:2>W1AW<BAND:3>20m<EOR>\n";
    char *path = write_tmp_adi(adi, sizeof(adi) - 1);

    GError *error = NULL;
    AdifParseResult *r = adif_parse_file(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    g_assert_nonnull(r->warning);   /* loaded best-effort, flagged */
    g_assert_nonnull(strstr(r->warning, "record 1 CALL"));
    AdifRecord *rec = g_ptr_array_index(r->records, 0);
    g_assert_cmpstr(adif_record_get_field(rec, "CALL"), ==, "W1AW");
    g_assert_cmpstr(adif_record_get_field(rec, "BAND"), ==, "20m");
    g_assert_true(adif_record_is_repaired(rec, 0));   /* CALL */
    g_assert_false(adif_record_is_repaired(rec, 1));  /* BAND */
    adif_parse_result_free(r);

    g_remove(path);
    g_free(path);
}

static void test_parse_utf8_byte_counts(void) {
    /* "Metsala" with a-diaeresis x2 = "Metsälä", 9 bytes / 7 chars. */
    static const char adi[] =
        "<ADIF_VER:5>3.1.4<EOH>\n<CALL:5>OH2XY<QTH:9>Mets\303\244l\303\244<EOR>\n";
    char *path = write_tmp_adi(adi, sizeof(adi) - 1);

    GError *error = NULL;
    AdifParseResult *r = adif_parse_file(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    g_assert_null(r->warning);
    g_assert_cmpstr(adif_record_get_field(g_ptr_array_index(r->records, 0), "QTH"), ==,
                    "Mets\303\244l\303\244");
    adif_parse_result_free(r);

    g_remove(path);
    g_free(path);
}

/* --- parser edge cases ----------------------------------------- */

/* Parse NUL-terminated `text` from a throwaway temp file. */
static AdifParseResult *parse_text(const char *text, GError **error) {
    char *path = write_tmp_adi(text, strlen(text));
    AdifParseResult *r = adif_parse_file(path, error);
    g_remove(path);
    g_free(path);
    return r;
}

static const char *field0(AdifParseResult *r, guint record, const char *name) {
    return adif_record_get_field(g_ptr_array_index(r->records, record), name);
}

static void test_parse_empty_file(void) {
    AdifParseResult *r = parse_text("", NULL);
    g_assert_nonnull(r);
    g_assert_cmpuint(r->records->len, ==, 0);
    g_assert_cmpuint(r->fieldnames->len, ==, 0);
    g_assert_null(r->header);
    adif_parse_result_free(r);
}

static void test_parse_headerless(void) {
    AdifParseResult *r = parse_text("<CALL:4>W1AW<BAND:3>20m<EOR>\n", NULL);
    g_assert_cmpuint(r->records->len, ==, 1);
    g_assert_cmpuint(r->fieldnames->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(r->fieldnames, 0), ==, "CALL");
    g_assert_null(r->header); /* no <EOH> */
    adif_parse_result_free(r);
}

static void test_parse_header_only(void) {
    AdifParseResult *r = parse_text("<ADIF_VER:5>3.1.4<PROGRAMID:4>test<EOH>\n", NULL);
    g_assert_cmpuint(r->records->len, ==, 0);
    g_assert_nonnull(r->header);
    g_assert_cmpstr(adif_record_get_field(r->header, "ADIF_VER"), ==, "3.1.4");
    adif_parse_result_free(r);
}

static void test_parse_value_with_angle_brackets(void) {
    /* The value is exactly 7 bytes and contains '<' and '>'. */
    AdifParseResult *r = parse_text("<COMMENT:7>a <b> c<EOR>\n", NULL);
    g_assert_cmpstr(field0(r, 0, "COMMENT"), ==, "a <b> c");
    g_assert_null(r->warning); /* the '<' inside the value must not look like a gap */
    adif_parse_result_free(r);
}

static void test_parse_spec_field_names(void) {
    /* The spec allows any printable name without , : < > { } or an
     * outer space - including '-', '.' and inner spaces. They're read,
     * and a saved copy re-reads cleanly (the fit heuristic knows them). */
    static const char adi[] = "<EOH>\n<CALL:4>W1AW<APP_X-Y.Z:3>abc<MY FIELD:2>ok<EOR>\n";
    AdifParseResult *r = adif_parse_data(adi, strlen(adi), NULL);
    g_assert_null(r->warning);
    g_assert_cmpstr(field0(r, 0, "APP_X-Y.Z"), ==, "abc");
    g_assert_cmpstr(field0(r, 0, "MY FIELD"), ==, "ok");
    adif_parse_result_free(r);

    char *out = reload_and_serialise(adi, "0.0.1", NULL);
    r = adif_parse_data(out, strlen(out), NULL);
    g_assert_null(r->warning);
    g_assert_cmpstr(field0(r, 0, "MY FIELD"), ==, "ok");
    adif_parse_result_free(r);
    g_free(out);
}

static void test_parse_bad_field_name_skipped(void) {
    /* A name the spec forbids can't be written back as ADIF: the field is
     * left out and the file flagged, so the original is never saved over. */
    static const char adi[] = "<EOH>\n<CALL:4>W1AW<BAD,NAME:3>abc<BAND:3>20m<EOR>\n";
    AdifParseResult *r = adif_parse_data(adi, strlen(adi), NULL);
    g_assert_nonnull(r->warning);
    g_assert_cmpuint(r->fieldnames->len, ==, 2);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "W1AW");
    g_assert_cmpstr(field0(r, 0, "BAND"), ==, "20m");
    adif_parse_result_free(r);
}

static void test_parse_stray_lt_before_tag(void) {
    /* "<oops <BAND:3>": the stray '<' is text, not the start of a tag
     * named "oops <BAND". */
    static const char adi[] = "<EOH>\n<CALL:4>W1AW<oops <BAND:3>20m<EOR>\n";
    AdifParseResult *r = adif_parse_data(adi, strlen(adi), NULL);
    g_assert_nonnull(r->warning);
    g_assert_cmpstr(field0(r, 0, "BAND"), ==, "20m");
    g_assert_cmpuint(r->fieldnames->len, ==, 2);
    adif_parse_result_free(r);
}

static void test_parse_length_splits_character(void) {
    /* Before any header nothing is fitted, so a byte length ending inside
     * "\303\266" (o-umlaut) must not leave half a character in the value
     * (a saved copy would be unreadable): it stops at the last whole one. */
    static const char adi[] = "free text\n<NAME:3>Bj\303\266rn<EOR>\n";
    AdifParseResult *r = adif_parse_data(adi, strlen(adi), NULL);
    g_assert_nonnull(r->warning);
    g_assert_cmpstr(field0(r, 0, "NAME"), ==, "Bj");
    adif_parse_result_free(r);
}

static void test_parse_type_indicator_ignored(void) {
    AdifParseResult *r = parse_text("<CALL:6:S>KC8YOI<QSO_DATE:8:D>20240101<EOR>\n", NULL);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "KC8YOI");
    g_assert_cmpstr(field0(r, 0, "QSO_DATE"), ==, "20240101");
    adif_parse_result_free(r);
}

/* adif_record_get_value() walks a record by column index: a field the
 * record lacks is NULL (including columns first seen in a later record),
 * and the header - whose names never become columns - has no values. */
static void test_parse_record_values_by_column(void) {
    AdifParseResult *r = parse_text("<PROGRAMID:1>X<EOH>"
                                    "<CALL:2>W1<BAND:3>20m<EOR>"
                                    "<BAND:3>40m<MODE:2>CW<EOR>",
                                    NULL);
    g_assert_nonnull(r);
    g_assert_cmpuint(r->fieldnames->len, ==, 3); /* CALL, BAND, MODE */
    AdifRecord *first = g_ptr_array_index(r->records, 0);
    AdifRecord *second = g_ptr_array_index(r->records, 1);
    g_assert_cmpstr(adif_record_get_value(first, 0), ==, "W1");
    g_assert_cmpstr(adif_record_get_value(first, 1), ==, "20m");
    g_assert_null(adif_record_get_value(first, 2)); /* MODE only appears later */
    g_assert_null(adif_record_get_value(second, 0));
    g_assert_cmpstr(adif_record_get_value(second, 2), ==, "CW");
    g_assert_null(adif_record_get_value(second, 99));
    g_assert_null(adif_record_get_value(r->header, 0));
    g_assert_cmpstr(adif_record_get_field(r->header, "PROGRAMID"), ==, "X");
    adif_parse_result_free(r);
}

static void test_parse_duplicate_field_last_wins(void) {
    AdifParseResult *r = parse_text("<CALL:4>W1AW<CALL:4>K1XX<EOR>\n", NULL);
    g_assert_cmpuint(r->fieldnames->len, ==, 1);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "K1XX");
    adif_parse_result_free(r);
}

static void test_parse_lowercases_names(void) {
    AdifParseResult *r = parse_text("<call:4>W1AW<band:3>20m<eor>\n", NULL);
    g_assert_cmpuint(r->records->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(r->fieldnames, 0), ==, "CALL");
    g_assert_cmpstr(g_ptr_array_index(r->fieldnames, 1), ==, "BAND");
    adif_parse_result_free(r);
}

static void test_parse_trailing_record_without_eor(void) {
    AdifParseResult *r = parse_text("<CALL:4>W1AW<EOR>\n<CALL:4>K1XX\n", NULL);
    g_assert_cmpuint(r->records->len, ==, 2);
    g_assert_cmpstr(field0(r, 1, "CALL"), ==, "K1XX");
    adif_parse_result_free(r);
}

static void test_parse_unterminated_tag(void) {
    /* A '<' with no '>' after a good record: keep what parsed, don't crash. */
    AdifParseResult *r = parse_text("<CALL:4>W1AW<EOR>\n<BAND:3", NULL);
    g_assert_cmpuint(r->records->len, ==, 1);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "W1AW");
    adif_parse_result_free(r);
}

static void test_parse_length_past_eof(void) {
    AdifParseResult *r = parse_text("<CALL:12>W1AW", NULL);
    g_assert_cmpuint(r->records->len, ==, 1);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "W1AW"); /* clamped to what's there */
    g_assert_nonnull(r->warning);                      /* flagged suspect */
    adif_parse_result_free(r);
}

static void test_parse_crlf_endings(void) {
    AdifParseResult *r = parse_text("<CALL:4>W1AW<EOR>\r\n<CALL:4>K1XX<EOR>\r\n", NULL);
    g_assert_cmpuint(r->records->len, ==, 2);
    g_assert_null(r->warning); /* CR/LF between tags is whitespace, not a gap */
    adif_parse_result_free(r);
}

/* --- fitting mis-sized values to the data ---------------------- */

static void test_parse_fit_char_count(void) {
    /* "Metsälä" is 7 chars / 9 bytes: a char-counting logger wrote 7. */
    AdifParseResult *r = parse_text("<CALL:5>OH2XY<QTH:7>Mets\303\244l\303\244<MODE:2>CW<EOR>\n",
                                    NULL);
    g_assert_nonnull(r->warning);
    g_assert_cmpstr(field0(r, 0, "QTH"), ==, "Mets\303\244l\303\244");
    g_assert_cmpstr(field0(r, 0, "MODE"), ==, "CW");
    g_assert_true(adif_record_is_repaired(g_ptr_array_index(r->records, 0), 1));
    g_assert_false(adif_record_is_repaired(g_ptr_array_index(r->records, 0), 2));
    adif_parse_result_free(r);
}

static void test_parse_fit_char_count_with_angle(void) {
    /* 5 chars ("ä <b>") / 6 bytes, with a '<' that isn't a tag. */
    AdifParseResult *r = parse_text("<COMMENT:5>\303\244 <b><EOR>\n", NULL);
    g_assert_cmpstr(field0(r, 0, "COMMENT"), ==, "\303\244 <b>");
    adif_parse_result_free(r);
}

static void test_parse_fit_too_long(void) {
    AdifParseResult *r = parse_text("<CALL:6>W1AW<BAND:3>20m<EOR>\n", NULL);
    g_assert_nonnull(r->warning);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "W1AW");
    g_assert_cmpstr(field0(r, 0, "BAND"), ==, "20m");
    adif_parse_result_free(r);
}

static void test_parse_fit_too_long_over_eor(void) {
    /* <CALL:7> runs into the <EOR>. */
    AdifParseResult *r = parse_text("<CALL:7>W1AW<EOR>\n<CALL:4>K1XX<EOR>\n", NULL);
    g_assert_cmpuint(r->records->len, ==, 2);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "W1AW");
    g_assert_cmpstr(field0(r, 1, "CALL"), ==, "K1XX");
    g_assert_true(adif_record_is_repaired(g_ptr_array_index(r->records, 0), 0));
    g_assert_false(adif_record_is_repaired(g_ptr_array_index(r->records, 1), 0));
    adif_parse_result_free(r);
}

static void test_parse_fit_too_short_trims_gap(void) {
    /* The newline before <BAND is the gap between tags, not data. */
    AdifParseResult *r = parse_text("<CALL:2>W1AW\n<BAND:3>20m<EOR>\n", NULL);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "W1AW");
    adif_parse_result_free(r);
}

static void test_parse_fit_warning_lists_examples(void) {
    /* Seven bad records: the warning names five, then "...". */
    GString *text = g_string_new(NULL);
    for (int i = 0; i < 7; i++) {
        g_string_append(text, "<CALL:2>W1AW<EOR>\n");
    }
    AdifParseResult *r = parse_text(text->str, NULL);
    g_assert_nonnull(strstr(r->warning, "7 field lengths"));
    g_assert_nonnull(strstr(r->warning, "record 5 CALL, ..."));
    g_assert_null(strstr(r->warning, "record 6 CALL"));
    adif_parse_result_free(r);
    g_string_free(text, TRUE);
}

static void test_parse_fit_duplicate_last_wins(void) {
    /* A repaired CALL then a clean one: the later value - and its flag - wins. */
    AdifParseResult *r = parse_text("<CALL:2>W1AW<CALL:4>K1XX<EOR>\n", NULL);
    g_assert_cmpstr(field0(r, 0, "CALL"), ==, "K1XX");
    g_assert_false(adif_record_is_repaired(g_ptr_array_index(r->records, 0), 0));
    adif_parse_result_free(r);
}

static void test_parse_clean_has_no_repairs(void) {
    AdifParseResult *r = parse_text("<CALL:4>W1AW<BAND:3>20m<EOR>\n", NULL);
    g_assert_null(r->warning);
    g_assert_false(adif_record_is_repaired(g_ptr_array_index(r->records, 0), 0));
    g_assert_false(adif_record_is_repaired(g_ptr_array_index(r->records, 0), 1));
    g_assert_false(adif_record_is_repaired(NULL, 0));
    adif_parse_result_free(r);
}

/* The repaired flags reach the document's rows, stay on the same field
 * when an earlier column is deleted, and a save of the repaired data
 * reparses cleanly. */
static void test_document_repaired_rows(void) {
    AdifParseResult *r = parse_text("<MODE:2>CW<CALL:2>W1AW<EOR>\n<MODE:3>SSB<CALL:4>K1XX<EOR>\n",
                                    NULL);
    AdifDocument *doc = adif_document_new();
    adif_document_load_parse(doc, r);
    adif_parse_result_free(r);

    guint call = di_at(doc, 1);
    AdifRow *first = adif_document_get_row(doc, 0);
    AdifRow *second = adif_document_get_row(doc, 1);
    g_assert_true(adif_row_is_repaired(first, call));
    g_assert_false(adif_row_is_repaired(first, di_at(doc, 0)));
    g_assert_false(adif_row_is_repaired(second, call));

    g_assert_true(adif_document_delete_column(doc, di_at(doc, 0)));
    g_assert_cmpuint(di_at(doc, 0), ==, call);
    g_assert_true(adif_row_is_repaired(first, call));

    char *out = adif_document_to_adif(doc, NULL, NULL);
    AdifParseResult *again = parse_text(out, NULL);
    g_assert_null(again->warning);
    g_assert_cmpstr(field0(again, 0, "CALL"), ==, "W1AW");
    adif_parse_result_free(again);
    g_free(out);
    g_object_unref(doc);
}

static void test_parse_leading_bom(void) {
    AdifParseResult *r = parse_text("\xEF\xBB\xBF<CALL:4>W1AW<EOR>\n", NULL);
    g_assert_nonnull(r); /* a BOM is valid UTF-8, just pre-header text */
    g_assert_cmpuint(r->records->len, ==, 1);
    adif_parse_result_free(r);
}

static void test_parse_non_utf8_in_header(void) {
    GError *error = NULL;
    AdifParseResult *r = parse_text("<PROGRAMID:5>N1\xFFXM<EOH>\n<CALL:4>W1AW<EOR>\n", &error);
    g_assert_null(r);
    g_assert_error(error, ADIF_PARSE_ERROR, ADIF_PARSE_ERROR_ENCODING);
    g_clear_error(&error);
}

/* --- adif_field ------------------------------------------------- */

static void test_field_name_valid(void) {
    g_assert_true(adif_field_name_is_valid("CALL"));
    g_assert_true(adif_field_name_is_valid("MY_RIG"));
    g_assert_true(adif_field_name_is_valid("APP_ADIFEDIT_ORIG_PROG"));
    g_assert_true(adif_field_name_is_valid("_X"));
    g_assert_true(adif_field_name_is_valid("A1"));
    g_assert_true(adif_field_name_is_valid("call")); /* case is irrelevant to validity */
}

static void test_field_name_invalid(void) {
    g_assert_false(adif_field_name_is_valid(NULL));
    g_assert_false(adif_field_name_is_valid(""));
    g_assert_false(adif_field_name_is_valid("1CALL"));  /* leading digit */
    g_assert_false(adif_field_name_is_valid("MY RIG")); /* space */
    g_assert_false(adif_field_name_is_valid("MY-RIG")); /* hyphen */
    g_assert_false(adif_field_name_is_valid("CALL!"));
    g_assert_false(adif_field_name_is_valid("EOR"));
    g_assert_false(adif_field_name_is_valid("eoh"));         /* reserved, case-insensitive */
    g_assert_false(adif_field_name_is_valid("M\xC3\x9CLL")); /* non-ASCII (U-umlaut) */
}

/* --- adif_row -------------------------------------------------- */

static void test_row_grows_with_gaps(void) {
    AdifRow *row = adif_row_new();
    adif_row_add_value(row, NULL); /* stored as "" */
    adif_row_set_value(row, 3, "x");
    g_assert_cmpstr(adif_row_get_value(row, 0), ==, "");
    g_assert_cmpstr(adif_row_get_value(row, 2), ==, "");
    g_assert_cmpstr(adif_row_get_value(row, 3), ==, "x");
    g_assert_cmpstr(adif_row_get_value(row, 99), ==, ""); /* past end, never NULL */
    g_object_unref(row);
}

/* --- undo / redo ------------------------------------------------- */

static void test_undo_redo_cell(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    adif_document_insert_row(doc, 0);

    g_assert_true(adif_document_can_undo(doc));    /* the insert-row step */

    adif_document_set_value(doc, 0, call, "W1AW");
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "W1AW");
    g_assert_false(adif_document_can_redo(doc));

    adif_document_undo(doc);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "");
    g_assert_true(adif_document_can_redo(doc));
    g_assert_cmpstr(adif_document_redo_label(doc), ==, "Edit Cell");

    adif_document_redo(doc);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "W1AW");
    g_assert_false(adif_document_can_redo(doc));

    g_object_unref(doc);
}

static void test_undo_insert_column(void) {
    const char *const cols[] = { "CALL", "MODE", NULL };
    AdifDocument *doc = doc_with_columns(cols);

    guint band = adif_document_insert_column(doc, 1, "BAND");
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 3);

    adif_document_undo(doc);
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 2);
    g_assert_cmpstr(adif_document_column_name_at(doc, 1), ==, "MODE");

    adif_document_redo(doc);
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 3);
    g_assert_cmpstr(adif_document_column_name_at(doc, 1), ==, "BAND");
    g_assert_cmpuint(adif_document_column_data_index_at(doc, 1), ==, band); /* stable */

    g_object_unref(doc);
}

static void test_undo_delete_column_restores_values(void) {
    const char *const cols[] = { "CALL", "SCRATCH", "BAND", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    guint scratch = di_at(doc, 1);
    adif_document_insert_row(doc, 0);
    adif_document_set_value(doc, 0, call, "W1AW");
    adif_document_set_value(doc, 0, scratch, "junk");

    g_assert_true(adif_document_delete_column(doc, scratch));
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 2);

    adif_document_undo(doc);
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 3);
    g_assert_cmpstr(adif_document_column_name_at(doc, 1), ==, "SCRATCH");
    g_assert_cmpstr(adif_document_get_value(doc, 0, scratch), ==, "junk");
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "W1AW");

    adif_document_redo(doc);
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 2);

    g_object_unref(doc);
}

static void test_undo_row_ops(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    adif_document_insert_row(doc, 0);
    adif_document_set_value(doc, 0, call, "A");
    adif_document_insert_row(doc, 1);
    adif_document_set_value(doc, 1, call, "B");

    adif_document_delete_row(doc, 0);
    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 1);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "B");

    adif_document_undo(doc);   /* undo the delete */
    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 2);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "A");
    g_assert_cmpstr(adif_document_get_value(doc, 1, call), ==, "B");

    adif_document_redo(doc);
    g_assert_cmpuint(adif_document_get_n_rows(doc), ==, 1);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "B");

    g_object_unref(doc);
}

static void test_undo_fill_column(void) {
    const char *const cols[] = { "CALL", "NOTE", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint note = di_at(doc, 1);
    for (guint i = 0; i < 3; i++) {
        adif_document_insert_row(doc, i);
    }
    adif_document_set_value(doc, 0, note, "x");
    adif_document_set_value(doc, 1, note, "y");

    adif_document_fill_column(doc, note, "ALL");
    g_assert_cmpstr(adif_document_get_value(doc, 2, note), ==, "ALL");

    adif_document_undo(doc);
    g_assert_cmpstr(adif_document_get_value(doc, 0, note), ==, "x");
    g_assert_cmpstr(adif_document_get_value(doc, 1, note), ==, "y");
    g_assert_cmpstr(adif_document_get_value(doc, 2, note), ==, "");

    g_object_unref(doc);
}

static void test_coalesced_cell_edits_are_one_step(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    adif_document_insert_row(doc, 0);
    AdifRow *row = adif_document_get_row(doc, 0);

    adif_document_edit_cell(doc, row, call, "W");
    adif_document_edit_cell(doc, row, call, "W1");
    adif_document_edit_cell(doc, row, call, "W1AW");
    g_assert_true(adif_document_can_undo(doc));   /* live pending edit counts */
    adif_document_commit_cell_edit(doc);

    adif_document_undo(doc);   /* one step undoes the whole typed string */
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "");
    adif_document_redo(doc);
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "W1AW");

    g_object_unref(doc);
}

static void test_redo_cleared_by_new_edit(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    adif_document_insert_row(doc, 0);

    adif_document_set_value(doc, 0, call, "A");
    adif_document_set_value(doc, 0, call, "B");
    adif_document_undo(doc);                       /* -> "A", redo available */
    g_assert_true(adif_document_can_redo(doc));

    adif_document_set_value(doc, 0, call, "C");    /* new branch */
    g_assert_false(adif_document_can_redo(doc));
    g_assert_cmpstr(adif_document_get_value(doc, 0, call), ==, "C");

    g_object_unref(doc);
}

static void test_undo_to_saved_clears_modified(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    adif_document_insert_row(doc, 0);
    adif_document_set_value(doc, 0, call, "W1AW");
    adif_document_set_modified(doc, FALSE);        /* pretend we just saved */
    g_assert_false(adif_document_get_modified(doc));

    adif_document_set_value(doc, 0, call, "K1XX");
    g_assert_true(adif_document_get_modified(doc));

    adif_document_undo(doc);                       /* back to the saved state */
    g_assert_false(adif_document_get_modified(doc));

    adif_document_redo(doc);
    g_assert_true(adif_document_get_modified(doc));

    g_object_unref(doc);
}

static void test_find_column(void) {
    const char *const cols[] = { "CALL", "BAND", NULL };
    AdifDocument *doc = doc_with_columns(cols);

    guint di = 999;
    g_assert_true(adif_document_find_column(doc, "band", &di)); /* case-insensitive */
    g_assert_cmpuint(di, ==, di_at(doc, 1));
    g_assert_false(adif_document_find_column(doc, "GRIDSQUARE", NULL));

    g_object_unref(doc);
}

static void test_undo_group_is_one_step(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    for (guint i = 0; i < 3; i++) {
        adif_document_insert_row(doc, i);
    }
    adif_document_set_value(doc, 0, call, "A");
    adif_document_set_value(doc, 2, call, "C");  /* row 1 stays blank */
    adif_document_set_modified(doc, FALSE);

    adif_document_begin_group(doc, "Batch Stamp: Test");
    guint rig = adif_document_insert_column(doc, 1, "RIG");
    guint ant = adif_document_insert_column(doc, 2, "MY_ANTENNA");
    adif_document_stamp_column(doc, rig, "IC-7300");
    adif_document_stamp_column(doc, ant, "EFHW");
    adif_document_end_group(doc);

    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 3);
    g_assert_cmpstr(adif_document_get_value(doc, 0, rig), ==, "IC-7300");
    g_assert_cmpstr(adif_document_get_value(doc, 1, rig), ==, "");   /* blank row skipped */
    g_assert_cmpstr(adif_document_get_value(doc, 2, ant), ==, "EFHW");
    g_assert_cmpstr(adif_document_undo_label(doc), ==, "Batch Stamp: Test");
    g_assert_true(adif_document_get_modified(doc));

    adif_document_undo(doc);   /* one step reverts the whole stamp */
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 1);
    g_assert_false(adif_document_get_modified(doc));   /* back to the saved state */
    g_assert_true(adif_document_can_redo(doc));

    adif_document_redo(doc);
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 3);
    g_assert_cmpstr(adif_document_get_value(doc, 0, rig), ==, "IC-7300");
    g_assert_cmpstr(adif_document_get_value(doc, 1, rig), ==, "");   /* still skipped on redo */

    g_object_unref(doc);
}

static guint schema_change_count;
static void on_schema_changed(AdifDocument *doc, gpointer data) {
    (void)doc;
    (void)data;
    schema_change_count++;
}

/* Several column inserts in a group reach the view as one
 * "schema-changed" (one rebuild), and so do its undo and redo. */
static void test_group_coalesces_schema_changed(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    adif_document_insert_row(doc, 0);
    adif_document_set_value(doc, 0, di_at(doc, 0), "A");
    schema_change_count = 0;
    g_signal_connect(doc, "schema-changed", G_CALLBACK(on_schema_changed), NULL);

    adif_document_begin_group(doc, "Batch Stamp: Test");
    guint rig = adif_document_insert_column(doc, 1, "RIG");
    adif_document_insert_column(doc, 2, "MY_ANTENNA");
    adif_document_insert_column(doc, 3, "TX_PWR");
    adif_document_stamp_column(doc, rig, "IC-7300");
    g_assert_cmpuint(schema_change_count, ==, 0);   /* held back while open */
    adif_document_end_group(doc);
    g_assert_cmpuint(schema_change_count, ==, 1);
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 4);

    adif_document_undo(doc);
    g_assert_cmpuint(schema_change_count, ==, 2);
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 1);

    adif_document_redo(doc);
    g_assert_cmpuint(schema_change_count, ==, 3);
    g_assert_cmpstr(adif_document_get_value(doc, 0, rig), ==, "IC-7300");

    /* Outside a group, each insert still fires straight away. */
    adif_document_insert_column(doc, 4, "COMMENT");
    g_assert_cmpuint(schema_change_count, ==, 4);

    g_object_unref(doc);
}

/* adif_document_column_write_step() must be able to stop partway through
 * a column (an exhausted deadline) and finish correctly from a second
 * call - the chunked path window_stamp.c drives a turn at a time. */
static void test_stamp_column_step_is_incremental(void) {
    const char *const cols[] = { "CALL", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    guint call = di_at(doc, 0);
    const guint n = 300; /* > the step's internal clock-check interval */
    for (guint i = 0; i < n; i++) {
        adif_document_insert_row(doc, i);
        if (i % 3 != 0) { /* every third row stays blank */
            adif_document_set_value(doc, i, call, "A");
        }
    }
    adif_document_set_modified(doc, FALSE);

    /* Matches how window_stamp.c actually drives this: the column insert
     * and the stamp share one undo group. */
    adif_document_begin_group(doc, "Batch Stamp: Test");
    guint rig = adif_document_insert_column(doc, 1, "RIG");
    AdifColumnWrite *step = adif_document_column_write_begin(doc, rig, "IC-7300", TRUE);
    g_assert_nonnull(step);

    /* An already-past deadline stops the first turn partway through. */
    g_assert_true(adif_document_column_write_step(step, g_get_monotonic_time()));
    g_assert_cmpstr(adif_document_get_value(doc, n - 1, rig), ==, ""); /* not reached yet */

    /* A generous deadline drains the rest. */
    g_assert_false(
        adif_document_column_write_step(step, g_get_monotonic_time() + G_TIME_SPAN_SECOND));
    adif_document_column_write_finish(step);
    adif_document_end_group(doc);

    for (guint i = 0; i < n; i++) {
        const char *expect = (i % 3 != 0) ? "IC-7300" : ""; /* blank rows skipped */
        g_assert_cmpstr(adif_document_get_value(doc, i, rig), ==, expect);
    }
    g_assert_cmpstr(adif_document_undo_label(doc), ==, "Batch Stamp: Test");
    g_assert_true(adif_document_get_modified(doc));

    adif_document_undo(doc); /* one step reverts the column insert and the write together */
    g_assert_cmpuint(adif_document_get_n_columns(doc), ==, 1);
    g_assert_false(adif_document_get_modified(doc));

    g_object_unref(doc);
}

static void test_empty_group_records_nothing(void) {
    AdifDocument *doc = adif_document_new(); /* pristine: no history */

    adif_document_begin_group(doc, "Nothing");
    adif_document_end_group(doc);

    g_assert_false(adif_document_can_undo(doc));
    g_assert_false(adif_document_get_modified(doc));

    g_object_unref(doc);
}

/* --- search & replace ---------------------------------------------- */

/* CALL / MODE / COMMENT, three rows:
 *   W1AW   FT8   first FT8 run
 *   K1ABC  SSB   (blank comment)
 *   N0FT8  FT8   ft8 again
 * Two cells hold "FT8" exactly (MODE 0, MODE 2); "FT8"/"ft8" appears as a
 * substring in CALL 2 and both non-blank comments too. */
static AdifDocument *search_grid(guint *call, guint *mode, guint *comment) {
    const char *const cols[] = { "CALL", "MODE", "COMMENT", NULL };
    AdifDocument *doc = doc_with_columns(cols);
    *call = di_at(doc, 0);
    *mode = di_at(doc, 1);
    *comment = di_at(doc, 2);
    for (guint i = 0; i < 3; i++) {
        adif_document_insert_row(doc, i);
    }
    adif_document_set_value(doc, 0, *call, "W1AW");
    adif_document_set_value(doc, 0, *mode, "FT8");
    adif_document_set_value(doc, 0, *comment, "first FT8 run");
    adif_document_set_value(doc, 1, *call, "K1ABC");
    adif_document_set_value(doc, 1, *mode, "SSB");
    adif_document_set_value(doc, 2, *call, "N0FT8");
    adif_document_set_value(doc, 2, *mode, "FT8");
    adif_document_set_value(doc, 2, *comment, "ft8 again");
    adif_document_set_modified(doc, FALSE);
    return doc;
}

static void test_search_count_and_find(void) {
    guint call, mode, comment;
    AdifDocument *doc = search_grid(&call, &mode, &comment);

    AdifQuery q = { .query = "FT8" };   /* ASCII case-insensitive substring */
    g_assert_cmpuint(adif_document_count_matches(doc, &q), ==, 5);

    AdifCellRef m = { 0 };
    AdifCellRef top = { .row = G_MAXUINT };   /* out of range == from the top */
    g_assert_true(adif_document_find(doc, &q, top, FALSE, FALSE, &m));
    g_assert_cmpuint(m.row, ==, 0);
    g_assert_cmpuint(m.data_index, ==, mode);   /* row 0: CALL misses, MODE hits */

    /* Walk forward through every match, no wrap surprises. */
    g_assert_true(adif_document_find(doc, &q, m, FALSE, FALSE, &m));
    g_assert_cmpuint(m.row, ==, 0);
    g_assert_cmpuint(m.data_index, ==, comment);
    g_assert_true(adif_document_find(doc, &q, m, FALSE, FALSE, &m));
    g_assert_cmpuint(m.row, ==, 2);
    g_assert_cmpuint(m.data_index, ==, call);
    adif_document_find(doc, &q, m, FALSE, FALSE, &m);   /* -> row 2 MODE */
    adif_document_find(doc, &q, m, FALSE, FALSE, &m);   /* -> row 2 COMMENT */
    g_assert_cmpuint(m.row, ==, 2);
    g_assert_cmpuint(m.data_index, ==, comment);

    /* One more wraps back to the first match. */
    g_assert_true(adif_document_find(doc, &q, m, FALSE, FALSE, &m));
    g_assert_cmpuint(m.row, ==, 0);
    g_assert_cmpuint(m.data_index, ==, mode);

    /* Backwards from there wraps the other way. */
    g_assert_true(adif_document_find(doc, &q, m, TRUE, FALSE, &m));
    g_assert_cmpuint(m.row, ==, 2);
    g_assert_cmpuint(m.data_index, ==, comment);

    /* No match anywhere. */
    AdifQuery none = { .query = "ZZZ" };
    g_assert_false(adif_document_find(doc, &none, top, FALSE, TRUE, &m));
    g_assert_cmpuint(adif_document_count_matches(doc, &none), ==, 0);

    g_object_unref(doc);
}

static void test_search_options(void) {
    guint call, mode, comment;
    AdifDocument *doc = search_grid(&call, &mode, &comment);
    AdifCellRef m = { 0 };
    AdifCellRef top = { .row = G_MAXUINT };

    /* Case-sensitive: "ft8" only matches the lower-case comment on row 2. */
    AdifQuery cs = { .query = "ft8", .case_sensitive = TRUE };
    g_assert_cmpuint(adif_document_count_matches(doc, &cs), ==, 1);
    g_assert_true(adif_document_find(doc, &cs, top, FALSE, TRUE, &m));
    g_assert_cmpuint(m.row, ==, 2);
    g_assert_cmpuint(m.data_index, ==, comment);

    /* Whole-field: exactly "FT8" - the two MODE cells, not the comments. */
    AdifQuery wf = { .query = "FT8", .whole_field = TRUE };
    g_assert_cmpuint(adif_document_count_matches(doc, &wf), ==, 2);

    /* Scoped to COMMENT: "FT8" as a substring, both comments. */
    AdifQuery sc = { .query = "FT8", .scoped = TRUE, .data_index = comment };
    g_assert_cmpuint(adif_document_count_matches(doc, &sc), ==, 2);
    g_assert_true(adif_document_find(doc, &sc, top, FALSE, TRUE, &m));
    g_assert_cmpuint(m.data_index, ==, comment);

    g_object_unref(doc);
}

static void test_replace_one_cell(void) {
    guint call, mode, comment;
    AdifDocument *doc = search_grid(&call, &mode, &comment);

    /* Substring replace keeps the rest of the cell. */
    AdifQuery q = { .query = "FT8", .replacement = "FT4" };
    AdifCellRef cell = { .row = 0, .data_index = comment };
    g_assert_true(adif_document_replace(doc, &q, cell));
    g_assert_cmpstr(adif_document_get_value(doc, 0, comment), ==, "first FT4 run");

    /* One undo step, and it's exactly reversible. */
    adif_document_undo(doc);
    g_assert_cmpstr(adif_document_get_value(doc, 0, comment), ==, "first FT8 run");
    g_assert_false(adif_document_get_modified(doc));

    /* A cell that doesn't match: no-op, returns FALSE. */
    AdifCellRef ssb = { .row = 1, .data_index = mode };
    g_assert_false(adif_document_replace(doc, &q, ssb));

    /* Replacement may contain the query without looping. */
    AdifQuery grow = { .query = "FT8", .replacement = "FT8-DIGI" };
    AdifCellRef r2mode = { .row = 2, .data_index = mode };
    g_assert_true(adif_document_replace(doc, &grow, r2mode));
    g_assert_cmpstr(adif_document_get_value(doc, 2, mode), ==, "FT8-DIGI");

    g_object_unref(doc);
}

static void test_replace_all_is_one_undo_step(void) {
    guint call, mode, comment;
    AdifDocument *doc = search_grid(&call, &mode, &comment);

    /* Snapshot every cell up front. */
    const guint dis[] = { call, mode, comment };
    char *before[3][3];
    for (guint r = 0; r < 3; r++) {
        for (guint c = 0; c < 3; c++) {
            before[r][c] = g_strdup(adif_document_get_value(doc, r, dis[c]));
        }
    }

    AdifQuery q = { .query = "FT8", .replacement = "FT4" };
    guint n = adif_document_replace_all(doc, &q);
    g_assert_cmpuint(n, ==, 5);   /* every matching cell, across all columns */
    g_assert_cmpuint(adif_document_count_matches(doc, &q), ==, 0);
    g_assert_cmpstr(adif_document_get_value(doc, 0, comment), ==, "first FT4 run");
    g_assert_cmpstr(adif_document_get_value(doc, 2, call), ==, "N0FT4");
    g_assert_true(adif_document_get_modified(doc));

    /* The whole thing is a single undo step. */
    char *label = g_strdup_printf("Replace \xE2\x80\x9C%s\xE2\x80\x9D", "FT8");
    g_assert_cmpstr(adif_document_undo_label(doc), ==, label);
    g_assert_false(adif_document_can_redo(doc));

    adif_document_undo(doc);   /* ONE Ctrl+Z */

    /* Every cell is byte-for-byte what it was. */
    for (guint r = 0; r < 3; r++) {
        for (guint c = 0; c < 3; c++) {
            g_assert_cmpstr(adif_document_get_value(doc, r, dis[c]), ==, before[r][c]);
        }
    }
    g_assert_false(adif_document_get_modified(doc));   /* back to the saved state */
    g_assert_true(adif_document_can_redo(doc));
    /* Exactly one step was undone: redo points back at the replace group. */
    g_assert_cmpstr(adif_document_redo_label(doc), ==, label);

    /* Redo re-applies the lot in one step. */
    adif_document_redo(doc);
    g_assert_cmpuint(adif_document_count_matches(doc, &q), ==, 0);
    g_assert_cmpstr(adif_document_get_value(doc, 2, call), ==, "N0FT4");
    g_assert_true(adif_document_get_modified(doc));

    for (guint r = 0; r < 3; r++) {
        for (guint c = 0; c < 3; c++) {
            g_free(before[r][c]);
        }
    }
    g_free(label);
    g_object_unref(doc);
}

static void test_replace_all_scoped_and_empty(void) {
    guint call, mode, comment;
    AdifDocument *doc = search_grid(&call, &mode, &comment);

    /* Scoped: only the COMMENT column is touched (both comments match). */
    AdifQuery sc = {
        .query = "FT8", .replacement = "digital", .scoped = TRUE, .data_index = comment
    };
    g_assert_cmpuint(adif_document_replace_all(doc, &sc), ==, 2);
    g_assert_cmpstr(adif_document_get_value(doc, 0, comment), ==, "first digital run");
    g_assert_cmpstr(adif_document_get_value(doc, 2, comment), ==, "digital again");
    g_assert_cmpstr(adif_document_get_value(doc, 0, mode), ==, "FT8");   /* untouched */
    g_assert_cmpstr(adif_document_get_value(doc, 2, call), ==, "N0FT8"); /* untouched */

    /* Empty query does nothing and records no step. */
    adif_document_set_modified(doc, FALSE);
    AdifQuery empty = { .query = "", .replacement = "x" };
    g_assert_cmpuint(adif_document_replace_all(doc, &empty), ==, 0);
    g_assert_false(adif_document_get_modified(doc));

    /* Replacing with "" clears the matched span. */
    AdifQuery del = { .query = "FT8", .whole_field = TRUE, .replacement = "" };
    g_assert_cmpuint(adif_document_replace_all(doc, &del), ==, 2);
    g_assert_cmpstr(adif_document_get_value(doc, 0, mode), ==, "");
    g_assert_cmpstr(adif_document_get_value(doc, 2, mode), ==, "");

    g_object_unref(doc);
}

/* Each column Replace All touched is refreshed once (not once per cell),
 * after the group closes; a single replace refreshes its column. */
static GArray *refreshed; /* guint data-indices, in emission order */
static void on_column_values_changed(AdifDocument *doc, guint data_index, gpointer data) {
    (void)doc;
    (void)data;
    g_array_append_val(refreshed, data_index);
}

static void test_replace_refreshes_each_column_once(void) {
    guint call, mode, comment;
    AdifDocument *doc = search_grid(&call, &mode, &comment);
    refreshed = g_array_new(FALSE, FALSE, sizeof(guint));
    g_signal_connect(doc, "column-values-changed", G_CALLBACK(on_column_values_changed), NULL);

    AdifQuery q = { .query = "FT8", .replacement = "FT4" };
    g_assert_cmpuint(adif_document_replace_all(doc, &q), ==, 5); /* over all 3 columns */
    g_assert_cmpuint(refreshed->len, ==, 3);
    gboolean seen_call = FALSE, seen_mode = FALSE, seen_comment = FALSE;
    for (guint i = 0; i < refreshed->len; i++) {
        guint di = g_array_index(refreshed, guint, i);
        seen_call |= di == call;
        seen_mode |= di == mode;
        seen_comment |= di == comment;
    }
    g_assert_true(seen_call && seen_mode && seen_comment);

    g_array_set_size(refreshed, 0);
    AdifQuery back = { .query = "FT4", .replacement = "FT8" };
    AdifCellRef cell = { .row = 0, .data_index = mode };
    g_assert_true(adif_document_replace(doc, &back, cell));
    g_assert_cmpuint(refreshed->len, ==, 1);
    g_assert_cmpuint(g_array_index(refreshed, guint, 0), ==, mode);

    g_array_unref(refreshed);
    g_object_unref(doc);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/document/empty", test_empty);
    g_test_add_func("/document/build-and-read", test_build_and_read);
    g_test_add_func("/document/column-insert-positions", test_column_insert_positions);
    g_test_add_func("/document/delete-column-keeps-values", test_delete_column_keeps_other_values);
    g_test_add_func("/document/delete-last-column-refused", test_delete_last_column_refused);
    g_test_add_func("/document/fill-column", test_fill_column);
    g_test_add_func("/document/replace-all-signals-history-once",
                    test_replace_all_signals_history_once);
    g_test_add_func("/document/fill-column-step-is-incremental",
                    test_fill_column_step_is_incremental);
    g_test_add_func("/document/row-insert-delete", test_row_insert_delete);
    g_test_add_func("/document/modified-flag", test_modified_flag);
    g_test_add_func("/document/new-column-name-check", test_new_column_name_check);
    g_test_add_func("/document/serialise-skips-empty", test_serialise_skips_empty);
    g_test_add_func("/document/serialise-skips-empty-rows", test_serialise_skips_empty_rows);
    g_test_add_func("/document/parse-roundtrip", test_parse_roundtrip);
    g_test_add_func("/document/reset-empties-the-document", test_reset_empties_the_document);
    g_test_add_func("/document/header-records-origin", test_header_records_origin);
    g_test_add_func("/document/header-no-origin-when-absent", test_header_no_origin_when_absent);
    g_test_add_func("/document/header-other-fields-kept", test_header_other_fields_kept);
    g_test_add_func("/document/record-type-indicators-kept", test_record_type_indicators_kept);
    g_test_add_func("/document/header-blank-origin-not-recorded",
                    test_header_blank_origin_not_recorded);
    g_test_add_func("/document/header-unknown-hash-is-written",
                    test_header_unknown_hash_is_written);
    g_test_add_func("/parser/rejects-non-utf8", test_parse_rejects_non_utf8);
    g_test_add_func("/parser/flags-length-mismatch", test_parse_flags_length_mismatch);
    g_test_add_func("/parser/utf8-byte-counts", test_parse_utf8_byte_counts);
    g_test_add_func("/parser/empty-file", test_parse_empty_file);
    g_test_add_func("/parser/headerless", test_parse_headerless);
    g_test_add_func("/parser/header-only", test_parse_header_only);
    g_test_add_func("/parser/value-with-angle-brackets", test_parse_value_with_angle_brackets);
    g_test_add_func("/parser/type-indicator-ignored", test_parse_type_indicator_ignored);
    g_test_add_func("/parser/duplicate-field-last-wins", test_parse_duplicate_field_last_wins);
    g_test_add_func("/parser/record-values-by-column", test_parse_record_values_by_column);
    g_test_add_func("/parser/lowercases-names", test_parse_lowercases_names);
    g_test_add_func("/parser/trailing-record-without-eor", test_parse_trailing_record_without_eor);
    g_test_add_func("/parser/unterminated-tag", test_parse_unterminated_tag);
    g_test_add_func("/parser/length-past-eof", test_parse_length_past_eof);
    g_test_add_func("/parser/fit-char-count", test_parse_fit_char_count);
    g_test_add_func("/parser/fit-char-count-with-angle", test_parse_fit_char_count_with_angle);
    g_test_add_func("/parser/fit-too-long", test_parse_fit_too_long);
    g_test_add_func("/parser/fit-too-long-over-eor", test_parse_fit_too_long_over_eor);
    g_test_add_func("/parser/fit-too-short-trims-gap", test_parse_fit_too_short_trims_gap);
    g_test_add_func("/parser/fit-warning-lists-examples", test_parse_fit_warning_lists_examples);
    g_test_add_func("/parser/fit-duplicate-last-wins", test_parse_fit_duplicate_last_wins);
    g_test_add_func("/parser/clean-has-no-repairs", test_parse_clean_has_no_repairs);
    g_test_add_func("/document/repaired-rows", test_document_repaired_rows);
    g_test_add_func("/parser/spec-field-names", test_parse_spec_field_names);
    g_test_add_func("/parser/bad-field-name-skipped", test_parse_bad_field_name_skipped);
    g_test_add_func("/parser/stray-lt-before-tag", test_parse_stray_lt_before_tag);
    g_test_add_func("/parser/length-splits-character", test_parse_length_splits_character);
    g_test_add_func("/parser/crlf-endings", test_parse_crlf_endings);
    g_test_add_func("/parser/leading-bom", test_parse_leading_bom);
    g_test_add_func("/parser/non-utf8-in-header", test_parse_non_utf8_in_header);

    g_test_add_func("/field/name-valid", test_field_name_valid);
    g_test_add_func("/field/name-invalid", test_field_name_invalid);

    g_test_add_func("/row/grows-with-gaps", test_row_grows_with_gaps);

    g_test_add_func("/document/undo-redo-cell", test_undo_redo_cell);
    g_test_add_func("/document/undo-insert-column", test_undo_insert_column);
    g_test_add_func("/document/undo-delete-column-restores-values",
                    test_undo_delete_column_restores_values);
    g_test_add_func("/document/undo-row-ops", test_undo_row_ops);
    g_test_add_func("/document/undo-fill-column", test_undo_fill_column);
    g_test_add_func("/document/coalesced-cell-edits", test_coalesced_cell_edits_are_one_step);
    g_test_add_func("/document/redo-cleared-by-new-edit", test_redo_cleared_by_new_edit);
    g_test_add_func("/document/undo-to-saved-clears-modified", test_undo_to_saved_clears_modified);
    g_test_add_func("/document/find-column", test_find_column);
    g_test_add_func("/document/undo-group-is-one-step", test_undo_group_is_one_step);
    g_test_add_func("/document/empty-group-records-nothing", test_empty_group_records_nothing);
    g_test_add_func("/document/group-coalesces-schema-changed",
                    test_group_coalesces_schema_changed);
    g_test_add_func("/document/stamp-column-step-is-incremental",
                    test_stamp_column_step_is_incremental);

    g_test_add_func("/document/search-count-and-find", test_search_count_and_find);
    g_test_add_func("/document/search-options", test_search_options);
    g_test_add_func("/document/replace-one-cell", test_replace_one_cell);
    g_test_add_func("/document/replace-all-is-one-undo-step", test_replace_all_is_one_undo_step);
    g_test_add_func("/document/replace-all-scoped-and-empty", test_replace_all_scoped_and_empty);
    g_test_add_func("/document/replace-refreshes-each-column-once",
                    test_replace_refreshes_each_column_once);

    return g_test_run();
}
