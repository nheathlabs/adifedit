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

#include "adif_document.h"

#include <string.h>

#include "adif_field.h"
#include "time_slice.h"

/* Cap on retained undo steps. One "Fill Column" step snapshots every
 * row's old value, so keeping this modest bounds the memory a long
 * editing session can hold. */
#define ADIF_UNDO_LIMIT 200

typedef struct {
    char *name;         /* owned, upper-cased ADIF field name */
    guint data_index;   /* stable slot in every AdifRow's values */
    char type;          /* type indicator from the loaded file (e.g. 'N'), written
                           back on save; 0 for none, and for a column added here */
} Column;

/* ------------------------------------------------------------------- */
/* Undo history                                                         */
/* ------------------------------------------------------------------- */

typedef enum {
    OP_SET_VALUE,
    OP_INSERT_COLUMN,
    OP_DELETE_COLUMN,
    OP_INSERT_ROW,
    OP_DELETE_ROW,
    OP_FILL_COLUMN,
    OP_GROUP, /* several ops that undo/redo as one step */
} OpType;

typedef struct {
    OpType type;
    AdifRow *row;        /* SET_VALUE: the edited row. INSERT/DELETE_ROW: the row object. (ref) */
    guint data_index;    /* SET_VALUE, *_COLUMN */
    guint position; /* INSERT/DELETE_COLUMN: visible column pos. INSERT/DELETE_ROW: row pos. */
    char *name; /* INSERT/DELETE_COLUMN: column name (upper-cased). GROUP: the step label. */
    char col_type; /* DELETE_COLUMN: the column's type indicator, restored by undo */
    char *text_before; /* SET_VALUE: the value before the edit */
    char *text_after; /* SET_VALUE: the value after. FILL_COLUMN: the fill text. */
    GPtrArray *values; /* FILL_COLUMN: each row's value before the fill (owned char*) */
    GPtrArray *children; /* GROUP: the member ops, in application order (owned Op*) */
    gboolean skip_blank; /* FILL_COLUMN: a stamp - don't touch entirely-blank rows */
} Op;

static Op *op_new(OpType type) {
    Op *op = g_new0(Op, 1);
    op->type = type;
    return op;
}

static const char *op_label(const Op *op) {
    switch (op->type) {
        case OP_SET_VALUE:
            return "Edit Cell";
        case OP_INSERT_COLUMN:
            return "Insert Column";
        case OP_DELETE_COLUMN:
            return "Delete Column";
        case OP_INSERT_ROW:
            return "Insert Row";
        case OP_DELETE_ROW:
            return "Delete Row";
        case OP_FILL_COLUMN:
            return "Fill Column";
        case OP_GROUP:
            return op->name;
    }
    return "";
}

static void op_free(gpointer data) {
    Op *op = data;
    g_free(op->name);
    g_free(op->text_before);
    g_free(op->text_after);
    g_clear_pointer(&op->values, g_ptr_array_unref);
    g_clear_pointer(&op->children, g_ptr_array_unref);
    g_clear_object(&op->row);
    g_free(op);
}

/* One field of the loaded file's header that adifedit writes back as it
 * was (see header_from_parse). */
typedef struct {
    char *name; /* upper-cased */
    char *value;
    char type; /* type indicator, or 0 */
} HeaderField;

static void header_field_free(gpointer data) {
    HeaderField *f = data;
    g_free(f->name);
    g_free(f->value);
    g_free(f);
}

/* ------------------------------------------------------------------- */

struct _AdifDocument {
    GObject parent_instance;

    GListStore *rows; /* AdifRow* */
    GArray *schema; /* Column, in visible order */
    guint next_data_index; /* monotonic; never reused, even across undo */
    gboolean modified;

    /* "<PROGRAMID> <PROGRAMVERSION>" of the file this document was loaded
     * from, captured once at load time so it survives re-saving through
     * adifedit. NULL if the source had no such header fields. */
    char *orig_prog;

    /* The loaded file's other header fields (HeaderField*, file order) -
     * USERDEFn definitions, CREATED_TIMESTAMP, other programs' APP_
     * fields - which a save writes back after adifedit's own. */
    GPtrArray *header;

    GPtrArray *undo_stack; /* Op*, oldest first; top == most recent */
    GPtrArray *redo_stack; /* Op*, top == next to redo */
    gssize clean_depth; /* undo_stack->len at the last save, or -1 if unreachable */

    Op *open_group;     /* between begin_group/end_group: ops accumulate here */
    guint group_depth;  /* begin_group nesting count (groups don't actually nest) */

    /* While > 0 (a group is open, or a group op is being applied or
     * reverted), "schema-changed" is held back and fired once on thaw, so
     * a multi-column step costs the view one rebuild rather than one per
     * column. */
    guint schema_freeze;
    gboolean schema_pending; /* a change was held back while frozen */

    /* Coalesced live cell edit in progress. */
    gboolean pending_active;
    AdifRow *pending_row; /* ref */
    guint pending_di;
    char *pending_before;
};

G_DEFINE_TYPE(AdifDocument, adif_document, G_TYPE_OBJECT)

