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
 * AdifTable core: build the GtkColumnView over an AdifDocument, keep its
 * columns in step with the document's schema, and size them to their
 * contents. The cell widgets and right-click menus are in
 * adif_table_view.c.
 */

#include "adif_table_priv.h"

#include "time_slice.h"

/* On load, each column is sized to its widest value, truncated to this
 * many characters so a stray long comment can't blow the layout out. */
#define ADIF_AUTOFIT_MAX_CHARS 24

/* Horizontal space a cell adds around its text (the editor's padding +
 * the column separator) plus a little slack, added to the measured
 * sample width. */
#define ADIF_CELL_CHROME_PX 32

/* ------------------------------------------------------------------- */
/* Walking the grid's columns and their realised cells                  */
/* ------------------------------------------------------------------- */

/* The grid column mirroring schema column `data_index`, or NULL.
 * Borrowed - the column view keeps the reference. */
static GtkColumnViewColumn *column_for_data_index(AdifTable *self, guint data_index) {
    GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(self->column_view));
    guint n = g_list_model_get_n_items(columns);
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *column = g_list_model_get_item(columns, i);
        g_object_unref(column); /* the model keeps it alive */
        if (adif_column_info(column)->data_index == data_index) {
            return column;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------- */
/* Column auto-fit                                                      */
/* ------------------------------------------------------------------- */

/* One remembered auto-fit result. The name is kept alongside the width
 * because data-indices restart from 0 when a file is loaded or the
 * document is reset. */
typedef struct {
    char *name; /* owned */
    int width;
} FitWidth;

static void fit_width_free(gpointer data) {
    FitWidth *fit = data;
    g_free(fit->name);
    g_free(fit);
}

/* The remembered width of column `data_index` named `name`, or -1. */
static int remembered_width(AdifTable *self, guint data_index, const char *name) {
    FitWidth *fit = g_hash_table_lookup(self->fit_widths, GUINT_TO_POINTER(data_index));
    return (fit != NULL && g_strcmp0(fit->name, name) == 0) ? fit->width : -1;
}

void adif_table_forget_width(AdifTable *self, guint data_index) {
    g_hash_table_remove(self->fit_widths, GUINT_TO_POINTER(data_index));
}

/* A layout in the *cells'* font, not the column view's - they can differ,
 * and the cell is what actually renders the text. Caller unrefs. */
static PangoLayout *create_cell_layout(AdifTable *self) {
    GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(self->column_view));
    guint n_cols = g_list_model_get_n_items(columns);
    GtkWidget *sample_cell = NULL;
    for (guint c = 0; c < n_cols && sample_cell == NULL; c++) {
        GtkColumnViewColumn *col = g_list_model_get_item(columns, c);
        GPtrArray *cells = adif_column_info(col)->cells;
        if (cells->len > 0) {
            sample_cell = adif_cell(g_ptr_array_index(cells, 0))->label;
        }
        g_object_unref(col);
    }
    return gtk_widget_create_pango_layout(sample_cell != NULL ? sample_cell : self->column_view,
                                          NULL);
}

/* Measure a throwaway editor like the one begin_edit() builds. */
static void measure_cell_editor(AdifTable *self) {
    GtkWidget *probe = g_object_ref_sink(gtk_entry_new());
    gtk_entry_set_has_frame(GTK_ENTRY(probe), FALSE);
    gtk_widget_add_css_class(probe, "adif-cell-editor");
    gtk_editable_set_width_chars(GTK_EDITABLE(probe), 0);
    int height = 0, width = 0;
    gtk_widget_measure(probe, GTK_ORIENTATION_VERTICAL, -1, NULL, &height, NULL, NULL);
    gtk_widget_measure(probe, GTK_ORIENTATION_HORIZONTAL, -1, &width, NULL, NULL, NULL);
    g_object_unref(probe);
    self->cell_height = MAX(height, 1);
    self->cell_inset = width / 2; /* its minimum when empty: just the side padding */
}

int adif_table_cell_height(AdifTable *self) {
    if (self->cell_height == 0) {
        measure_cell_editor(self);
    }
    return self->cell_height;
}

int adif_table_cell_inset(AdifTable *self) {
    if (self->cell_height == 0) {
        measure_cell_editor(self);
    }
    return self->cell_inset;
}

