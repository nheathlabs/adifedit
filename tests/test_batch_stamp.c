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

/* Headless tests for the batch-stamp .bst parser. */

#include <string.h>

#include <glib/gstdio.h>

#include "batch_stamp.h"

static char *write_tmp(const char *text) {
    char *path = NULL;
    gint fd = g_file_open_tmp("adifedit-XXXXXX.bst", &path, NULL);
    g_assert_cmpint(fd, !=, -1);
    g_close(fd, NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    return path;
}

/* --- stamp discovery (XDG dirs sandboxed to the build tree) ------- */

/* The <data-dir>/adifedit/stamps directory under a sandboxed XDG root. */
static char *stamps_dir(const char *xdg_root) {
    return g_build_filename(xdg_root, "adifedit", "stamps", NULL);
}

/* Remove every file from one stamps directory (it holds only files). */
static void wipe_stamps(const char *xdg_root) {
    char *dir = stamps_dir(xdg_root);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d != NULL) {
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            char *p = g_build_filename(dir, name, NULL);
            g_remove(p);
            g_free(p);
        }
        g_dir_close(d);
    }
    g_free(dir);
}

static void put_stamp(const char *xdg_root, const char *id, const char *text) {
    char *dir = stamps_dir(xdg_root);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
    char *name = g_strconcat(id, ".bst", NULL);
    char *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    g_free(path);
    g_free(name);
    g_free(dir);
}

static void test_discovery_finds_and_sorts(void) {
    const char *home = g_getenv("XDG_DATA_HOME");
    wipe_stamps(home);
    put_stamp(home, "z", "Zeta\nCALL\n");
    put_stamp(home, "a", "Alpha\nCALL\n");

    GPtrArray *list = adif_stamp_list();
    g_assert_cmpuint(list->len, ==, 2);
    g_assert_cmpstr(((AdifStamp *)list->pdata[0])->title, ==, "Alpha"); /* sorted by title */
    g_assert_cmpstr(((AdifStamp *)list->pdata[1])->title, ==, "Zeta");
    adif_stamp_list_free(list);
    wipe_stamps(home);
}

static void test_discovery_user_wins_on_id(void) {
    const char *home = g_getenv("XDG_DATA_HOME");
    const char *sys = g_getenv("XDG_DATA_DIRS");
    wipe_stamps(home);
    wipe_stamps(sys);
    put_stamp(sys, "dup", "From system\nCALL\n");
    put_stamp(home, "dup", "From user\nRIG\n");

    GPtrArray *list = adif_stamp_list();
    g_assert_cmpuint(list->len, ==, 1);
    g_assert_cmpstr(((AdifStamp *)list->pdata[0])->id, ==, "dup");
    g_assert_cmpstr(((AdifStamp *)list->pdata[0])->title, ==, "From user");
    adif_stamp_list_free(list);
    wipe_stamps(home);
    wipe_stamps(sys);
}

static void test_user_dir_is_created(void) {
    char *dir = adif_stamp_user_dir();
    char *want = stamps_dir(g_getenv("XDG_DATA_HOME"));
    g_assert_cmpstr(dir, ==, want);
    g_assert_true(g_file_test(dir, G_FILE_TEST_IS_DIR));
    g_free(want);
    g_free(dir);
}

static void test_shipped_example(void) {
    GError *error = NULL;
    AdifStamp *s = adif_stamp_load(EXAMPLE_BST, &error);
    g_assert_no_error(error);
    g_assert_nonnull(s);

    g_assert_cmpstr(s->id, ==, "example");     /* filename */
    g_assert_cmpstr(s->title, ==, "Example");  /* line 1 - wins over the filename */
    g_assert_cmpuint(s->fields->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(s->fields, 0), ==, "MY_ANTENNA");
    g_assert_cmpstr(g_ptr_array_index(s->fields, 1), ==, "MY_RIG");
    g_assert_cmpstr(g_ptr_array_index(s->fields, 2), ==, "TX_PWR");
    g_assert_cmpuint(s->bad->len, ==, 0);

    adif_stamp_free(s);
}

static void test_case_fold_and_dedup(void) {
    char *path = write_tmp("Station\n\nrig\nRIG\n  My_Antenna  \n");
    AdifStamp *s = adif_stamp_load(path, NULL);
    g_assert_nonnull(s);
    g_assert_cmpstr(s->title, ==, "Station");
    g_assert_cmpuint(s->fields->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(s->fields, 0), ==, "RIG");
    g_assert_cmpstr(g_ptr_array_index(s->fields, 1), ==, "MY_ANTENNA");
    adif_stamp_free(s);
    g_remove(path);
    g_free(path);
}

static void test_bad_field_names(void) {
    char *path = write_tmp("Broken\nRIG\nMY ANTENNA\n2TONE\nEOR\n");
    AdifStamp *s = adif_stamp_load(path, NULL);
    g_assert_nonnull(s);
    g_assert_cmpuint(s->fields->len, ==, 1); /* just RIG */
    g_assert_cmpuint(s->bad->len, ==, 3);    /* MY ANTENNA, 2TONE, EOR */
    g_assert_nonnull(strstr(g_ptr_array_index(s->bad, 0), "MY ANTENNA"));
    adif_stamp_free(s);
    g_remove(path);
    g_free(path);
}

static void test_comment_before_title(void) {
    char *path = write_tmp("# a note\n#another\nMy Stamp\nCALL\n");
    AdifStamp *s = adif_stamp_load(path, NULL);
    g_assert_nonnull(s);
    g_assert_cmpstr(s->title, ==, "My Stamp");
    g_assert_cmpuint(s->fields->len, ==, 1);
    adif_stamp_free(s);
    g_remove(path);
    g_free(path);
}

static void test_structural_errors(void) {
    GError *error = NULL;

    char *p1 = write_tmp("# only comments\n\n");
    g_assert_null(adif_stamp_load(p1, &error));
    g_assert_error(error, ADIF_STAMP_ERROR, ADIF_STAMP_ERROR_NO_TITLE);
    g_clear_error(&error);
    g_remove(p1);
    g_free(p1);

    char *p2 = write_tmp("Just a title\n\n# nothing else\n");
    g_assert_null(adif_stamp_load(p2, &error));
    g_assert_error(error, ADIF_STAMP_ERROR, ADIF_STAMP_ERROR_NO_FIELDS);
    g_clear_error(&error);
    g_remove(p2);
    g_free(p2);
}

int main(int argc, char *argv[]) {
    /* Point XDG data dirs at the build tree before anything reads (and
     * caches) the real ones, so discovery only sees test fixtures. */
    char *home = g_build_filename(TEST_BUILDDIR, "xdg-home", NULL);
    char *sys = g_build_filename(TEST_BUILDDIR, "xdg-sys", NULL);
    g_setenv("XDG_DATA_HOME", home, TRUE);
    g_setenv("XDG_DATA_DIRS", sys, TRUE);
    g_free(home);
    g_free(sys);

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/stamp/shipped-example", test_shipped_example);
    g_test_add_func("/stamp/case-fold-and-dedup", test_case_fold_and_dedup);
    g_test_add_func("/stamp/bad-field-names", test_bad_field_names);
    g_test_add_func("/stamp/comment-before-title", test_comment_before_title);
    g_test_add_func("/stamp/structural-errors", test_structural_errors);
    g_test_add_func("/stamp/discovery-finds-and-sorts", test_discovery_finds_and_sorts);
    g_test_add_func("/stamp/discovery-user-wins-on-id", test_discovery_user_wins_on_id);
    g_test_add_func("/stamp/user-dir-is-created", test_user_dir_is_created);
    return g_test_run();
}