enum {
    SIGNAL_SCHEMA_CHANGED,
    SIGNAL_MODIFIED_CHANGED,
    SIGNAL_HISTORY_CHANGED,
    SIGNAL_COLUMN_VALUES_CHANGED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static void clear_column(gpointer data) {
    Column *c = data;
    g_clear_pointer(&c->name, g_free);
}

/* ------------------------------------------------------------------- */
/* GObject boilerplate                                                  */
/* ------------------------------------------------------------------- */

static void adif_document_finalize(GObject *object) {
    AdifDocument *self = ADIF_DOCUMENT(object);
    g_clear_pointer(&self->open_group, op_free);
    g_clear_pointer(&self->undo_stack, g_ptr_array_unref);
    g_clear_pointer(&self->redo_stack, g_ptr_array_unref);
    g_clear_object(&self->pending_row);
    g_clear_pointer(&self->pending_before, g_free);
    g_clear_pointer(&self->orig_prog, g_free);
    g_ptr_array_unref(self->header);
    g_clear_object(&self->rows);
    g_clear_pointer(&self->schema, g_array_unref);
    G_OBJECT_CLASS(adif_document_parent_class)->finalize(object);
}

static void adif_document_class_init(AdifDocumentClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = adif_document_finalize;

    signals[SIGNAL_SCHEMA_CHANGED] = g_signal_new("schema-changed", G_TYPE_FROM_CLASS(klass),
                                                  G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
                                                  G_TYPE_NONE, 0);
    signals[SIGNAL_MODIFIED_CHANGED] = g_signal_new("modified-changed", G_TYPE_FROM_CLASS(klass),
                                                    G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
                                                    G_TYPE_NONE, 0);
    signals[SIGNAL_HISTORY_CHANGED] = g_signal_new("history-changed", G_TYPE_FROM_CLASS(klass),
                                                   G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
                                                   G_TYPE_NONE, 0);
    signals[SIGNAL_COLUMN_VALUES_CHANGED] = g_signal_new(
        "column-values-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void adif_document_init(AdifDocument *self) {
    self->rows = g_list_store_new(ADIF_TYPE_ROW);
    self->schema = g_array_new(FALSE, FALSE, sizeof(Column));
    g_array_set_clear_func(self->schema, clear_column);
    self->undo_stack = g_ptr_array_new_with_free_func(op_free);
    self->redo_stack = g_ptr_array_new_with_free_func(op_free);
    self->clean_depth = 0;
    self->header = g_ptr_array_new_with_free_func(header_field_free);
}

AdifDocument *adif_document_new(void) {
    return g_object_new(ADIF_TYPE_DOCUMENT, NULL);
}

/* ------------------------------------------------------------------- */
/* Small accessors                                                      */
/* ------------------------------------------------------------------- */

GListModel *adif_document_get_rows(AdifDocument *self) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);
    return G_LIST_MODEL(self->rows);
}

guint adif_document_get_n_rows(AdifDocument *self) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), 0);
    return g_list_model_get_n_items(G_LIST_MODEL(self->rows));
}

AdifRow *adif_document_get_row(AdifDocument *self, guint position) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);
    AdifRow *row = g_list_model_get_item(G_LIST_MODEL(self->rows), position);
    if (row != NULL) {
        g_object_unref(row); /* hand back a borrowed pointer */
    }
    return row;
}

gboolean adif_document_find_row(AdifDocument *self, AdifRow *row, guint *position) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), FALSE);
    return g_list_store_find(self->rows, row, position);
}

guint adif_document_get_n_columns(AdifDocument *self) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), 0);
    return self->schema->len;
}

static Column *column_at(AdifDocument *self, guint position) {
    return position < self->schema->len ? &g_array_index(self->schema, Column, position) : NULL;
}

/* Visible position of column `data_index`, or the column count if none. */
static guint column_pos(AdifDocument *self, guint data_index) {
    for (guint i = 0; i < self->schema->len; i++) {
        if (g_array_index(self->schema, Column, i).data_index == data_index) {
            return i;
        }
    }
    return self->schema->len;
}

static Column *column_by_index(AdifDocument *self, guint data_index) {
    return column_at(self, column_pos(self, data_index));
}

const char *adif_document_column_name_at(AdifDocument *self, guint position) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);
    Column *c = column_at(self, position);
    return c != NULL ? c->name : NULL;
}

guint adif_document_column_data_index_at(AdifDocument *self, guint position) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), 0);
    Column *c = column_at(self, position);
    return c != NULL ? c->data_index : 0;
}

const char *adif_document_column_name(AdifDocument *self, guint data_index) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);
    Column *c = column_by_index(self, data_index);
    return c != NULL ? c->name : NULL;
}

guint adif_document_column_position(AdifDocument *self, guint data_index) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), 0);
    return column_pos(self, data_index);
}

gboolean adif_document_find_column(AdifDocument *self, const char *name, guint *data_index) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), FALSE);
    g_return_val_if_fail(name != NULL, FALSE);
    for (guint i = 0; i < self->schema->len; i++) {
        Column *c = &g_array_index(self->schema, Column, i);
        if (g_ascii_strcasecmp(c->name, name) == 0) {
            if (data_index != NULL) {
                *data_index = c->data_index;
            }
            return TRUE;
        }
    }
    return FALSE;
}

const char *adif_document_get_value(AdifDocument *self, guint row, guint data_index) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), "");
    AdifRow *r = adif_document_get_row(self, row);
    return r != NULL ? adif_row_get_value(r, data_index) : "";
}

/* ------------------------------------------------------------------- */
/* Modified flag                                                        */
/* ------------------------------------------------------------------- */

static void set_modified_flag(AdifDocument *self, gboolean modified) {
    modified = !!modified;
    if (self->modified == modified) {
        return;
    }
    self->modified = modified;
    g_signal_emit(self, signals[SIGNAL_MODIFIED_CHANGED], 0);
}

static gboolean pending_differs(AdifDocument *self) {
    if (!self->pending_active) {
        return FALSE;
    }
    const char *now = adif_row_get_value(self->pending_row, self->pending_di);
    return g_strcmp0(now, self->pending_before) != 0;
}

/* Recompute the flag from the history position, any pending edit, and any
 * ops accumulated in a not-yet-closed group. */
static void update_modified(AdifDocument *self) {
    gboolean dirty = self->clean_depth < 0 || (gssize)self->undo_stack->len != self->clean_depth
                     || pending_differs(self)
                     || (self->open_group != NULL && self->open_group->children->len > 0);
    set_modified_flag(self, dirty);
}

gboolean adif_document_get_modified(AdifDocument *self) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), FALSE);
    return self->modified;
}

void adif_document_set_modified(AdifDocument *self, gboolean modified) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    if (modified) {
        self->clean_depth = -1;
    } else {
        adif_document_commit_cell_edit(self);
        self->clean_depth = (gssize)self->undo_stack->len;
    }
    update_modified(self);
}

