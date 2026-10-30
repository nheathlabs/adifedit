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

/* Headless tests for the async load/save wrappers in adif_io. Each test
 * drives one operation and spins the default main context until its
 * callback lands. */

#include <string.h>

#include <glib/gstdio.h>

#include "adif_io.h"
#include "adif_parser.h"

typedef struct {
    GMainLoop *loop;
    AdifParseResult *result; /* load */
    gboolean saved;          /* save */
    char *etag;              /* the file's etag, from either */
    GError *error;
} IoWait;

static void on_loaded(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source;
    IoWait *w = data;
    g_free(w->etag);
    w->etag = NULL;
    w->result = adif_io_load_finish(res, &w->etag, &w->error);
    g_main_loop_quit(w->loop);
}

static void on_saved(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source;
    IoWait *w = data;
    g_free(w->etag);
    w->etag = NULL;
    w->saved = adif_io_save_finish(res, &w->etag, &w->error);
    g_main_loop_quit(w->loop);
}

static char *tmp_path(void) {
    char *path = NULL;
    gint fd = g_file_open_tmp("adifedit-io-XXXXXX.adi", &path, NULL);
    g_assert_cmpint(fd, !=, -1);
    g_close(fd, NULL);
    return path;
}

static void test_load_async(void) {
    char *path = tmp_path();
    g_assert_true(
        g_file_set_contents(path, "<CALL:6>KJ5LMR<BAND:3>20m<EOR>\n<CALL:4>W1AW<EOR>\n", -1, NULL));

    IoWait w = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_load_async(path, on_loaded, &w);
    g_main_loop_run(w.loop);

    g_assert_no_error(w.error);
    g_assert_nonnull(w.result);
    g_assert_cmpuint(w.result->records->len, ==, 2);
    g_assert_cmpstr(adif_record_get_field(g_ptr_array_index(w.result->records, 0), "CALL"), ==,
                    "KJ5LMR");
    adif_parse_result_free(w.result);

    g_free(w.etag);

    g_main_loop_unref(w.loop);
    g_remove(path);
    g_free(path);
}

static void test_load_async_missing_file(void) {
    IoWait w = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_load_async("/nonexistent/adifedit-io-test.adi", on_loaded, &w);
    g_main_loop_run(w.loop);

    g_assert_null(w.result);
    g_assert_nonnull(w.error);
    g_clear_error(&w.error);
    g_free(w.etag);
    g_main_loop_unref(w.loop);
}

static void test_load_async_rejects_non_utf8(void) {
    char *path = tmp_path();
    static const char latin1[] = "<CALL:5>OH2XY<NAME:5>Bj\366rn<EOR>\n";
    g_assert_true(g_file_set_contents(path, latin1, sizeof(latin1) - 1, NULL));

    IoWait w = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_load_async(path, on_loaded, &w);
    g_main_loop_run(w.loop);

    g_assert_null(w.result);
    g_assert_error(w.error, ADIF_PARSE_ERROR, ADIF_PARSE_ERROR_ENCODING);
    g_clear_error(&w.error);

    g_free(w.etag);

    g_main_loop_unref(w.loop);
    g_remove(path);
    g_free(path);
}

static void test_save_async(void) {
    char *path = tmp_path();

    IoWait w = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_save_async(path, g_strdup("hello adif\n"), NULL, on_saved, &w); /* takes the string */
    g_main_loop_run(w.loop);

    g_assert_no_error(w.error);
    g_assert_true(w.saved);

    char *back = NULL;
    g_assert_true(g_file_get_contents(path, &back, NULL, NULL));
    g_assert_cmpstr(back, ==, "hello adif\n");
    g_free(back);

    g_free(w.etag);

    g_main_loop_unref(w.loop);
    g_remove(path);
    g_free(path);
}

