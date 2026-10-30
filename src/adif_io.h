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

#include <gio/gio.h>

#include "adif_parser.h"

G_BEGIN_DECLS

/*
 * Thin asynchronous wrappers around the (blocking) ADIF parser and a
 * plain file write. Both run their work on a thread from the GLib pool
 * and report back on the caller's main context, so the UI stays
 * responsive during large loads and saves.
 */

/* Parse `path` off the main thread. Retrieve the result in `callback`
 * with adif_io_load_finish(). */
void adif_io_load_async(const char *path, GAsyncReadyCallback callback, gpointer user_data);

/* Returns the parse result (caller frees with adif_parse_result_free) or
 * NULL with `error` set. `etag` (may be NULL) receives the file's etag
 * as it was before reading - its identity for a later save's
 * changed-on-disk check - or NULL if it couldn't be read; caller frees. */
AdifParseResult *adif_io_load_finish(GAsyncResult *result, char **etag, GError **error);

/* Write `contents` to `path` off the main thread. Ownership of
 * `contents` transfers to this call. The write goes to a temporary file
 * renamed over `path`, following a symlink and keeping the file's
 * permissions. With a non-NULL `etag` (from a load or an earlier save)
 * it is refused - G_IO_ERROR_WRONG_ETAG, nothing written - if the file
 * has changed since. Finish with adif_io_save_finish(). */
void adif_io_save_async(const char *path, char *contents, const char *etag,
                        GAsyncReadyCallback callback, gpointer user_data);

/* TRUE once saved, with `new_etag` (may be NULL) set to the written
 * file's etag (caller frees); FALSE with `error` set otherwise. */
gboolean adif_io_save_finish(GAsyncResult *result, char **new_etag, GError **error);

/* The file name Save As suggests for a copy of `basename`: "-adifedit"
 * goes before the extension ("log.adi" -> "log-adifedit.adi"); a name
 * with no extension gets ".adi" ("log" -> "log-adifedit.adi"). Caller
 * frees. */
char *adif_io_copy_name(const char *basename);

/* TRUE if `a` and `b` name the same file - compared by device and inode
 * when both exist (POSIX), so a symlink or "./" spelling of a path still
 * matches; otherwise by GFile path equality. */
gboolean adif_io_same_file(const char *a, const char *b);

G_END_DECLS