/* ------------------------------------------------------------------- */
/* Coalesced "schema-changed"                                           */
/* ------------------------------------------------------------------- */

static void emit_schema_changed(AdifDocument *self) {
    if (self->schema_freeze > 0) {
        self->schema_pending = TRUE;
        return;
    }
    g_signal_emit(self, signals[SIGNAL_SCHEMA_CHANGED], 0);
}

static void freeze_schema(AdifDocument *self) {
    self->schema_freeze++;
}

static void thaw_schema(AdifDocument *self) {
    g_return_if_fail(self->schema_freeze > 0);
    if (--self->schema_freeze == 0 && self->schema_pending) {
        self->schema_pending = FALSE;
        g_signal_emit(self, signals[SIGNAL_SCHEMA_CHANGED], 0);
    }
}

/* ------------------------------------------------------------------- */
/* Raw mutators - change state and fire signals, but record no history  */
/* ------------------------------------------------------------------- */

static void raw_set_value(AdifRow *row, guint data_index, const char *text) {
    adif_row_set_value(row, data_index, text);
}

static void raw_insert_column(AdifDocument *self, guint position, const char *name_upper,
                              guint data_index, char type) {
    Column c = { .name = g_strdup(name_upper), .data_index = data_index, .type = type };
    g_array_insert_val(self->schema, MIN(position, self->schema->len), c);
    if (data_index >= self->next_data_index) {
        self->next_data_index = data_index + 1;
    }
    emit_schema_changed(self);
}

static void raw_delete_column(AdifDocument *self, guint data_index) {
    guint pos = column_pos(self, data_index);
    if (pos < self->schema->len) {
        /* The orphaned value slot in each row is left untouched - undo
         * just re-inserts the column and the values are still there. */
        g_array_remove_index(self->schema, pos);
        emit_schema_changed(self);
    }
}

static gboolean row_is_blank(AdifDocument *self, guint r);

static void raw_fill_column(AdifDocument *self, guint data_index, const char *text,
                            gboolean skip_blank) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->rows));
    for (guint i = 0; i < n; i++) {
        if (!skip_blank || !row_is_blank(self, i)) {
            raw_set_value(adif_document_get_row(self, i), data_index, text);
        }
    }
    g_signal_emit(self, signals[SIGNAL_COLUMN_VALUES_CHANGED], 0, data_index);
}

static void raw_restore_column(AdifDocument *self, guint data_index, GPtrArray *values) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->rows));
    for (guint i = 0; i < n && i < values->len; i++) {
        raw_set_value(adif_document_get_row(self, i), data_index, g_ptr_array_index(values, i));
    }
    g_signal_emit(self, signals[SIGNAL_COLUMN_VALUES_CHANGED], 0, data_index);
}

/* TRUE if row `r` has no non-empty value in any current column. */
static gboolean row_is_blank(AdifDocument *self, guint r) {
    AdifRow *row = adif_document_get_row(self, r);
    if (row == NULL) {
        return TRUE;
    }
    for (guint c = 0; c < self->schema->len; c++) {
        if (adif_row_get_value(row, g_array_index(self->schema, Column, c).data_index)[0] != '\0') {
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------- */
/* Applying and reverting one op                                        */
/* ------------------------------------------------------------------- */

static void op_apply(AdifDocument *self, Op *op) {
    switch (op->type) {
        case OP_SET_VALUE:
            raw_set_value(op->row, op->data_index, op->text_after);
            g_signal_emit(self, signals[SIGNAL_COLUMN_VALUES_CHANGED], 0, op->data_index);
            break;
        case OP_INSERT_COLUMN:
            raw_insert_column(self, op->position, op->name, op->data_index, op->col_type);
            break;
        case OP_DELETE_COLUMN:
            raw_delete_column(self, op->data_index);
            break;
        case OP_INSERT_ROW:
            g_list_store_insert(self->rows, op->position, op->row);
            break;
        case OP_DELETE_ROW:
            g_list_store_remove(self->rows, op->position);
            break;
        case OP_FILL_COLUMN:
            raw_fill_column(self, op->data_index, op->text_after, op->skip_blank);
            break;
        case OP_GROUP:
            freeze_schema(self);
            for (guint i = 0; i < op->children->len; i++) {
                op_apply(self, g_ptr_array_index(op->children, i));
            }
            thaw_schema(self);
            break;
    }
}

static void op_revert(AdifDocument *self, Op *op) {
    switch (op->type) {
        case OP_SET_VALUE:
            raw_set_value(op->row, op->data_index, op->text_before);
            g_signal_emit(self, signals[SIGNAL_COLUMN_VALUES_CHANGED], 0, op->data_index);
            break;
        case OP_INSERT_COLUMN:
            raw_delete_column(self, op->data_index);
            break;
        case OP_DELETE_COLUMN:
            raw_insert_column(self, op->position, op->name, op->data_index, op->col_type);
            break;
        case OP_INSERT_ROW:
            g_list_store_remove(self->rows, op->position);
            break;
        case OP_DELETE_ROW:
            g_list_store_insert(self->rows, op->position, op->row);
            break;
        case OP_FILL_COLUMN:
            raw_restore_column(self, op->data_index, op->values);
            break;
        case OP_GROUP:
            freeze_schema(self);
            for (guint i = op->children->len; i > 0; i--) {
                op_revert(self, g_ptr_array_index(op->children, i - 1));
            }
            thaw_schema(self);
            break;
    }
}

/* ------------------------------------------------------------------- */
/* History bookkeeping                                                  */
/* ------------------------------------------------------------------- */

static void emit_history_changed(AdifDocument *self) {
    g_signal_emit(self, signals[SIGNAL_HISTORY_CHANGED], 0);
}

/* Push a freshly-applied op, discarding the redo branch. Inside a group,
 * accumulate it instead - the group is pushed as one step by end_group. */
static void record_op(AdifDocument *self, Op *op) {
    if (self->open_group != NULL) {
        g_ptr_array_add(self->open_group->children, op);
        update_modified(self);
        return;
    }
    if (self->redo_stack->len > 0 && self->clean_depth > (gssize)self->undo_stack->len) {
        self->clean_depth = -1; /* the saved state lived in the redo branch we're dropping */
    }
    g_ptr_array_set_size(self->redo_stack, 0);
    g_ptr_array_add(self->undo_stack, op);

    while (self->undo_stack->len > ADIF_UNDO_LIMIT) {
        g_ptr_array_remove_index(self->undo_stack, 0);
        if (self->clean_depth > 0) {
            self->clean_depth--;
        } else {
            self->clean_depth = -1; /* evicted the saved state */
        }
    }

    update_modified(self);
    emit_history_changed(self);
}

/* ------------------------------------------------------------------- */
/* Live cell editing (coalesced)                                        */
/* ------------------------------------------------------------------- */

static void clear_pending(AdifDocument *self) {
    self->pending_active = FALSE;
    g_clear_object(&self->pending_row);
    g_clear_pointer(&self->pending_before, g_free);
}

void adif_document_commit_cell_edit(AdifDocument *self) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    if (!self->pending_active) {
        return;
    }
    if (pending_differs(self)) {
        Op *op = op_new(OP_SET_VALUE);
        op->row = g_object_ref(self->pending_row);
        op->data_index = self->pending_di;
        op->text_before = g_strdup(self->pending_before);
        op->text_after = g_strdup(adif_row_get_value(self->pending_row, self->pending_di));
        clear_pending(self);
        record_op(self, op);
        return;
    }
    clear_pending(self);
    update_modified(self);
}

void adif_document_edit_cell(AdifDocument *self, AdifRow *row, guint data_index, const char *text) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    g_return_if_fail(ADIF_IS_ROW(row));

    if (self->pending_active && (self->pending_row != row || self->pending_di != data_index)) {
        adif_document_commit_cell_edit(self);
    }
    if (!self->pending_active) {
        self->pending_active = TRUE;
        self->pending_row = g_object_ref(row);
        self->pending_di = data_index;
        self->pending_before = g_strdup(adif_row_get_value(row, data_index));
    }
    raw_set_value(row, data_index, text);
    update_modified(self);
    emit_history_changed(self);
}

/* ------------------------------------------------------------------- */
/* Undo / redo                                                          */
/* ------------------------------------------------------------------- */

gboolean adif_document_can_undo(AdifDocument *self) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), FALSE);
    return self->undo_stack->len > 0 || pending_differs(self);
}

