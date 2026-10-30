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
#include "adif_row.h"

G_BEGIN_DECLS

/*
 * AdifDocument is the headless model behind the editor: the field list
 * (schema) plus the records, an unsaved-changes flag, and an undo/redo
 * history. No widget in sight - AdifTable renders it; the window's
 * actions mutate it through the functions below. It is deliberately
 * GTK-free so it can be unit-tested without a display (see tests/).
 *
 * Schema
 * ------
 * Each column has a name (upper-cased ADIF field name) and a stable
 * data-index - its slot in every row's value array. The data-index is
 * handed out once and never reused, so inserting or deleting a column
 * only reshuffles the visible order; the other columns' values stay put.
 * A deleted column's values stay in the rows (abandoned), which is what
 * lets undo restore a deleted column for free.
 *
 * Undo / redo
 * -----------
 * Every mutating call below is one undo step, except live cell editing:
 * successive adif_document_edit_cell() calls on the same cell collapse
 * into one step, closed by adif_document_commit_cell_edit().
 *
 * Signals
 * -------
 *   "schema-changed"    (void)  - columns added, removed, or reloaded.
 *                                 Inside a group (and while one is undone
 *                                 or redone) it fires once, at the end.
 *   "modified-changed"  (void)  - the unsaved-changes flag flipped.
 *   "history-changed"   (void)  - the undo or redo availability changed.
 *   "column-values-changed" (guint data_index) - values in one column
 *                                 changed in place (fill, set_value,
 *                                 replace, or an undo/redo of a cell
 *                                 edit); the view refreshes that column.
 *                                 Live edits (edit_cell) don't emit it -
 *                                 they come from the view.
 * Row insertions and deletions are observed through the GListModel
 * returned by adif_document_get_rows() ("items-changed").
 */

#define ADIF_TYPE_DOCUMENT (adif_document_get_type())
G_DECLARE_FINAL_TYPE(AdifDocument, adif_document, ADIF, DOCUMENT, GObject)

AdifDocument *adif_document_new(void);

/* Replace the entire schema and all rows from a fresh parse. Clears the
 * modified flag and the undo history, and emits "schema-changed".
 * Synchronous convenience (tests, small callers); the app uses the
 * incremental load_parse_begin() below. */
void adif_document_load_parse(AdifDocument *self, const AdifParseResult *result);

/* Incremental form of load_parse(), for a caller with its own main-loop
 * turn (e.g. an idle source) that wants to spread building a big file's
 * rows over several turns instead of blocking for the whole thing.
 *
 * Unlike column_write_begin()/step()/finish() below, whose effect on the
 * document only becomes visible at finish(), this one fires
 * "schema-changed" immediately from begin() - once the columns exist,
 * before any row does - so a view can show the (empty) grid right away.
 * Each step() then splices its rows straight into the document, which
 * fires the rows model's own "items-changed" same as any other row
 * insert, so a view fills in as loading progresses. finish() sets the
 * modified flag and fires "schema-changed" a second time, once every row
 * exists, so a view can re-fit columns to their real content instead of
 * just the headers it fit to at begin() - that repeat emission is the one
 * difference from load_parse().
 *
 * Usage: begin() once; step() repeatedly - each call writes rows until
 * `deadline` (a g_get_monotonic_time() timestamp) or the file ends,
 * returning TRUE if rows remain; once it returns FALSE, call finish().
 * If the document is going away before that, call cancel() instead - it
 * just drops the in-progress state; whatever rows had already been
 * spliced in stay as they are (harmless since the document is being
 * discarded anyway).
 *
 * begin() borrows `result`, like load_parse() - the caller must keep it
 * alive until finish() or cancel(). `self` must outlive the returned
 * step. */
typedef struct AdifLoadStep AdifLoadStep;
AdifLoadStep *adif_document_load_parse_begin(AdifDocument *self, const AdifParseResult *result);
gboolean adif_document_load_parse_step(AdifLoadStep *step, gint64 deadline);
void adif_document_load_parse_finish(AdifLoadStep *step);
void adif_document_load_parse_cancel(AdifLoadStep *step);

/* Empty the document back to its just-created state - no columns, no
 * rows, no history, not modified. Emits "schema-changed". */
void adif_document_reset(AdifDocument *self);