static void test_save_then_load_roundtrip(void) {
    char *path = tmp_path();
    const char *adif = "<ADIF_VER:5>3.1.4<EOH>\n<CALL:4>W1AW<MODE:2>CW<EOR>\n";

    IoWait sw = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_save_async(path, g_strdup(adif), NULL, on_saved, &sw);
    g_main_loop_run(sw.loop);
    g_assert_true(sw.saved);
    g_free(sw.etag);
    g_main_loop_unref(sw.loop);

    IoWait lw = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_load_async(path, on_loaded, &lw);
    g_main_loop_run(lw.loop);
    g_assert_nonnull(lw.result);
    g_assert_cmpuint(lw.result->records->len, ==, 1);
    g_assert_cmpstr(adif_record_get_field(g_ptr_array_index(lw.result->records, 0), "MODE"), ==,
                    "CW");
    adif_parse_result_free(lw.result);
    g_free(lw.etag);
    g_main_loop_unref(lw.loop);

    g_remove(path);
    g_free(path);
}

#ifndef G_OS_WIN32
/* Saving through a symlink writes the file it points at - the link stays
 * a link - and keeps that file's permissions. */
static void test_save_through_symlink(void) {
    char *path = tmp_path();
    g_assert_cmpint(g_chmod(path, 0640), ==, 0);
    char *link = g_strconcat(path, ".link", NULL);
    GFile *link_file = g_file_new_for_path(link);
    g_assert_true(g_file_make_symbolic_link(link_file, path, NULL, NULL));

    IoWait w = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_save_async(link, g_strdup("via link\n"), NULL, on_saved, &w);
    g_main_loop_run(w.loop);
    g_assert_no_error(w.error);
    g_assert_true(w.saved);
    g_free(w.etag);
    g_main_loop_unref(w.loop);

    g_assert_cmpint(g_file_query_file_type(link_file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL),
                    ==, G_FILE_TYPE_SYMBOLIC_LINK);
    g_object_unref(link_file);
    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0640);
    char *back = NULL;
    g_assert_true(g_file_get_contents(path, &back, NULL, NULL));
    g_assert_cmpstr(back, ==, "via link\n");
    g_free(back);

    g_remove(link);
    g_remove(path);
    g_free(link);
    g_free(path);
}
#endif

/* Load `path`, returning its etag (caller frees). */
static char *load_etag(const char *path) {
    IoWait w = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_load_async(path, on_loaded, &w);
    g_main_loop_run(w.loop);
    g_assert_no_error(w.error);
    g_assert_nonnull(w.etag);
    adif_parse_result_free(w.result);
    g_main_loop_unref(w.loop);
    return w.etag;
}

/* Save `contents` to `path`, requiring `etag`; returns the IoWait for
 * the caller to check (and free: loop, etag, error). */
static IoWait save_with_etag(const char *path, const char *contents, const char *etag) {
    IoWait w = { .loop = g_main_loop_new(NULL, FALSE) };
    adif_io_save_async(path, g_strdup(contents), etag, on_saved, &w);
    g_main_loop_run(w.loop);
    return w;
}

/* A file that changed after it was loaded - a logger appending a QSO -
 * isn't overwritten by a save carrying the load's etag. */
static void test_save_refuses_changed_file(void) {
    char *path = tmp_path();
    g_assert_true(g_file_set_contents(path, "<CALL:4>W1AW<EOR>\n", -1, NULL));
    char *etag = load_etag(path);

    /* Behind adifedit's back, with a clearly different mtime. */
    g_assert_true(g_file_set_contents(path, "<CALL:4>W1AW<EOR>\n<CALL:5>G4XYZ<EOR>\n", -1, NULL));
    GFile *file = g_file_new_for_path(path);
    g_assert_true(g_file_set_attribute_uint64(file, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                              (guint64)(g_get_real_time() / G_USEC_PER_SEC) + 60,
                                              G_FILE_QUERY_INFO_NONE, NULL, NULL));
    g_object_unref(file);

    IoWait w = save_with_etag(path, "edited\n", etag);
    g_assert_false(w.saved);
    g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG);
    g_clear_error(&w.error);
    g_main_loop_unref(w.loop);

    char *back = NULL;
    g_assert_true(g_file_get_contents(path, &back, NULL, NULL));
    g_assert_cmpstr(back, ==, "<CALL:4>W1AW<EOR>\n<CALL:5>G4XYZ<EOR>\n"); /* untouched */
    g_free(back);

    g_free(etag);
    g_remove(path);
    g_free(path);
}