gboolean adif_document_can_redo(AdifDocument *self) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), FALSE);
    return self->redo_stack->len > 0;
}

const char *adif_document_undo_label(AdifDocument *self) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);
    if (self->undo_stack->len > 0) {
        return op_label(g_ptr_array_index(self->undo_stack, self->undo_stack->len - 1));
    }
    return pending_differs(self) ? "Edit Cell" : NULL;
}

const char *adif_document_redo_label(AdifDocument *self) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);
    if (self->redo_stack->len > 0) {
        return op_label(g_ptr_array_index(self->redo_stack, self->redo_stack->len - 1));
    }
    return NULL;
}

void adif_document_undo(AdifDocument *self) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    g_return_if_fail(self->open_group == NULL);
    adif_document_commit_cell_edit(self);
    if (self->undo_stack->len == 0) {
        return;
    }
    Op *op = g_ptr_array_steal_index(self->undo_stack, self->undo_stack->len - 1);
    op_revert(self, op);
    g_ptr_array_add(self->redo_stack, op);
    update_modified(self);
    emit_history_changed(self);
}

void adif_document_redo(AdifDocument *self) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    g_return_if_fail(self->open_group == NULL);
    adif_document_commit_cell_edit(self);
    if (self->redo_stack->len == 0) {
        return;
    }
    Op *op = g_ptr_array_steal_index(self->redo_stack, self->redo_stack->len - 1);
    op_apply(self, op);
    g_ptr_array_add(self->undo_stack, op);
    update_modified(self);
    emit_history_changed(self);
}

void adif_document_begin_group(AdifDocument *self, const char *label) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    if (self->group_depth++ > 0) {
        return; /* already grouping - flatten, don't nest */
    }
    adif_document_commit_cell_edit(self); /* a live cell edit is its own step */
    freeze_schema(self);
    Op *op = op_new(OP_GROUP);
    op->name = g_strdup(label);
    op->children = g_ptr_array_new_with_free_func(op_free);
    self->open_group = op;
}

void adif_document_end_group(AdifDocument *self) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    g_return_if_fail(self->group_depth > 0);
    if (--self->group_depth > 0) {
        return;
    }
    thaw_schema(self); /* the view catches up on the columns in one go */
    Op *group = g_steal_pointer(&self->open_group);
    if (group->children->len == 0) {
        op_free(group); /* nothing happened */
        update_modified(self);
        return;
    }
    record_op(self, group);
}

/* ------------------------------------------------------------------- */
/* Column-name validation                                               */
/* ------------------------------------------------------------------- */

AdifColumnNameStatus adif_document_check_new_column_name(AdifDocument *self, const char *name) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), ADIF_COLUMN_NAME_INVALID);
    if (name == NULL || name[0] == '\0') {
        return ADIF_COLUMN_NAME_EMPTY;
    }
    if (!adif_field_name_is_valid(name)) {
        return ADIF_COLUMN_NAME_INVALID;
    }
    if (adif_document_find_column(self, name, NULL)) {
        return ADIF_COLUMN_NAME_DUPLICATE;
    }
    return ADIF_COLUMN_NAME_OK;
}

/* ------------------------------------------------------------------- */
/* Public mutators - apply, then record one undo step                   */
/* ------------------------------------------------------------------- */

guint adif_document_insert_column(AdifDocument *self, guint position, const char *name) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), 0);
    g_return_val_if_fail(name != NULL, 0);
    adif_document_commit_cell_edit(self);

    Op *op = op_new(OP_INSERT_COLUMN);
    op->position = MIN(position, self->schema->len);
    op->name = g_ascii_strup(name, -1);
    op->data_index = self->next_data_index++;

    op_apply(self, op);
    record_op(self, op);
    return op->data_index;
}