/* apply_column_widths()'s progress across idle re-entries. A fresh one
 * replaces any previous run whenever schedule_autofit() (re)starts -
 * which always happens right after a fresh sync_columns(), so a stale
 * step can never outlive the columns it was measuring. */
struct AdifWidthStep {
    guint next_column; /* index of the next column to (re)consider */
    /* Mid-column state, meaningful only between choosing a column and
     * finishing its second pass: */
    gboolean in_progress;
    int phase;       /* 0: counting chars, 1: measuring pixels */
    guint row;       /* next row to look at, in the current phase */
    glong max_chars; /* phase 0's running answer */
    int widest;      /* phase 1's running answer */
    PangoLayout *layout; /* the cell-font layout, created on first need, reused for the run */
    GHashTable *seen;    /* phase 1, this column only: owned char* -> its already-measured
                            pixel width, so a repeated value costs a hash lookup, not a
                            fresh Pango shape - a stamp or fill makes every candidate row
                            byte-identical, which would otherwise re-measure the same text
                            once per row */
};

static void width_step_free(AdifWidthStep *step) {
    g_clear_object(&step->layout);
    g_clear_pointer(&step->seen, g_hash_table_unref);
    g_free(step);
}

/* Take `column` up for fitting. TRUE if its remembered width was applied
 * instead, leaving nothing to measure. */
static gboolean start_column(AdifTable *self, AdifWidthStep *step, GtkColumnViewColumn *column) {
    const AdifColumnInfo *info = adif_column_info(column);
    int cached = remembered_width(self, info->data_index, info->name);
    if (cached >= 0) {
        gtk_column_view_column_set_fixed_width(column, cached);
        return TRUE;
    }
    if (step->layout == NULL) {
        step->layout = create_cell_layout(self);
    }
    step->in_progress = TRUE;
    step->phase = 0;
    step->row = 0;
    step->max_chars = (info->name != NULL) ? g_utf8_strlen(info->name, -1) : 0;
    step->widest = 0;
    return FALSE;
}

/* Phase 0: the longest value by character count - a cheap first cut so
 * phase 1 only pixel-measures the handful of candidates near that
 * length. Ends by measuring the header, which the column must fit too.
 * FALSE if the turn ran out first; call again next turn. */
static gboolean scan_longest(AdifTable *self, AdifWidthStep *step, const AdifColumnInfo *info,
                             guint *since_check, gint64 deadline) {
    guint n_rows = adif_document_get_n_rows(self->doc);
    while (step->row < n_rows) {
        const char *value = adif_document_get_value(self->doc, step->row, info->data_index);
        step->max_chars = MAX(step->max_chars, g_utf8_strlen(value, -1));
        step->row++;
        if (adif_slice_expired(since_check, deadline)) {
            return FALSE;
        }
    }
    step->max_chars = MIN(step->max_chars, ADIF_AUTOFIT_MAX_CHARS);
    if (info->name != NULL) {
        pango_layout_set_text(step->layout, info->name, -1);
        pango_layout_get_pixel_size(step->layout, &step->widest, NULL);
    }
    step->phase = 1;
    step->row = 0;
    step->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return TRUE;
}

/* `text`'s pixel width in the cell font, shaped once per distinct text
 * in the column (see AdifWidthStep.seen). */
static int text_width(AdifWidthStep *step, const char *text) {
    gpointer cached = NULL;
    if (g_hash_table_lookup_extended(step->seen, text, NULL, &cached)) {
        return GPOINTER_TO_INT(cached);
    }
    int width = 0;
    pango_layout_set_text(step->layout, text, -1);
    pango_layout_get_pixel_size(step->layout, &width, NULL);
    g_hash_table_insert(step->seen, g_strdup(text), GINT_TO_POINTER(width));
    return width;
}

/* Phase 1: pixel-measure the values within two characters of the
 * longest, as shown (truncated to ADIF_AUTOFIT_MAX_CHARS). FALSE if the
 * turn ran out first; call again next turn. */
