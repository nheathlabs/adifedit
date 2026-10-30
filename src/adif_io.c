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

#include "adif_io.h"

#include <string.h>

#include <glib/gstdio.h>

/* ------------------------------------------------------------------- */
/* Load                                                                 */
/* ------------------------------------------------------------------- */

/* What a load hands back: the parse, and the file's etag as it was read. */
typedef struct {
    AdifParseResult *result;
    char *etag;
} LoadData;

static void load_data_free(gpointer data) {
    LoadData *load = data;
    if (load->result != NULL) {
        adif_parse_result_free(load->result);
    }
    g_free(load->etag);
    g_free(load);
}

/* The file's etag (its modification time, for a local file), or NULL if
 * it can't be read. Caller frees. */
static char *query_etag(const char *path) {
    GFile *file = g_file_new_for_path(path);
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_ETAG_VALUE, G_FILE_QUERY_INFO_NONE,
                                        NULL, NULL);
    char *etag = NULL;
    if (info != NULL) {
        etag = g_strdup(g_file_info_get_etag(info));
        g_object_unref(info);
    }
    g_object_unref(file);
    return etag;
}

static void load_thread_func(GTask *task, G_GNUC_UNUSED gpointer source_object, gpointer task_data,
                             G_GNUC_UNUSED GCancellable *cancellable) {
    const char *path = task_data;

    /* Taken before reading: if the file changes in between, the later
     * save sees a mismatch and asks, rather than overwriting the change. */
    char *etag = query_etag(path);
    GError *error = NULL;
    AdifParseResult *result = adif_parse_file(path, &error);
    if (result == NULL) {
        g_free(etag);
        g_task_return_error(task, error);
        return;
    }
    LoadData *load = g_new0(LoadData, 1);
    load->result = result;
    load->etag = etag;
    g_task_return_pointer(task, load, load_data_free);
}

void adif_io_load_async(const char *path, GAsyncReadyCallback callback, gpointer user_data) {
    g_return_if_fail(path != NULL);

    GTask *task = g_task_new(NULL, NULL, callback, user_data);
    g_task_set_task_data(task, g_strdup(path), g_free);
    g_task_run_in_thread(task, load_thread_func);
    g_object_unref(task);
}

AdifParseResult *adif_io_load_finish(GAsyncResult *result, char **etag, GError **error) {
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    LoadData *load = g_task_propagate_pointer(G_TASK(result), error);
    if (load == NULL) {
        return NULL;
    }
    AdifParseResult *parsed = g_steal_pointer(&load->result);
    if (etag != NULL) {
        *etag = g_steal_pointer(&load->etag);
    }
    load_data_free(load);
    return parsed;
}

/* ------------------------------------------------------------------- */
/* Save                                                                 */
/* ------------------------------------------------------------------- */

typedef struct {
    char *path;
    char *contents;
    char *etag; /* the file must still have this etag, or NULL for no check */
} SaveData;

static void save_data_free(gpointer data) {
    SaveData *save = data;
    g_free(save->path);
    g_free(save->contents);
    g_free(save->etag);
    g_free(save);
}

static void save_thread_func(GTask *task, G_GNUC_UNUSED gpointer source_object, gpointer task_data,
                             G_GNUC_UNUSED GCancellable *cancellable) {
    SaveData *save = task_data;

    /* Written to a temporary file and renamed over the original, like
     * g_file_set_contents(), but this follows a symlink to the real file
     * and keeps the original's permissions, rather than replacing either
     * with a fresh regular file. */
    GFile *file = g_file_new_for_path(save->path);
    GError *error = NULL;
    char *new_etag = NULL;
    gboolean ok = g_file_replace_contents(file, save->contents, strlen(save->contents), save->etag,
                                          FALSE, G_FILE_CREATE_NONE, &new_etag, NULL, &error);
    g_object_unref(file);
    if (!ok) {
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, new_etag, g_free);
}

void adif_io_save_async(const char *path, char *contents, const char *etag,
                        GAsyncReadyCallback callback, gpointer user_data) {
    g_return_if_fail(path != NULL);

    SaveData *save = g_new0(SaveData, 1);
    save->path = g_strdup(path);
    save->contents = contents; /* takes ownership */
    save->etag = g_strdup(etag);

    GTask *task = g_task_new(NULL, NULL, callback, user_data);
    g_task_set_task_data(task, save, save_data_free);
    g_task_run_in_thread(task, save_thread_func);
    g_object_unref(task);
}

gboolean adif_io_save_finish(GAsyncResult *result, char **new_etag, GError **error) {
    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
    GError *local = NULL;
    char *etag = g_task_propagate_pointer(G_TASK(result), &local);
    if (local != NULL) {
        g_propagate_error(error, local);
        return FALSE;
    }
    if (new_etag != NULL) {
        *new_etag = etag;
    } else {
        g_free(etag);
    }
    return TRUE;
}

/* ------------------------------------------------------------------- */
/* File names                                                           */
/* ------------------------------------------------------------------- */

char *adif_io_copy_name(const char *basename) {
    g_return_val_if_fail(basename != NULL, NULL);

    const char *dot = strrchr(basename, '.');
    if (dot == NULL || dot == basename) { /* no extension, or a dotfile */
        return g_strconcat(basename, "-adifedit.adi", NULL);
    }
    return g_strdup_printf("%.*s-adifedit%s", (int)(dot - basename), basename, dot);
}

gboolean adif_io_same_file(const char *a, const char *b) {
    g_return_val_if_fail(a != NULL && b != NULL, FALSE);

    /* Windows' stat() leaves st_ino 0 for every file, so only trust the
     * inode where there is one. */
    GStatBuf sa, sb;
    if (g_stat(a, &sa) == 0 && g_stat(b, &sb) == 0 && sa.st_ino != 0) {
        return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    }
    GFile *fa = g_file_new_for_path(a);
    GFile *fb = g_file_new_for_path(b);
    gboolean same = g_file_equal(fa, fb);
    g_object_unref(fa);
    g_object_unref(fb);
    return same;
}