gboolean adif_document_delete_column(AdifDocument *self, guint data_index) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), FALSE);
    adif_document_commit_cell_edit(self);

    if (self->schema->len <= 1) {
        return FALSE;
    }
    guint position = column_pos(self, data_index);
    if (position >= self->schema->len) {
        return FALSE;
    }

    Op *op = op_new(OP_DELETE_COLUMN);
    op->position = position;
    op->name = g_strdup(g_array_index(self->schema, Column, position).name);
    op->col_type = g_array_index(self->schema, Column, position).type;
    op->data_index = data_index;

    op_apply(self, op);
    record_op(self, op);
    return TRUE;
}

void adif_document_insert_row(AdifDocument *self, guint position) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    adif_document_commit_cell_edit(self);

    Op *op = op_new(OP_INSERT_ROW);
    op->position = MIN(position, g_list_model_get_n_items(G_LIST_MODEL(self->rows)));
    op->row = adif_row_new(); /* the op keeps this ref; the store takes its own */

    op_apply(self, op);
    record_op(self, op);
}

void adif_document_delete_row(AdifDocument *self, guint position) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    adif_document_commit_cell_edit(self);

    if (position >= g_list_model_get_n_items(G_LIST_MODEL(self->rows))) {
        return;
    }

    Op *op = op_new(OP_DELETE_ROW);
    op->position = position;
    op->row = g_list_model_get_item(G_LIST_MODEL(self->rows), position); /* owned ref */

    op_apply(self, op);
    record_op(self, op);
}

/* Set one cell as one SET_VALUE op (joining any open group), without
 * telling the view - for callers that change many cells and emit
 * "column-values-changed" once per column themselves. Builds the op
 * directly rather than going through edit_cell() + commit, which would
 * fire "history-changed" once per cell. FALSE if there is no such row. */
static gboolean set_value_quietly(AdifDocument *self, guint row, guint data_index,
                                  const char *text) {
    AdifRow *r = adif_document_get_row(self, row);
    if (r == NULL) {
        return FALSE;
    }
    adif_document_commit_cell_edit(self);
    if (text == NULL) {
        text = "";
    }
    const char *before = adif_row_get_value(r, data_index);
    if (strcmp(before, text) == 0) {
        return TRUE; /* unchanged - no undo step */
    }
    Op *op = op_new(OP_SET_VALUE);
    op->row = g_object_ref(r);
    op->data_index = data_index;
    op->text_before = g_strdup(before);
    op->text_after = g_strdup(text);
    raw_set_value(r, data_index, op->text_after);
    record_op(self, op);
    return TRUE;
}

void adif_document_set_value(AdifDocument *self, guint row, guint data_index, const char *text) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    if (set_value_quietly(self, row, data_index, text)) {
        g_signal_emit(self, signals[SIGNAL_COLUMN_VALUES_CHANGED], 0, data_index);
    }
}

/* Run a whole column write in one go - the synchronous form of
 * column_write_begin()/step()/finish(). */
static void column_write_now(AdifDocument *self, guint data_index, const char *text,
                             gboolean skip_blank) {
    AdifColumnWrite *write = adif_document_column_write_begin(self, data_index, text, skip_blank);
    if (write == NULL) {
        return;
    }
    adif_document_column_write_step(write, G_MAXINT64); /* no deadline: always finishes now */
    adif_document_column_write_finish(write);
}

void adif_document_fill_column(AdifDocument *self, guint data_index, const char *text) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    column_write_now(self, data_index, text, FALSE);
}

/* Like fill_column, but leaves entirely-blank rows alone (a stamp
 * shouldn't turn empty rows into saved records). Reverting from the
 * whole-column snapshot still puts those rows back to "". */
void adif_document_stamp_column(AdifDocument *self, guint data_index, const char *text) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    column_write_now(self, data_index, text, TRUE);
}

/* ------------------------------------------------------------------- */
/* Incremental column writes - fill and stamp (see adif_document.h)     */
/* ------------------------------------------------------------------- */

struct AdifColumnWrite {
    AdifDocument *doc; /* borrowed - the caller keeps it alive */
    guint data_index;
    char *text;          /* owned - the fill text */
    gboolean skip_blank; /* a stamp: leave entirely-blank rows alone */
    GPtrArray *values;   /* owned - each row's value before the write, filled row by row */
    guint next_row;
    guint n_rows;
};

AdifColumnWrite *adif_document_column_write_begin(AdifDocument *self, guint data_index,
                                                  const char *text, gboolean skip_blank) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);
    adif_document_commit_cell_edit(self);
    if (column_by_index(self, data_index) == NULL) {
        return NULL;
    }
    AdifColumnWrite *write = g_new0(AdifColumnWrite, 1);
    write->doc = self;
    write->data_index = data_index;
    write->text = g_strdup(text != NULL ? text : "");
    write->skip_blank = skip_blank;
    write->n_rows = adif_document_get_n_rows(self);
    write->values = g_ptr_array_new_full(write->n_rows, g_free);
    return write;
}

gboolean adif_document_column_write_step(AdifColumnWrite *write, gint64 deadline) {
    g_return_val_if_fail(write != NULL, FALSE);
    guint since_check = 0;
    while (write->next_row < write->n_rows) {
        guint r = write->next_row++;
        AdifRow *row = adif_document_get_row(write->doc, r);
        g_ptr_array_add(write->values, g_strdup(adif_row_get_value(row, write->data_index)));
        if (!write->skip_blank || !row_is_blank(write->doc, r)) {
            raw_set_value(row, write->data_index, write->text);
        }
        if (adif_slice_expired(&since_check, deadline)) {
            return TRUE; /* rows remain */
        }
    }
    return FALSE; /* every row visited */
}

