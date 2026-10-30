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

/*
 * Internal glue shared between the AdifTable implementation files:
 *   adif_table.c       - construction, schema mirroring, column auto-fit
 *   adif_table_view.c  - the cell widgets and the right-click menus
 *
 * Not a public header - only those two .c files include it.
 */
#pragma once

#include "adif_table.h"

G_BEGIN_DECLS

typedef struct AdifWidthStep AdifWidthStep; /* defined in adif_table.c */

struct _AdifTable {
    AdifDocument *doc;        /* borrowed; the window owns it */
    GtkWidget *column_view;   /* GtkColumnView, owned by the widget tree */
    GtkWidget *cell_menu;     /* right-click popover, parented to column_view */
    guint current_di;         /* data-index of the last-focused cell's column,
                                 or G_MAXUINT before any cell is focused */
    guint width_idle;         /* pending/in-progress "size columns to content" idle, or 0 */
    AdifWidthStep *width_step; /* its progress while width_idle != 0, else NULL */
    GHashTable *fit_widths;   /* data-index -> owned FitWidth: the last
                                 auto-fit result per column, so a rebuild
                                 only measures columns that are new or
                                 whose values changed since */

    int cell_height;          /* adif_table_cell_height()'s cached answer, or 0 */
    int cell_inset;           /* adif_table_cell_inset()'s cached answer */

    AdifRow *match_row;       /* borrowed: the row of the current search match, or NULL */
    guint match_di;           /* its column's data-index (valid when match_row != NULL) */
};

/* Column `data_index`'s content changed behind the document's signals
 * (a live cell edit): re-measure it on the next auto-fit. */
void adif_table_forget_width(AdifTable *self, guint data_index);

/* A cell editor's natural height, and how far in from its left edge its
 * text starts - measured once from a probe entry, so the cells can match
 * the editor that replaces them while editing. */
int adif_table_cell_height(AdifTable *self);
int adif_table_cell_inset(AdifTable *self);

/* --- adif_table_view.c ---------------------------------------------- */

/*
 * Each grid column mirrors one document column. Its identity, and the
 * cell widgets it currently has set up, hang off the GtkColumnViewColumn
 * as one AdifColumnInfo; each of those cell widgets carries an AdifCell.
 */
typedef struct {
    AdifTable *table; /* borrowed; NULL once the column leaves the view or
                         the table is freed, which makes any late cell
                         callback during teardown a no-op */
    char *name;       /* owned ADIF field name */
    guint data_index; /* the document's stable slot for this column */
    GPtrArray *cells; /* GtkWidget* cell boxes currently set up (not owned), so
                         the auto-fit pass can measure text in the real cell
                         font and in-place refreshes can reach them */
} AdifColumnInfo;

typedef struct {
    AdifColumnInfo *info; /* a reference: the column this cell belongs to */
    AdifRow *row;         /* borrowed while bound, else NULL */
    GtkWidget *box;       /* the cell widget (the list item's child) */
    GtkWidget *label;     /* GtkLabel showing the value; hidden while editing */
    GtkWidget *editor;    /* GtkEntry while the cell is being edited, else NULL */
    char *edit_original;  /* owned: the value when editing began (for Escape) */
    gulong changed_handler; /* the editor's "changed" handler */
} AdifCell;

/* Build (but do not add) an editable column for `table`, mirroring the
 * document column `data_index`, named `fieldname`. Returns one owned
 * reference. */
GtkColumnViewColumn *adif_column_new(AdifTable *table, const char *fieldname, guint data_index);

/* The AdifColumnInfo of a column built by adif_column_new(). */
AdifColumnInfo *adif_column_info(GtkColumnViewColumn *column);

/* The AdifCell of a cell widget. */
AdifCell *adif_cell(GtkWidget *widget);

/* Show new text in a bound cell (and its editor, if open) without it
 * counting as a user edit. */
void adif_cell_set_text_silently(GtkWidget *widget, const char *text);

/* Add or remove the "current search match" CSS class on cell `widget`, given
 * the row and column it is bound to. Called from bind and whenever the
 * match moves. */
void adif_table_apply_match_class(AdifTable *self, GtkWidget *widget, AdifRow *row,
                                  guint data_index);

G_END_DECLS