static gboolean measure_candidates(AdifTable *self, AdifWidthStep *step, const AdifColumnInfo *info,
                                   guint *since_check, gint64 deadline) {
    guint n_rows = adif_document_get_n_rows(self->doc);
    while (step->row < n_rows) {
        const char *value = adif_document_get_value(self->doc, step->row, info->data_index);
        glong len = g_utf8_strlen(value, -1);
        if (value[0] != '\0' && len >= step->max_chars - 2) {
            char *shown = (len > ADIF_AUTOFIT_MAX_CHARS)
                              ? g_utf8_substring(value, 0, ADIF_AUTOFIT_MAX_CHARS)
                              : NULL;
            int width = text_width(step, shown != NULL ? shown : value);
            step->widest = MAX(step->widest, width);
            g_free(shown);
        }
        step->row++;
        if (adif_slice_expired(since_check, deadline)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Apply a fully measured column's width, and remember it. */
static void finish_column(AdifTable *self, AdifWidthStep *step, GtkColumnViewColumn *column) {
    const AdifColumnInfo *info = adif_column_info(column);
    int width = step->widest + ADIF_CELL_CHROME_PX;
    FitWidth *fit = g_new0(FitWidth, 1);
    fit->name = g_strdup(info->name);
    fit->width = width;
    g_hash_table_replace(self->fit_widths, GUINT_TO_POINTER(info->data_index), fit);
    gtk_column_view_column_set_fixed_width(column, width);
    g_clear_pointer(&step->seen, g_hash_table_unref);
    step->in_progress = FALSE;
}

/* Take one column's fit as far as this turn allows. TRUE once it's done. */
static gboolean fit_column(AdifTable *self, AdifWidthStep *step, GtkColumnViewColumn *column,
                           gint64 deadline) {
    if (!step->in_progress && start_column(self, step, column)) {
        return TRUE; /* remembered width: nothing to measure */
    }
    const AdifColumnInfo *info = adif_column_info(column);
    guint since_check = 0;
    if (step->phase == 0 && !scan_longest(self, step, info, &since_check, deadline)) {
        return FALSE;
    }
    if (!measure_candidates(self, step, info, &since_check, deadline)) {
        return FALSE;
    }
    finish_column(self, step, column);
    return TRUE;
}

/* Size each column to the widest text it actually shows, rendered in the
 * cell's font - character counts lie ("KJ5LMR" is far wider than six
 * average glyphs, "iiii" far narrower), each value truncated to
 * ADIF_AUTOFIT_MAX_CHARS so one long comment can't blow the layout out.
 * Run from an idle so the view's font/CSS is resolved first; measuring it
 * synchronously undershoots and clips. Columns stay resizable.
 *
 * Each measurement scans every row, so it is remembered (fit_widths) and
 * only columns that are new or whose values changed are measured again -
 * adding one column to a wide log doesn't re-scan all the others.
 *
 * Spread across turns: a big log's first load, or a stamp/fill that
 * touches a column with no cached width, would otherwise measure many
 * thousands of rows in one uninterrupted call and freeze the display for
 * it. Each turn measures up to ADIF_SLICE_BUDGET_US worth of rows,
 * then returns G_SOURCE_CONTINUE so the main loop gets a chance to paint
 * before the next turn - a plain repeating idle, not a frame-clock tick,
 * so it keeps making progress regardless of what else is or isn't being
 * drawn. */
static gboolean apply_column_widths(gpointer data) {
    AdifTable *self = data;
    AdifWidthStep *step = self->width_step;
    GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(self->column_view));
    guint n_cols = g_list_model_get_n_items(columns);
    gint64 deadline = adif_slice_deadline();

    while (step->next_column < n_cols) {
        GtkColumnViewColumn *column = g_list_model_get_item(columns, step->next_column);
        gboolean finished = fit_column(self, step, column, deadline);
        g_object_unref(column);
        if (!finished) {
            return G_SOURCE_CONTINUE; /* out of time mid-column: resume it next turn */
        }
        step->next_column++;
    }

    self->width_idle = 0;
    width_step_free(step);
    self->width_step = NULL;
    return G_SOURCE_REMOVE;
}

static void schedule_autofit(AdifTable *self) {
    if (self->width_idle != 0) {
        g_source_remove(self->width_idle);
        width_step_free(self->width_step);
        self->width_step = NULL;
    }
    self->width_step = g_new0(AdifWidthStep, 1);
    self->width_idle = g_idle_add(apply_column_widths, self);
}

/* ------------------------------------------------------------------- */
/* Schema mirroring                                                     */
/* ------------------------------------------------------------------- */

/* Remove the view's columns from position `from` on. */
static void clear_columns_from(GtkColumnView *column_view, guint from) {
    GListModel *columns = gtk_column_view_get_columns(column_view);
    while (g_list_model_get_n_items(columns) > from) {
        GtkColumnViewColumn *column = g_list_model_get_item(columns, from);
        adif_column_info(column)->table = NULL; /* its cells' late callbacks become no-ops */
        gtk_column_view_remove_column(column_view, column);
        g_object_unref(column);
    }
}

/* TRUE if the view's columns are already the document's schema, in order -
 * a "schema-changed" that changed nothing (e.g. the one a load emits once
 * its rows are in, to ask for a re-fit) needn't rebuild anything. */
static gboolean columns_match_schema(AdifTable *self) {
    GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(self->column_view));
    guint n = adif_document_get_n_columns(self->doc);
    if (g_list_model_get_n_items(columns) != n) {
        return FALSE;
    }
    gboolean match = TRUE;
    for (guint i = 0; i < n && match; i++) {
        GtkColumnViewColumn *column = g_list_model_get_item(columns, i);
        AdifColumnInfo *info = adif_column_info(column);
        match = info->data_index == adif_document_column_data_index_at(self->doc, i)
                && g_strcmp0(info->name, adif_document_column_name_at(self->doc, i)) == 0;
        g_object_unref(column);
    }
    return match;
}

/* TRUE if `column` mirrors the document column `name` at `data_index`. */
static gboolean column_is(GtkColumnViewColumn *column, guint data_index, const char *name) {
    const AdifColumnInfo *info = adif_column_info(column);
    return info->data_index == data_index && g_strcmp0(info->name, name) == 0;
}

/* Where in the view, at or after `from`, the column mirroring
 * (`data_index`, `name`) is; G_MAXUINT if it has none. */
static guint find_column_from(GListModel *columns, guint from, guint data_index, const char *name) {
    guint n = g_list_model_get_n_items(columns);
    for (guint i = from; i < n; i++) {
        GtkColumnViewColumn *column = g_list_model_get_item(columns, i);
        gboolean found = column_is(column, data_index, name);
        g_object_unref(column);
        if (found) {
            return i;
        }
    }
    return G_MAXUINT;
}

/* Drop the view columns whose document column is gone (deleted, or the
 * whole schema replaced by a load). */
static void remove_vanished_columns(AdifTable *self) {
    GtkColumnView *column_view = GTK_COLUMN_VIEW(self->column_view);
    GListModel *columns = gtk_column_view_get_columns(column_view);
    for (guint i = g_list_model_get_n_items(columns); i-- > 0;) {
        GtkColumnViewColumn *column = g_list_model_get_item(columns, i);
        AdifColumnInfo *info = adif_column_info(column);
        if (g_strcmp0(adif_document_column_name(self->doc, info->data_index), info->name) != 0) {
            info->table = NULL; /* its cells' late callbacks become no-ops */
            gtk_column_view_remove_column(column_view, column);
        }
        g_object_unref(column);
    }
}

/* Bring the view's columns in line with the document's schema, touching
 * only what changed: an insert adds one column and a delete removes one,
 * while every other column - and the cell that has the focus, which
 * would otherwise take the scroll position with it - stays put. */
static void sync_columns(AdifTable *self) {
    GtkColumnView *column_view = GTK_COLUMN_VIEW(self->column_view);
    GListModel *columns = gtk_column_view_get_columns(column_view);
    remove_vanished_columns(self);

    guint n = adif_document_get_n_columns(self->doc);
    for (guint i = 0; i < n; i++) {
        const char *name = adif_document_column_name_at(self->doc, i);
        guint di = adif_document_column_data_index_at(self->doc, i);
        guint at = find_column_from(columns, i, di, name);
        if (at == i) {
            continue;
        }
        GtkColumnViewColumn *column;
        if (at != G_MAXUINT) {
            column = g_list_model_get_item(columns, at); /* reordered: move it */
        } else {
            column = adif_column_new(self, name, di);
            /* An unchanged column keeps its width straight away rather
             * than waiting for the auto-fit idle. */
            int width = remembered_width(self, di, name);
            if (width >= 0) {
                gtk_column_view_column_set_fixed_width(column, width);
            }
        }
        gtk_column_view_insert_column(column_view, i, column);
        g_object_unref(column);
    }
    clear_columns_from(column_view, n); /* nothing should be left over, but never leave a stray */
}

static void on_schema_changed(G_GNUC_UNUSED AdifDocument *doc, gpointer user_data) {
    AdifTable *self = user_data;
    if (columns_match_schema(self)) {
        /* The rows may still be new (a reload of the same file), so the
         * borrowed match_row can't be trusted - clear it and its styling. */
        adif_table_clear_match(self);
    } else {
        self->match_row = NULL; /* column set changed - any search match is stale */
        sync_columns(self);
    }
    schedule_autofit(self);
}

/* ------------------------------------------------------------------- */
/* Search-match highlight                                               */
/* ------------------------------------------------------------------- */

#define ADIF_MATCH_CSS_CLASS "adif-search-match"

void adif_table_apply_match_class(AdifTable *self, GtkWidget *widget, AdifRow *row,
                                  guint data_index) {
    gboolean is_match = self->match_row != NULL && row == self->match_row
                        && data_index == self->match_di;
    if (is_match) {
        gtk_widget_add_css_class(widget, ADIF_MATCH_CSS_CLASS);
    } else {
        gtk_widget_remove_css_class(widget, ADIF_MATCH_CSS_CLASS);
    }
}

/* Re-evaluate the highlight on every cell currently on screen. */
static void refresh_match_class(AdifTable *self) {
    GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(self->column_view));
    guint n = g_list_model_get_n_items(columns);
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *column = g_list_model_get_item(columns, i);
        g_object_unref(column);
        AdifColumnInfo *info = adif_column_info(column);
        for (guint c = 0; c < info->cells->len; c++) {
            GtkWidget *widget = g_ptr_array_index(info->cells, c);
            adif_table_apply_match_class(self, widget, adif_cell(widget)->row, info->data_index);
        }
    }
}