void adif_document_column_write_finish(AdifColumnWrite *write) {
    g_return_if_fail(write != NULL);
    g_return_if_fail(write->next_row == write->n_rows); /* caller must drain step() first */
    AdifDocument *self = write->doc;

    Op *op = op_new(OP_FILL_COLUMN);
    op->data_index = write->data_index;
    op->text_after = g_steal_pointer(&write->text);
    op->values = g_steal_pointer(&write->values);
    op->skip_blank = write->skip_blank;
    record_op(self, op); /* the writes already happened in step() - don't op_apply() again */
    g_signal_emit(self, signals[SIGNAL_COLUMN_VALUES_CHANGED], 0, write->data_index);

    g_free(write);
}

void adif_document_column_write_cancel(AdifColumnWrite *write) {
    if (write == NULL) {
        return;
    }
    /* Whatever rows were already written stay as they are - cancel() is
     * only for a document that's going away, so there is nothing left to
     * revert them for. */
    g_free(write->text);
    g_clear_pointer(&write->values, g_ptr_array_unref);
    g_free(write);
}

/* ------------------------------------------------------------------- */
/* Search & replace                                                     */
/* ------------------------------------------------------------------- */

static gboolean query_is_usable(const AdifQuery *q) {
    return q != NULL && q->query != NULL && q->query[0] != '\0';
}

/* strstr, ASCII case-insensitive when !case_sensitive. Pointer into hay. */
static const char *find_in(const char *hay, const char *needle, gboolean case_sensitive) {
    if (case_sensitive) {
        return strstr(hay, needle);
    }
    gsize nlen = strlen(needle);
    for (const char *p = hay; *p != '\0'; p++) {
        if (g_ascii_strncasecmp(p, needle, nlen) == 0) {
            return p;
        }
    }
    return NULL;
}

static gboolean value_matches(const char *value, const AdifQuery *q) {
    if (q->whole_field) {
        return q->case_sensitive ? strcmp(value, q->query) == 0
                                 : g_ascii_strcasecmp(value, q->query) == 0;
    }
    return find_in(value, q->query, q->case_sensitive) != NULL;
}

/* The replacement value for a matching cell (caller frees). */
static char *value_replaced(const char *value, const AdifQuery *q) {
    const char *repl = q->replacement != NULL ? q->replacement : "";
    if (q->whole_field) {
        return g_strdup(repl);
    }
    gsize nlen = strlen(q->query);
    GString *out = g_string_new(NULL);
    for (const char *p = value;;) {
        const char *hit = find_in(p, q->query, q->case_sensitive);
        if (hit == NULL) {
            g_string_append(out, p);
            break;
        }
        g_string_append_len(out, p, (gssize)(hit - p));
        g_string_append(out, repl);
        p = hit + nlen; /* past the replaced span, so repl may contain the query */
    }
    return g_string_free(out, FALSE);
}

/* Does the cell at (row, column position `pos`) match, honouring scope? */
static gboolean cell_matches(AdifDocument *self, guint row, guint pos, const AdifQuery *q) {
    guint di = g_array_index(self->schema, Column, pos).data_index;
    if (q->scoped && di != q->data_index) {
        return FALSE;
    }
    return value_matches(adif_row_get_value(adif_document_get_row(self, row), di), q);
}

guint adif_document_count_matches(AdifDocument *self, const AdifQuery *q) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), 0);
    if (!query_is_usable(q)) {
        return 0;
    }
    guint n_rows = adif_document_get_n_rows(self);
    guint count = 0;
    for (guint r = 0; r < n_rows; r++) {
        for (guint c = 0; c < self->schema->len; c++) {
            if (cell_matches(self, r, c, q)) {
                count++;
            }
        }
    }
    return count;
}

gboolean adif_document_find(AdifDocument *self, const AdifQuery *q, AdifCellRef origin,
                            gboolean backward, gboolean include_origin, AdifCellRef *match) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), FALSE);
    if (!query_is_usable(q)) {
        return FALSE;
    }
    guint n_rows = adif_document_get_n_rows(self);
    guint n_cols = self->schema->len;
    if (n_rows == 0 || n_cols == 0) {
        return FALSE;
    }
    guint total = n_rows * n_cols;
    gint64 step = backward ? -1 : 1;

    /* Row-major linear index to start scanning from. */
    gint64 start;
    if (origin.row >= n_rows) {
        start = backward ? (gint64)total - 1 : 0; /* the far end - no origin cell */
    } else {
        guint pos = column_pos(self, origin.data_index);
        if (pos >= n_cols) {
            pos = 0;
        }
        gint64 origin_lin = ((gint64)origin.row * n_cols) + pos;
        start = include_origin ? origin_lin : origin_lin + step;
    }

    /* Scan a full cycle: every cell exactly once, in direction `step`. */
    for (guint i = 0; i < total; i++) {
        gint64 lin = ((((start + (step * (gint64)i)) % total) + total) % total);
        guint row = (guint)(lin / n_cols);
        guint pos = (guint)(lin % n_cols);
        if (cell_matches(self, row, pos, q)) {
            match->row = row;
            match->data_index = g_array_index(self->schema, Column, pos).data_index;
            return TRUE;
        }
    }
    return FALSE;
}

gboolean adif_document_replace(AdifDocument *self, const AdifQuery *q, AdifCellRef cell) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), FALSE);
    if (!query_is_usable(q) || column_by_index(self, cell.data_index) == NULL) {
        return FALSE;
    }
    const char *current = adif_document_get_value(self, cell.row, cell.data_index);
    if (!value_matches(current, q)) {
        return FALSE;
    }
    char *next = value_replaced(current, q);
    gboolean changed = strcmp(current, next) != 0;
    if (changed) {
        adif_document_set_value(self, cell.row, cell.data_index, next); /* one undo step */
    }
    g_free(next);
    return changed;
}

/* Add `data_index` to `touched` unless it's already there. */
static void note_touched(GArray *touched, guint data_index) {
    for (guint t = 0; t < touched->len; t++) {
        if (g_array_index(touched, guint, t) == data_index) {
            return;
        }
    }
    g_array_append_val(touched, data_index);
}

