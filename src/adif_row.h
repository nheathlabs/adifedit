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

#include <glib-object.h>

G_BEGIN_DECLS

/*
 * AdifRow wraps a single ADIF record as a flat array of string values,
 * ordered to match the dynamically-discovered column list. This is the
 * GObject stored in the GListStore backing the GtkColumnView.
 */

#define ADIF_TYPE_ROW (adif_row_get_type())
G_DECLARE_FINAL_TYPE(AdifRow, adif_row, ADIF, ROW, GObject)

AdifRow *adif_row_new(void);

/* Appends the next value in column order. Pass NULL for a missing field;
 * it will be stored/returned as an empty string. */
void adif_row_add_value(AdifRow *self, const char *value);

/* Returns the value for the given column index, or "" if out of range
 * or unset. Never returns NULL. */
const char *adif_row_get_value(AdifRow *self, guint index);

/* Replaces the value at the given column index. The row is grown with
 * empty strings as needed so `index` is always in range afterwards.
 * Pass NULL to store an empty string. Rows carry no signals: the
 * document tracks edits (and the modified flag) itself. */
void adif_row_set_value(AdifRow *self, guint index, const char *value);

/* Flag the value at `index` as one the parser repaired - its declared
 * length didn't match the data (adif_record_is_repaired). The flag stays
 * for the row's lifetime, whatever is later written there: it says what
 * loading did, for the view to highlight. */
void adif_row_set_repaired(AdifRow *self, guint index);

/* TRUE if adif_row_set_repaired() flagged `index`. */
gboolean adif_row_is_repaired(AdifRow *self, guint index);

G_END_DECLS