void adif_table_go_to_match(AdifTable *self, AdifCellRef cell) {
    g_return_if_fail(self != NULL);
    self->match_row = adif_document_get_row(self->doc, cell.row);
    self->match_di = cell.data_index;
    if (self->match_row == NULL) {
        return;
    }

    GtkColumnViewColumn *target = column_for_data_index(self, cell.data_index); /* borrowed */
    gtk_column_view_scroll_to(GTK_COLUMN_VIEW(self->column_view), cell.row, target,
                              GTK_LIST_SCROLL_SELECT, NULL);
    self->current_di = cell.data_index;
    refresh_match_class(self);
}

void adif_table_clear_match(AdifTable *self) {
    g_return_if_fail(self != NULL);
    self->match_row = NULL;
    refresh_match_class(self);
}

/* Values in one column changed in place (fill, stamp, replace, undo/redo
 * of a cell edit). Push the new text into the cells currently on screen (off-screen ones re-read it
 * from the document when they next bind). */
static void on_column_values_changed(G_GNUC_UNUSED AdifDocument *doc, guint data_index,
                                     gpointer user_data) {
    AdifTable *self = user_data;
    adif_table_forget_width(self, data_index);
    GtkColumnViewColumn *column = column_for_data_index(self, data_index);
    if (column == NULL) {
        return;
    }
    GPtrArray *cells = adif_column_info(column)->cells;
    for (guint c = 0; c < cells->len; c++) {
        GtkWidget *widget = g_ptr_array_index(cells, c);
        AdifRow *row = adif_cell(widget)->row;
        if (row != NULL) {
            adif_cell_set_text_silently(widget, adif_row_get_value(row, data_index));
        }
    }
}