/* Replace All's work on one cell: TRUE if it changed. */
static gboolean replace_in_cell(AdifDocument *self, const AdifQuery *q, guint row,
                                guint data_index) {
    const char *current = adif_row_get_value(adif_document_get_row(self, row), data_index);
    if (!value_matches(current, q)) {
        return FALSE;
    }
    char *next = value_replaced(current, q);
    gboolean changed = strcmp(current, next) != 0;
    if (changed) {
        set_value_quietly(self, row, data_index, next); /* joins the open group */
    }
    g_free(next);
    return changed;
}

guint adif_document_replace_all(AdifDocument *self, const AdifQuery *q) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), 0);
    if (!query_is_usable(q)) {
        return 0;
    }
    adif_document_commit_cell_edit(self);

    char *label = g_strdup_printf("Replace \xE2\x80\x9C%s\xE2\x80\x9D", q->query);
    adif_document_begin_group(self, label);
    g_free(label);

    guint n_rows = adif_document_get_n_rows(self);
    guint changed = 0;
    GArray *touched = g_array_new(FALSE, FALSE, sizeof(guint)); /* data-indices, once each */
    for (guint r = 0; r < n_rows; r++) {
        for (guint c = 0; c < self->schema->len; c++) {
            guint di = g_array_index(self->schema, Column, c).data_index;
            if ((!q->scoped || di == q->data_index) && replace_in_cell(self, q, r, di)) {
                changed++;
                note_touched(touched, di);
            }
        }
    }

    adif_document_end_group(self);
    /* One refresh per affected column, not one per replaced cell. */
    for (guint t = 0; t < touched->len; t++) {
        g_signal_emit(self, signals[SIGNAL_COLUMN_VALUES_CHANGED], 0,
                      g_array_index(touched, guint, t));
    }
    g_array_unref(touched);
    return changed;
}

/* ------------------------------------------------------------------- */
/* Load / serialise                                                     */
/* ------------------------------------------------------------------- */

/* Record which program produced the file we're loading, so a later
 * re-save can keep pointing at the true origin rather than at adifedit.
 * An existing APP_ADIFEDIT_ORIG_PROG wins - it was captured on an
 * earlier pass and must not be rewritten. A file adifedit itself wrote
 * without one has no other origin, so nothing is recorded. */
static char *origin_from_header(const AdifRecord *header) {
    if (header == NULL) {
        return NULL;
    }
    const char *prior = adif_record_get_field(header, "APP_ADIFEDIT_ORIG_PROG");
    if (prior != NULL && prior[0] != '\0') {
        return g_strdup(prior);
    }
    const char *id = adif_record_get_field(header, "PROGRAMID");
    if (id != NULL && g_ascii_strcasecmp(id, "adifedit") == 0) {
        return NULL;
    }
    const char *version = adif_record_get_field(header, "PROGRAMVERSION");
    if ((id == NULL || id[0] == '\0') && (version == NULL || version[0] == '\0')) {
        return NULL;
    }
    char *joined = g_strdup_printf("%s %s", id != NULL ? id : "", version != NULL ? version : "");
    g_strstrip(joined); /* in place */
    if (joined[0] == '\0') { /* only whitespace: nothing to record */
        g_free(joined);
        return NULL;
    }
    return joined;
}

/* Header fields adifedit writes itself on every save, so it doesn't keep
 * the loaded file's copies. */
static gboolean header_field_is_own(const char *name) {
    return strcmp(name, "ADIF_VER") == 0 || strcmp(name, "PROGRAMID") == 0
           || strcmp(name, "PROGRAMVERSION") == 0 || g_str_has_prefix(name, "APP_ADIFEDIT_");
}

/* Keep the loaded header's fields, other than adifedit's own, to write
 * back on save - everything adifedit read, it hands back. */
static void header_from_parse(AdifDocument *self, const AdifRecord *header) {
    guint n = adif_record_header_n_fields(header);
    for (guint i = 0; i < n; i++) {
        const char *name = adif_record_header_name_at(header, i);
        if (header_field_is_own(name)) {
            continue;
        }
        HeaderField *f = g_new0(HeaderField, 1);
        f->name = g_strdup(name);
        f->value = g_strdup(adif_record_get_field(header, name));
        f->type = adif_record_header_type_at(header, i);
        g_ptr_array_add(self->header, f);
    }
}

/* Drop everything - schema, rows, undo history, pending edit, origin -
 * without emitting. Shared by load_parse and reset. */
static void clear_all_state(AdifDocument *self) {
    clear_pending(self);
    g_clear_pointer(&self->open_group, op_free);
    self->group_depth = 0;
    self->schema_freeze = 0; /* the caller emits "schema-changed" itself */
    self->schema_pending = FALSE;
    g_ptr_array_set_size(self->undo_stack, 0);
    g_ptr_array_set_size(self->redo_stack, 0);
    self->clean_depth = 0;

    g_list_store_remove_all(self->rows);
    if (self->schema->len > 0) {
        g_array_remove_range(self->schema, 0, self->schema->len);
    }
    self->next_data_index = 0;
    g_clear_pointer(&self->orig_prog, g_free);
    g_ptr_array_set_size(self->header, 0);
}

void adif_document_reset(AdifDocument *self) {
    g_return_if_fail(ADIF_IS_DOCUMENT(self));
    clear_all_state(self);
    set_modified_flag(self, FALSE);
    g_signal_emit(self, signals[SIGNAL_SCHEMA_CHANGED], 0);
    emit_history_changed(self);
}

struct AdifLoadStep {
    AdifDocument *doc;             /* borrowed */
    const AdifParseResult *result; /* borrowed; caller keeps it alive until finish()/cancel() */
    guint n_fields;
    guint next_record;
};