/* Serialise the current (edited) content to ADIF text. Empty cells are
 * skipped; a row with no non-empty cell is omitted entirely. A column's
 * type indicator from the loaded file is written with each of its fields
 * (<NAME:LEN:T>).
 *
 * `program_version` and `program_hash` (borrowed, may be NULL) go into
 * the header as <PROGRAMVERSION> and <APP_ADIFEDIT_PROGRAMHASH>; a NULL
 * omits that line. If the loaded file named the program that created it,
 * that identity is preserved as <APP_ADIFEDIT_ORIG_PROG> - captured once
 * on load and never overwritten on re-save. The loaded file's other
 * header fields (USERDEFn, other programs' APP_ fields, ...) follow, in
 * their original order and with their type indicators;
 * CREATED_TIMESTAMP is set to now. Caller frees. */
char *adif_document_to_adif(AdifDocument *self, const char *program_version,
                            const char *program_hash);

/* --- rows ---------------------------------------------------------- */

/* The rows as a GListModel of AdifRow* (borrowed; ref it to keep it).
 * Suitable to hand straight to a GtkColumnView's selection model. */
GListModel *adif_document_get_rows(AdifDocument *self);

guint adif_document_get_n_rows(AdifDocument *self);
AdifRow *adif_document_get_row(AdifDocument *self, guint position); /* borrowed, or NULL */

/* Position of `row` in the document; FALSE if it is not (or no longer) a
 * row of this document. */
gboolean adif_document_find_row(AdifDocument *self, AdifRow *row, guint *position);

void adif_document_insert_row(AdifDocument *self, guint position); /* clamped */
void adif_document_delete_row(AdifDocument *self, guint position); /* ignored if out of range */

/* --- schema ------------------------------------------------------- */

guint adif_document_get_n_columns(AdifDocument *self);

/* By visible position (0-based). name is borrowed; NULL / 0 if out of range. */
const char *adif_document_column_name_at(AdifDocument *self, guint position);
guint adif_document_column_data_index_at(AdifDocument *self, guint position);

/* By stable data-index. */
const char *adif_document_column_name(AdifDocument *self, guint data_index); /* or NULL */
/* Visible position of that column, or the column count if there is none. */
guint adif_document_column_position(AdifDocument *self, guint data_index);

/* Look up a column by name (case-insensitive). Sets *data_index and
 * returns TRUE if found. */
gboolean adif_document_find_column(AdifDocument *self, const char *name, guint *data_index);

typedef enum {
    ADIF_COLUMN_NAME_OK,
    ADIF_COLUMN_NAME_EMPTY,     /* nothing entered */
    ADIF_COLUMN_NAME_INVALID,   /* not a valid ADIF field name */
    ADIF_COLUMN_NAME_DUPLICATE, /* a column with that name already exists */
} AdifColumnNameStatus;

/* Vet `name` as a brand-new column name (case-insensitive duplicate
 * check). The window turns the status into a sentence for the user. */
AdifColumnNameStatus adif_document_check_new_column_name(AdifDocument *self, const char *name);

/* Insert a new empty column named `name` (upper-cased) at visible
 * `position` (clamped). Returns its data-index. Emits "schema-changed". */
guint adif_document_insert_column(AdifDocument *self, guint position, const char *name);

/* Remove the column with `data_index`. Returns FALSE (and does nothing)
 * if it is the last column or there is no such column. Emits
 * "schema-changed" on success. */
gboolean adif_document_delete_column(AdifDocument *self, guint data_index);

/* --- cell values ------------------------------------------------- */

/* "" if the row or column is out of range; never NULL. */
const char *adif_document_get_value(AdifDocument *self, guint row, guint data_index);

/* Set one cell as a single undo step (row grows to fit the column). */
void adif_document_set_value(AdifDocument *self, guint row, guint data_index, const char *text);

/* Set every row's value in the column to `text` (one undo step). */
void adif_document_fill_column(AdifDocument *self, guint data_index, const char *text);

/* Like fill_column, but skips rows that have no value in any column, so a
 * batch stamp doesn't turn blank rows into saved records (one undo step). */
void adif_document_stamp_column(AdifDocument *self, guint data_index, const char *text);