/* TRUE if rows [position, position + n) have no value in any column. */
static gboolean rows_are_blank(AdifTable *self, GListModel *rows, guint position, guint n) {
    guint n_cols = adif_document_get_n_columns(self->doc);
    for (guint r = position; r < position + n; r++) {
        AdifRow *row = g_list_model_get_item(rows, r);
        gboolean blank = TRUE;
        for (guint c = 0; c < n_cols && blank; c++) {
            guint di = adif_document_column_data_index_at(self->doc, c);
            blank = adif_row_get_value(row, di)[0] == '\0';
        }
        g_object_unref(row);
        if (!blank) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Rows added or removed (including a load or reset, which replace them
 * all) can change any column's widest value - except inserting blank
 * rows (Insert Row, the common case), which can't widen anything, so the
 * remembered widths stay and the next auto-fit needn't re-measure every
 * column. */
static void on_rows_changed(GListModel *rows, guint position, guint removed, guint added,
                            gpointer user_data) {
    AdifTable *self = user_data;
    if (removed == 0 && rows_are_blank(self, rows, position, added)) {
        return;
    }
    g_hash_table_remove_all(self->fit_widths);
}

/* ------------------------------------------------------------------- */
/* Construction                                                         */
/* ------------------------------------------------------------------- */

AdifTable *adif_table_new(AdifDocument *doc) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(doc), NULL);

    AdifTable *self = g_new0(AdifTable, 1);
    self->doc = doc;
    self->current_di = G_MAXUINT;
    self->fit_widths = g_hash_table_new_full(NULL, NULL, NULL, fit_width_free);

    /* The selection model takes ownership of the ref we pass, so ref the
     * document's rows model for it; the document keeps its own ref. */
    GtkSingleSelection *selection = gtk_single_selection_new(
        g_object_ref(adif_document_get_rows(doc)));
    self->column_view = gtk_column_view_new(GTK_SELECTION_MODEL(selection));
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(self->column_view), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(self->column_view), TRUE);

    /* One shared cell menu, parented to the data view; its model is
     * rebuilt per click to carry the target row/column. Unparent it
     * before the column view finalises to avoid a leftover-child warning. */
    self->cell_menu = gtk_popover_menu_new_from_model(NULL);
    gtk_popover_set_has_arrow(GTK_POPOVER(self->cell_menu), FALSE);
    gtk_widget_set_halign(self->cell_menu, GTK_ALIGN_START);
    gtk_widget_set_parent(self->cell_menu, self->column_view);
    g_signal_connect_swapped(self->column_view, "destroy", G_CALLBACK(gtk_widget_unparent),
                             self->cell_menu);

    g_signal_connect(doc, "schema-changed", G_CALLBACK(on_schema_changed), self);
    g_signal_connect(doc, "column-values-changed", G_CALLBACK(on_column_values_changed), self);
    g_signal_connect(adif_document_get_rows(doc), "items-changed", G_CALLBACK(on_rows_changed),
                     self);
    sync_columns(self);
    schedule_autofit(self);

    return self;
}

