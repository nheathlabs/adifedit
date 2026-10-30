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

/* libFuzzer target for the ADIF parser and the save path behind it (the
 * `fuzz` preset; see CONTRIBUTING.md "Fuzzing"). For any input the parser
 * accepts:
 *   - loading it into an AdifDocument and saving must give ADIF that
 *     re-parses with no warning (adifedit's own output has exact lengths);
 *   - loading and saving that again must keep every record's values (the
 *     column order may change - it follows first appearance, so a first
 *     record without some field moves that column - but nothing else),
 *     each column's type indicator, and every header field, in order.
 * Memory errors and undefined behaviour are caught by the sanitizers the
 * preset builds with.
 *
 * Without ADIFEDIT_LIBFUZZER (every other build) it gets a main() that
 * replays the files in the directories it is given - the `fuzz_corpus`
 * ctest, which keeps every input saved in tests/fuzz_corpus/ passing. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "adif_document.h"
#include "adif_parser.h"

/* The name and signature are libFuzzer's. */
/* NOLINTNEXTLINE(readability-identifier-naming) */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Do `a` and `b` hold the same records, field for field? */
static void assert_same_records(const AdifParseResult *a, const AdifParseResult *b) {
    g_assert_cmpuint(a->records->len, ==, b->records->len);
    g_assert_cmpuint(a->fieldnames->len, ==, b->fieldnames->len);
    for (guint r = 0; r < a->records->len; r++) {
        const AdifRecord *ra = g_ptr_array_index(a->records, r);
        const AdifRecord *rb = g_ptr_array_index(b->records, r);
        for (guint c = 0; c < a->fieldnames->len; c++) {
            const char *name = g_ptr_array_index(a->fieldnames, c);
            g_assert_cmpstr(adif_record_get_field(ra, name), ==, adif_record_get_field(rb, name));
        }
    }
    /* Each column keeps its type indicator, wherever it moved to. */
    for (guint c = 0; c < a->fieldnames->len; c++) {
        guint at = 0;
        g_assert_true(g_ptr_array_find_with_equal_func(
            b->fieldnames, g_ptr_array_index(a->fieldnames, c), g_str_equal, &at));
        g_assert_cmpint(g_array_index(a->fieldtypes, char, c), ==,
                        g_array_index(b->fieldtypes, char, at));
    }
}

/* ... and the same header fields, in order? CREATED_TIMESTAMP's value is
 * the save time, so only its presence is compared. */
static void assert_same_header(const AdifParseResult *a, const AdifParseResult *b) {
    guint n = adif_record_header_n_fields(a->header);
    g_assert_cmpuint(n, ==, adif_record_header_n_fields(b->header));
    for (guint i = 0; i < n; i++) {
        const char *name = adif_record_header_name_at(a->header, i);
        g_assert_cmpstr(name, ==, adif_record_header_name_at(b->header, i));
        g_assert_cmpint(adif_record_header_type_at(a->header, i), ==,
                        adif_record_header_type_at(b->header, i));
        if (strcmp(name, "CREATED_TIMESTAMP") != 0) {
            g_assert_cmpstr(adif_record_get_field(a->header, name), ==,
                            adif_record_get_field(b->header, name));
        }
    }
}

/* Load `result` into a fresh document and serialise it (caller frees). */
static char *load_and_save(const AdifParseResult *result) {
    AdifDocument *doc = adif_document_new();
    adif_document_load_parse(doc, result);
    char *out = adif_document_to_adif(doc, "fuzz", "fuzz");
    g_object_unref(doc);
    return out;
}

/* NOLINTNEXTLINE(readability-identifier-naming) */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    AdifParseResult *first = adif_parse_data((const char *)data, size, NULL);
    if (first == NULL) {
        return 0; /* rejected (not UTF-8): nothing more to check */
    }
    char *saved = load_and_save(first);
    adif_parse_result_free(first);

    AdifParseResult *second = adif_parse_data(saved, strlen(saved), NULL);
    g_assert_nonnull(second);
    g_assert_null(second->warning);
    char *resaved = load_and_save(second);
    AdifParseResult *third = adif_parse_data(resaved, strlen(resaved), NULL);
    g_assert_nonnull(third);
    g_assert_null(third->warning);
    assert_same_records(second, third);
    assert_same_header(second, third);

    adif_parse_result_free(third);
    adif_parse_result_free(second);
    g_free(resaved);
    g_free(saved);
    return 0;
}

#ifndef ADIFEDIT_LIBFUZZER
/* Run every file in `dir` through the target. */
static void replay_dir(const char *dir) {
    GError *error = NULL;
    GDir *d = g_dir_open(dir, 0, &error);
    g_assert_no_error(error);
    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        char *path = g_build_filename(dir, name, NULL);
        char *contents = NULL;
        gsize length = 0;
        g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
        g_print("%s\n", name);
        LLVMFuzzerTestOneInput((const uint8_t *)contents, length);
        g_free(contents);
        g_free(path);
    }
    g_dir_close(d);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        g_printerr("usage: %s CORPUS_DIR...\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        replay_dir(argv[i]);
    }
    return 0;
}
#endif