/* An unchanged file saves with the load's etag, and the save hands back
 * the new etag, which the next save then passes. */
static void test_save_with_matching_etag(void) {
    char *path = tmp_path();
    g_assert_true(g_file_set_contents(path, "<CALL:4>W1AW<EOR>\n", -1, NULL));
    char *etag = load_etag(path);

    IoWait first = save_with_etag(path, "one\n", etag);
    g_assert_no_error(first.error);
    g_assert_true(first.saved);
    g_assert_nonnull(first.etag);

    IoWait second = save_with_etag(path, "two\n", first.etag);
    g_assert_no_error(second.error);
    g_assert_true(second.saved);

    char *back = NULL;
    g_assert_true(g_file_get_contents(path, &back, NULL, NULL));
    g_assert_cmpstr(back, ==, "two\n");
    g_free(back);

    g_free(second.etag);
    g_main_loop_unref(second.loop);
    g_free(first.etag);
    g_main_loop_unref(first.loop);
    g_free(etag);
    g_remove(path);
    g_free(path);
}

static void test_copy_name(void) {
    const struct {
        const char *in, *out;
    } cases[] = {
        { "log.adi", "log-adifedit.adi" }, { "log.ADIF", "log-adifedit.ADIF" },
        { "a.b.adi", "a.b-adifedit.adi" }, { "log", "log-adifedit.adi" },
        { ".adi", ".adi-adifedit.adi" }, /* a dotfile has no extension */
    };
    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
        char *got = adif_io_copy_name(cases[i].in);
        g_assert_cmpstr(got, ==, cases[i].out);
        g_free(got);
    }
}

static void test_same_file(void) {
    char *path = tmp_path();
    char *other = tmp_path();
    char *dir = g_path_get_dirname(path);
    char *base = g_path_get_basename(path);
    char *dotted = g_build_filename(dir, ".", base, NULL);
    char *missing = g_strconcat(path, ".missing", NULL);

    g_assert_true(adif_io_same_file(path, path));
    g_assert_true(adif_io_same_file(path, dotted));
    g_assert_false(adif_io_same_file(path, other));
    g_assert_false(adif_io_same_file(path, missing));
    g_assert_true(adif_io_same_file(missing, missing));

#ifndef G_OS_WIN32
    char *link = g_strconcat(path, ".link", NULL);
    GFile *link_file = g_file_new_for_path(link);
    g_assert_true(g_file_make_symbolic_link(link_file, path, NULL, NULL));
    g_object_unref(link_file);
    g_assert_true(adif_io_same_file(path, link));
    g_remove(link);
    g_free(link);
#endif

    g_remove(path);
    g_remove(other);
    g_free(missing);
    g_free(dotted);
    g_free(base);
    g_free(dir);
    g_free(other);
    g_free(path);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/io/load-async", test_load_async);
    g_test_add_func("/io/load-async-missing-file", test_load_async_missing_file);
    g_test_add_func("/io/load-async-rejects-non-utf8", test_load_async_rejects_non_utf8);
    g_test_add_func("/io/save-async", test_save_async);
    g_test_add_func("/io/save-then-load-roundtrip", test_save_then_load_roundtrip);
    g_test_add_func("/io/save-refuses-changed-file", test_save_refuses_changed_file);
    g_test_add_func("/io/save-with-matching-etag", test_save_with_matching_etag);
#ifndef G_OS_WIN32
    g_test_add_func("/io/save-through-symlink", test_save_through_symlink);
#endif
    g_test_add_func("/io/copy-name", test_copy_name);
    g_test_add_func("/io/same-file", test_same_file);
    return g_test_run();
}