void adif_table_free(AdifTable *self) {
    if (self == NULL) {
        return;
    }
    if (self->width_idle != 0) {
        g_source_remove(self->width_idle);
        width_step_free(self->width_step);
    }
    g_signal_handlers_disconnect_by_data(self->doc, self);
    g_signal_handlers_disconnect_by_data(adif_document_get_rows(self->doc), self);
    g_hash_table_destroy(self->fit_widths);
    /* The columns outlive the table (the widget tree tears them down
     * later): unhook them so their cells' callbacks can't reach it. */
    GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(self->column_view));
    for (guint i = 0; i < g_list_model_get_n_items(columns); i++) {
        GtkColumnViewColumn *column = g_list_model_get_item(columns, i);
        adif_column_info(column)->table = NULL;
        g_object_unref(column);
    }
    /* The column view and everything under it belong to the widget tree
     * and are torn down with it; the selection model owned the row-model
     * ref we handed it. Nothing to release here. */
    g_free(self);
}

GtkWidget *adif_table_get_widget(AdifTable *self) {
    return self->column_view;
}

guint adif_table_selected_row(AdifTable *self) {
    GtkSelectionModel *model = gtk_column_view_get_model(GTK_COLUMN_VIEW(self->column_view));
    return gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(model));
}

guint adif_table_current_column(AdifTable *self) {
    guint n = adif_document_get_n_columns(self->doc);
    if (n == 0) {
        return GTK_INVALID_LIST_POSITION;
    }
    guint pos = adif_document_column_position(self->doc, self->current_di);
    return (pos < n) ? pos : 0;
}