/* Incremental form of fill_column() (`skip_blank` FALSE) and
 * stamp_column() (`skip_blank` TRUE), for a caller with its own
 * main-loop turn (e.g. an idle source) that wants to spread a big column
 * over several turns instead of blocking for the whole thing. Same
 * one-op, one-signal result as the synchronous forms - only finish()
 * records the undo step and fires "column-values-changed", once.
 *
 * Usage: begin() once; step() repeatedly - each call writes rows until
 * `deadline` (a g_get_monotonic_time() timestamp) or the column ends,
 * returning TRUE if rows remain; once it returns FALSE, call finish().
 * Nothing else may change the document in between. If the document is
 * going away before that, call cancel() instead - it drops the
 * in-progress state without touching the document (whatever rows had
 * already been written stay as they are).
 *
 * begin() returns NULL if the column doesn't exist. `self` must outlive
 * the returned write. */
typedef struct AdifColumnWrite AdifColumnWrite;
AdifColumnWrite *adif_document_column_write_begin(AdifDocument *self, guint data_index,
                                                  const char *text, gboolean skip_blank);
gboolean adif_document_column_write_step(AdifColumnWrite *write, gint64 deadline);
void adif_document_column_write_finish(AdifColumnWrite *write);
void adif_document_column_write_cancel(AdifColumnWrite *write);

/* Live in-place editing for the grid: repeated calls on the same
 * (row, data_index) fold into one undo step. commit_cell_edit() closes
 * the step (call it on focus-out; the document also calls it itself
 * before any other mutation and before undo/redo). */
void adif_document_edit_cell(AdifDocument *self, AdifRow *row, guint data_index, const char *text);
void adif_document_commit_cell_edit(AdifDocument *self);

/* --- search & replace ----------------------------------------- */

/* One cell, addressed by row position and stable column data-index. */
typedef struct {
    guint row;
    guint data_index;
} AdifCellRef;

/* A search (and optionally replace) over cell values. `query` and
 * `replacement` are borrowed. An empty or NULL `query` matches nothing.
 * Matching is ASCII case-insensitive unless `case_sensitive`; it is a
 * substring test unless `whole_field` (whole cell value). When `scoped`,
 * only the column with `data_index` is considered. */
typedef struct {
    const char *query;
    const char *replacement; /* replace only; NULL is treated as "" */
    gboolean case_sensitive;
    gboolean whole_field;
    gboolean scoped;
    guint data_index;
} AdifQuery;

/* How many cells match. */
guint adif_document_count_matches(AdifDocument *self, const AdifQuery *q);

/* First match found scanning row-major (across a row, then down) from
 * `origin`, wrapping around the grid exactly once. `backward` reverses
 * the scan; `include_origin` lets the origin cell itself be the answer
 * (pass FALSE for "find next"). An out-of-range `origin` means the top
 * (or bottom, when backward). Returns FALSE if nothing matches anywhere;
 * otherwise `*match` is set. */
gboolean adif_document_find(AdifDocument *self, const AdifQuery *q, AdifCellRef origin,
                            gboolean backward, gboolean include_origin, AdifCellRef *match);

/* Replace within the single cell `cell` if it matches: every occurrence
 * of the query in that cell (or the whole value, when `whole_field`).
 * One undo step. Returns TRUE if the cell's value changed. */
gboolean adif_document_replace(AdifDocument *self, const AdifQuery *q, AdifCellRef cell);

/* Replace in every matching cell as a single undo step (one Ctrl+Z puts
 * them all back). Returns the number of cells changed. */
guint adif_document_replace_all(AdifDocument *self, const AdifQuery *q);

/* --- undo / redo ----------------------------------------------- */

gboolean adif_document_can_undo(AdifDocument *self);
gboolean adif_document_can_redo(AdifDocument *self);
/* Short label for what the next undo/redo would act on
 * (e.g. "Insert Column"), or NULL when there is nothing. Borrowed. */
const char *adif_document_undo_label(AdifDocument *self);
const char *adif_document_redo_label(AdifDocument *self);
void adif_document_undo(AdifDocument *self);
void adif_document_redo(AdifDocument *self);

/* Everything mutated between begin_group() and the matching end_group()
 * becomes a single undo step labelled `label`. Calls don't nest (a nested
 * begin/end pair is folded into the outer group). Don't undo/redo while a
 * group is open. Column inserts/deletes inside the group emit a single
 * "schema-changed" from end_group(), not one each. */
void adif_document_begin_group(AdifDocument *self, const char *label);
void adif_document_end_group(AdifDocument *self);

/* --- modified flag --------------------------------------------- */

gboolean adif_document_get_modified(AdifDocument *self);
/* FALSE marks the current state as the saved one (undoing back to it
 * clears the flag again); TRUE forces it. */
void adif_document_set_modified(AdifDocument *self, gboolean modified);

G_END_DECLS