AdifLoadStep *adif_document_load_parse_begin(AdifDocument *self, const AdifParseResult *result) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);
    g_return_val_if_fail(result != NULL, NULL);

    clear_all_state(self);
    self->orig_prog = origin_from_header(result->header);
    header_from_parse(self, result->header);

    guint n_fields = result->fieldnames->len;
    for (guint c = 0; c < n_fields; c++) {
        Column col = {
            .name = g_strdup(g_ptr_array_index(result->fieldnames, c)),
            .data_index = self->next_data_index++,
            .type = g_array_index(result->fieldtypes, char, c),
        };
        g_array_append_val(self->schema, col);
    }
    /* Columns exist, no rows yet: let a view show the empty grid right
     * away instead of waiting for the whole file. */
    g_signal_emit(self, signals[SIGNAL_SCHEMA_CHANGED], 0);

    AdifLoadStep *step = g_new0(AdifLoadStep, 1);
    step->doc = self;
    step->result = result;
    step->n_fields = n_fields;
    return step;
}

gboolean adif_document_load_parse_step(AdifLoadStep *step, gint64 deadline) {
    guint n_records = step->result->records->len;

    while (step->next_record < n_records) {
        guint chunk_end = MIN(step->next_record + ADIF_SLICE_CHECK_ROWS, n_records);
        GPtrArray *chunk = g_ptr_array_new_full(chunk_end - step->next_record, g_object_unref);
        for (; step->next_record < chunk_end; step->next_record++) {
            AdifRecord *record = g_ptr_array_index(step->result->records, step->next_record);
            AdifRow *row = adif_row_new();
            for (guint c = 0; c < step->n_fields; c++) {
                adif_row_add_value(row, adif_record_get_value(record, c));
                /* begin() gave column c data index c (clear_all_state
                 * restarts the numbering). */
                if (adif_record_is_repaired(record, c)) {
                    adif_row_set_repaired(row, c);
                }
            }
            g_ptr_array_add(chunk, row);
        }
        /* One splice per chunk, not one append per row: the bound
         * GtkColumnView would otherwise do an incremental relayout per
         * row instead of one per chunk. This is also what makes the rows
         * already in view fill in as loading progresses. */
        guint pos = g_list_model_get_n_items(G_LIST_MODEL(step->doc->rows));
        g_list_store_splice(step->doc->rows, pos, 0, chunk->pdata, chunk->len);
        g_ptr_array_unref(chunk);

        if (step->next_record < n_records && g_get_monotonic_time() >= deadline) {
            return TRUE;
        }
    }
    return FALSE;
}

void adif_document_load_parse_finish(AdifLoadStep *step) {
    AdifDocument *self = step->doc;
    set_modified_flag(self, FALSE);
    /* All rows are in now: fire a second "schema-changed" so a view
     * re-fits its columns to the real content instead of just the
     * headers it fit to at begin(). */
    g_signal_emit(self, signals[SIGNAL_SCHEMA_CHANGED], 0);
    emit_history_changed(self);
    g_free(step);
}

void adif_document_load_parse_cancel(AdifLoadStep *step) {
    g_free(step);
}

void adif_document_load_parse(AdifDocument *self, const AdifParseResult *result) {
    AdifLoadStep *step = adif_document_load_parse_begin(self, result);
    if (step == NULL) {
        return;
    }
    adif_document_load_parse_step(step, G_MAXINT64);
    adif_document_load_parse_finish(step);
}

static void append_field(GString *out, const char *name, const char *value) {
    g_string_append_printf(out, "<%s:%zu>%s\n", name, strlen(value), value);
}

/* The loaded file's kept header fields, as they were - except
 * CREATED_TIMESTAMP, which becomes now: the saved file is a new one. */
static void append_kept_header(AdifDocument *self, GString *out) {
    for (guint i = 0; i < self->header->len; i++) {
        const HeaderField *f = g_ptr_array_index(self->header, i);
        char *now = NULL;
        const char *value = f->value;
        if (strcmp(f->name, "CREATED_TIMESTAMP") == 0) {
            GDateTime *utc = g_date_time_new_now_utc();
            now = g_date_time_format(utc, "%Y%m%d %H%M%S");
            g_date_time_unref(utc);
            value = now;
        }
        if (f->type != '\0') {
            g_string_append_printf(out, "<%s:%zu:%c>%s\n", f->name, strlen(value), f->type, value);
        } else {
            append_field(out, f->name, value);
        }
        g_free(now);
    }
}

/* Append one record for `row`: its non-empty fields in column order and
 * an <EOR>. A row with every field empty is left out entirely. */
static void append_record(AdifDocument *self, GString *out, AdifRow *row) {
    gsize row_start = out->len;
    for (guint c = 0; c < self->schema->len; c++) {
        const Column *col = &g_array_index(self->schema, Column, c);
        const char *value = adif_row_get_value(row, col->data_index);
        if (value[0] != '\0') {
            if (col->type != '\0') {
                g_string_append_printf(out, "<%s:%zu:%c>%s", col->name, strlen(value), col->type,
                                       value);
            } else {
                g_string_append_printf(out, "<%s:%zu>%s", col->name, strlen(value), value);
            }
        }
    }
    if (out->len != row_start) {
        g_string_append(out, "<EOR>\n");
    }
}

char *adif_document_to_adif(AdifDocument *self, const char *program_version,
                            const char *program_hash) {
    g_return_val_if_fail(ADIF_IS_DOCUMENT(self), NULL);

    GString *out = g_string_new(NULL);
    g_string_append(out, "ADIF export from adifedit\n");
    append_field(out, "ADIF_VER", ADIF_SPEC_VERSION);
    append_field(out, "PROGRAMID", "adifedit");
    if (program_version != NULL) {
        append_field(out, "PROGRAMVERSION", program_version);
    }
    if (program_hash != NULL) {
        append_field(out, "APP_ADIFEDIT_PROGRAMHASH", program_hash);
    }
    if (self->orig_prog != NULL) {
        append_field(out, "APP_ADIFEDIT_ORIG_PROG", self->orig_prog);
    }
    append_kept_header(self, out);
    g_string_append(out, "<EOH>\n\n");

    guint n_rows = g_list_model_get_n_items(G_LIST_MODEL(self->rows));
    for (guint r = 0; r < n_rows; r++) {
        append_record(self, out, adif_document_get_row(self, r));
    }

    return g_string_free(out, FALSE);
}
