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

/* Headless tests for the static ADIF_KNOWN_FIELDS suggestion list. */

#include <string.h>

#include "adif_field.h"
#include "adif_known_fields.h"

static guint count_fields(void) {
    guint n = 0;
    while (ADIF_KNOWN_FIELDS[n] != NULL) {
        n++;
    }
    return n;
}

static void test_non_empty_and_terminated(void) {
    guint n = count_fields();
    g_assert_cmpuint(n, >, 0);
    g_assert_null(ADIF_KNOWN_FIELDS[n]);
}

/* Strictly ascending under strcmp proves both "sorted" and "no
 * duplicates" in one pass. */
static void test_sorted_no_duplicates(void) {
    for (guint i = 1; ADIF_KNOWN_FIELDS[i] != NULL; i++) {
        g_assert_cmpint(strcmp(ADIF_KNOWN_FIELDS[i - 1], ADIF_KNOWN_FIELDS[i]), <, 0);
    }
}

/* Catches stray whitespace / lowercase / punctuation transcription
 * slips - every entry must itself be a syntactically valid field name. */
static void test_every_entry_is_a_valid_field_name(void) {
    for (guint i = 0; ADIF_KNOWN_FIELDS[i] != NULL; i++) {
        g_assert_true(adif_field_name_is_valid(ADIF_KNOWN_FIELDS[i]));
    }
}

static gboolean contains(const char *name) {
    for (guint i = 0; ADIF_KNOWN_FIELDS[i] != NULL; i++) {
        if (strcmp(ADIF_KNOWN_FIELDS[i], name) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void test_known_qso_fields_present(void) {
    g_assert_true(contains("CALL"));
    g_assert_true(contains("QSO_DATE"));
    g_assert_true(contains("BAND"));
    g_assert_true(contains("MODE"));
    g_assert_true(contains("GRIDSQUARE"));
    g_assert_true(contains("MY_GRIDSQUARE"));
    g_assert_true(contains("RST_RCVD"));
    g_assert_true(contains("RST_SENT"));
    g_assert_true(contains("SOTA_REF"));
    g_assert_true(contains("POTA_REF"));
}

/* Insert Column adds a per-record field, not a header field - these
 * must NOT appear (header-only fields, and the USERDEFn template). */
static void test_header_only_fields_absent(void) {
    g_assert_false(contains("ADIF_VER"));
    g_assert_false(contains("CREATED_TIMESTAMP"));
    g_assert_false(contains("PROGRAMID"));
    g_assert_false(contains("PROGRAMVERSION"));
    for (guint i = 0; ADIF_KNOWN_FIELDS[i] != NULL; i++) {
        g_assert_true(g_str_has_prefix(ADIF_KNOWN_FIELDS[i], "USERDEF") == FALSE);
    }
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/known-fields/non-empty-and-terminated", test_non_empty_and_terminated);
    g_test_add_func("/known-fields/sorted-no-duplicates", test_sorted_no_duplicates);
    g_test_add_func("/known-fields/every-entry-valid", test_every_entry_is_a_valid_field_name);
    g_test_add_func("/known-fields/known-qso-fields-present", test_known_qso_fields_present);
    g_test_add_func("/known-fields/header-only-fields-absent", test_header_only_fields_absent);
    return g_test_run();
}
