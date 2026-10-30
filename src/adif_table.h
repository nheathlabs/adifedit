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

#include <gtk/gtk.h>

#include "adif_document.h"

G_BEGIN_DECLS

/*
 * AdifTable is the view: a GtkColumnView that renders an AdifDocument as
 * an editable, spreadsheet-like grid. It owns no data - it mirrors the
 * document's schema into columns, writes cell edits back into the
 * document's rows, and re-reads the schema whenever the document's
 * "schema-changed" fires. All row/column operations live on AdifDocument;
 * the window's actions call them directly.
 *
 * The document must outlive the table.
 */

typedef struct _AdifTable AdifTable;

AdifTable *adif_table_new(AdifDocument *doc);
void adif_table_free(AdifTable *self);

/* The GtkColumnView widget; owned by the table (put it in a container). */
GtkWidget *adif_table_get_widget(AdifTable *self);

/* The current cell, for the toolbar's "insert relative to cursor" buttons:
 *   selected_row    - the highlighted row, or GTK_INVALID_LIST_POSITION.
 *   current_column  - visual position of the column whose cell was last
 *                     focused (0 if none yet); GTK_INVALID_LIST_POSITION
 *                     when the grid has no columns. */
guint adif_table_selected_row(AdifTable *self);
guint adif_table_current_column(AdifTable *self);

/* Search support (driven by window_search.c).
 *   go_to_match  - scroll `cell` into view, select its row, and mark the
 *                  cell as the current match (a highlight the cell keeps
 *                  as it is scrolled off and back).
 *   clear_match  - drop that highlight. */
void adif_table_go_to_match(AdifTable *self, AdifCellRef cell);
void adif_table_clear_match(AdifTable *self);

G_END_DECLS
