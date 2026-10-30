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

#include "batch_stamp.h"

#include <string.h>

#include <glib/gstdio.h>

#include "adif_field.h"

/* clang-format off */  /* the hyphenated quark name is one token */
G_DEFINE_QUARK(adif-stamp-error, adif_stamp_error)
/* clang-format on */

void adif_stamp_free(AdifStamp *stamp) {
    if (stamp == NULL) {
        return;
    }
    g_free(stamp->id);
    g_free(stamp->path);
    g_free(stamp->title);
    g_clear_pointer(&stamp->fields, g_ptr_array_unref);
    g_clear_pointer(&stamp->bad, g_ptr_array_unref);
    g_free(stamp);
}

/* Why `raw` isn't a usable ADIF field name (a static phrase for the UI). */
static const char *field_problem(const char *raw) {
    switch (adif_field_name_check(raw)) {
        case ADIF_FIELD_NAME_EMPTY:
            return "the field name is empty";
        case ADIF_FIELD_NAME_HAS_SPACE:
            return "field names can't contain spaces";
        case ADIF_FIELD_NAME_BAD_START:
            return "must start with a letter or underscore";
        case ADIF_FIELD_NAME_RESERVED:
            return "EOR and EOH are reserved";
        case ADIF_FIELD_NAME_INVALID_CHAR:
        case ADIF_FIELD_NAME_OK:
            break;
    }
    return "only letters, digits and underscores are allowed";
}

/* One non-blank, non-comment line after the title: a field name, which
 * joins the stamp's fields (once) or, if it isn't a valid ADIF name, its
 * `bad` list. `seen` borrows the fields' strings. */
static void stamp_add_line(AdifStamp *stamp, GHashTable *seen, const char *line, guint lineno) {
    char *up = g_ascii_strup(line, -1);
    if (!adif_field_name_is_valid(up)) {
        g_ptr_array_add(stamp->bad,
                        g_strdup_printf("line %u: \"%s\" - %s", lineno, line, field_problem(line)));
        g_free(up);
    } else if (g_hash_table_contains(seen, up)) {
        g_free(up); /* duplicate field - ignore silently */
    } else {
        g_hash_table_add(seen, up);
        g_ptr_array_add(stamp->fields, up); /* transfer; seen keeps a borrowed pointer */
    }
}

/* The stamp's id: its file name without the .bst extension (caller frees). */
static char *stamp_id_from_path(const char *path) {
    char *id = g_path_get_basename(path);
    if (g_str_has_suffix(id, ".bst") && strlen(id) > 4) {
        id[strlen(id) - 4] = '\0';
    }
    return id;
}

/* An error for a stamp that parsed but can't be used, else FALSE. */
static gboolean stamp_is_unusable(const AdifStamp *stamp, GError **error) {
    if (stamp->title == NULL) {
        g_set_error_literal(error, ADIF_STAMP_ERROR, ADIF_STAMP_ERROR_NO_TITLE,
                            "The stamp file has no name on its first line.");
        return TRUE;
    }
    if (stamp->fields->len == 0 && stamp->bad->len == 0) {
        g_set_error_literal(error, ADIF_STAMP_ERROR, ADIF_STAMP_ERROR_NO_FIELDS,
                            "The stamp file lists no fields.");
        return TRUE;
    }
    return FALSE;
}

AdifStamp *adif_stamp_load(const char *path, GError **error) {
    g_return_val_if_fail(path != NULL, NULL);

    char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, error)) {
        return NULL;
    }
    char **lines = g_strsplit(contents, "\n", -1);
    g_free(contents);

    AdifStamp *stamp = g_new0(AdifStamp, 1);
    stamp->path = g_strdup(path);
    stamp->id = stamp_id_from_path(path);
    stamp->fields = g_ptr_array_new_with_free_func(g_free);
    stamp->bad = g_ptr_array_new_with_free_func(g_free);

    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal); /* borrows fields' entries */
    guint lineno = 0;
    for (char **l = lines; *l != NULL; l++) {
        lineno++;
        char *line = g_strstrip(*l);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        if (stamp->title == NULL) {
            stamp->title = g_strdup(line); /* the first line is the stamp's name */
        } else {
            stamp_add_line(stamp, seen, line, lineno);
        }
    }
    g_hash_table_unref(seen);
    g_strfreev(lines);

    if (stamp_is_unusable(stamp, error)) {
        adif_stamp_free(stamp);
        return NULL;
    }
    return stamp;
}

/* ------------------------------------------------------------------- */
/* Discovery                                                            */
/* ------------------------------------------------------------------- */

static void scan_dir(const char *dir, GHashTable *by_id) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d == NULL) {
        return;
    }
    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (!g_str_has_suffix(name, ".bst")) {
            continue;
        }
        char *path = g_build_filename(dir, name, NULL);
        AdifStamp *s = adif_stamp_load(path, NULL);
        g_free(path);
        if (s == NULL) {
            continue;
        }
        AdifStamp *old = g_hash_table_lookup(by_id, s->id);
        if (old != NULL) {
            adif_stamp_free(old); /* value_destroy is NULL; replace it by hand */
        }
        g_hash_table_replace(by_id, g_strdup(s->id), s);
    }
    g_dir_close(d);
}

static int cmp_stamp_title(gconstpointer a, gconstpointer b) {
    const AdifStamp *sa = *(const AdifStamp *const *)a;
    const AdifStamp *sb = *(const AdifStamp *const *)b;
    return g_ascii_strcasecmp(sa->title, sb->title);
}

GPtrArray *adif_stamp_list(void) {
    GHashTable *by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (const char *const *sd = g_get_system_data_dirs(); *sd != NULL; sd++) {
        char *dir = g_build_filename(*sd, "adifedit", "stamps", NULL);
        scan_dir(dir, by_id);
        g_free(dir);
    }
    char *udir = g_build_filename(g_get_user_data_dir(), "adifedit", "stamps", NULL);
    scan_dir(udir, by_id); /* scanned last -> a same-id user stamp wins */
    g_free(udir);

    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)adif_stamp_free);
    GList *values = g_hash_table_get_values(by_id);
    for (GList *v = values; v != NULL; v = v->next) {
        g_ptr_array_add(out, v->data); /* ownership moves to `out` */
    }
    g_list_free(values);
    g_hash_table_destroy(by_id); /* frees the id keys; values were moved out */

    g_ptr_array_sort(out, cmp_stamp_title);
    return out;
}

void adif_stamp_list_free(GPtrArray *stamps) {
    g_clear_pointer(&stamps, g_ptr_array_unref);
}

char *adif_stamp_user_dir(void) {
    char *dir = g_build_filename(g_get_user_data_dir(), "adifedit", "stamps", NULL);
    g_mkdir_with_parents(dir, 0755);
    return dir;
}
