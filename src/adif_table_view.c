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
 * The "view" side of AdifTable: how a document cell becomes a widget, and
 * the right-click menus. The schema mirroring and auto-fit are in
 * adif_table.c; all data lives in the AdifDocument.
 */

#include "adif_table_priv.h"

/* ------------------------------------------------------------------- */
/* Column and cell state                                                */
/* ------------------------------------------------------------------- */

#define ADIF_COLUMN_INFO_KEY "adif-column-info"
#define ADIF_CELL_KEY "adif-cell"

/* Reference-counted: the column holds one reference and each of its cells
 * another, because GTK can keep a cell alive (and deliver its focus-leave)
 * after the column itself is gone - a column removed while one of its
 * cells has the focus. */
static void column_info_clear(gpointer data) {
    AdifColumnInfo *info = data;
    g_free(info->name);
    g_ptr_array_unref(info->cells);
}

static void column_info_release(gpointer data) {
    g_rc_box_release_full(data, column_info_clear);
}

AdifColumnInfo *adif_column_info(GtkColumnViewColumn *column) {
    return g_object_get_data(G_OBJECT(column), ADIF_COLUMN_INFO_KEY);
}

static void cell_free(gpointer data) {
    AdifCell *cell = data;
    column_info_release(cell->info);
    g_free(cell->edit_original);
    g_free(cell);
}

AdifCell *adif_cell(GtkWidget *widget) {
    return g_object_get_data(G_OBJECT(widget), ADIF_CELL_KEY);
}

/* ------------------------------------------------------------------- */
/* Cell factory                                                         */
/* ------------------------------------------------------------------- */

/*
 * A cell is a focusable box showing its value in a GtkLabel - cheap to
 * build, which matters because the grid builds one per visible row for
 * every column. (Not GtkInscription: it drew ellipsized text a few pixels
 * off from unclipped text in the same row.) Only the cell being edited gets a real GtkEntry,
 * created when editing starts (click, Enter / F2, or typing) and dropped
 * when it ends (Enter, Escape, or focus leaving the cell).
 *
 * While the editor is up, each keystroke goes through
 * adif_document_edit_cell(), which coalesces them into one undo step and
 * keeps the modified flag current; the step is closed when editing ends.
 * (GtkText has no built-in Ctrl+Z, so that key always reaches the
 * document's undo action.)
 */

void adif_cell_set_text_silently(GtkWidget *widget, const char *text) {
    AdifCell *cell = adif_cell(widget);
    gtk_label_set_text(GTK_LABEL(cell->label), text);
    if (cell->editor != NULL) {
        g_signal_handler_block(cell->editor, cell->changed_handler);
        gtk_editable_set_text(GTK_EDITABLE(cell->editor), text);
        g_signal_handler_unblock(cell->editor, cell->changed_handler);
    }
}

static void on_editor_changed(GtkEditable *editable, gpointer user_data) {
    AdifCell *cell = user_data;
    AdifTable *table = cell->info->table;
    if (cell->row == NULL || table == NULL) {
        return;
    }
    guint di = cell->info->data_index;
    adif_document_edit_cell(table->doc, cell->row, di, gtk_editable_get_text(editable));
    adif_table_forget_width(table, di);
}

/* TRUE if keyboard focus is on `area` or inside it. */
static gboolean focus_within(GtkWidget *area) {
    GtkRoot *root = gtk_widget_get_root(area);
    GtkWidget *focus = root != NULL ? gtk_root_get_focus(root) : NULL;
    return focus != NULL && (focus == area || gtk_widget_is_ancestor(focus, area));
}

/* Close the editor, if there is one: back to the label showing the
 * cell's current value, and close the edit's undo step. `revert` first
 * restores the text the edit started from (Escape). Focus stays on the
 * cell if the editor had it. */
static void end_edit(AdifCell *cell, gboolean revert) {
    GtkWidget *editor = cell->editor;
    if (editor == NULL) {
        return;
    }
    if (revert && cell->edit_original != NULL) {
        gtk_editable_set_text(GTK_EDITABLE(editor), cell->edit_original); /* via the doc */
    }
    if (focus_within(editor)) {
        gtk_widget_grab_focus(cell->box);
    }

    cell->editor = NULL;
    g_clear_pointer(&cell->edit_original, g_free);
    g_signal_handler_disconnect(editor, cell->changed_handler);
    cell->changed_handler = 0;
    /* This can run inside the editor's own signal handlers: keep it alive
     * until they've unwound. */
    g_object_ref(editor);
    gtk_box_remove(GTK_BOX(cell->box), editor);
    g_idle_add_once((GSourceOnceFunc)g_object_unref, editor);

    gtk_widget_set_visible(cell->label, TRUE);
    if (cell->row != NULL) {
        gtk_label_set_text(GTK_LABEL(cell->label),
                           adif_row_get_value(cell->row, cell->info->data_index));
    }
    if (cell->info->table != NULL) {
        adif_document_commit_cell_edit(cell->info->table->doc);
    }
}

static void on_editor_activate(G_GNUC_UNUSED GtkEntry *entry, gpointer user_data) {
    end_edit(user_data, FALSE);
}

static gboolean on_editor_key(G_GNUC_UNUSED GtkEventControllerKey *controller, guint keyval,
                              G_GNUC_UNUSED guint keycode, G_GNUC_UNUSED GdkModifierType state,
                              gpointer user_data) {
    if (keyval == GDK_KEY_Escape) {
        end_edit(user_data, TRUE);
        return TRUE;
    }
    return FALSE;
}

/* Swap the label for an entry and focus it. `text` replaces the
 * value (typing over a cell), or NULL keeps it with the caret at the end. */
static void begin_edit(AdifCell *cell, const char *text) {
    AdifTable *table = cell->info->table;
    if (cell->editor != NULL || cell->row == NULL || table == NULL) {
        return;
    }
    const char *value = adif_row_get_value(cell->row, cell->info->data_index);

    GtkWidget *editor = gtk_entry_new();
    gtk_entry_set_has_frame(GTK_ENTRY(editor), FALSE);
    gtk_widget_add_css_class(editor, "adif-cell-editor");
    gtk_editable_set_width_chars(GTK_EDITABLE(editor), 0); /* the column owns the width */
    gtk_widget_set_hexpand(editor, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(editor), value);
    cell->edit_original = g_strdup(value);
    cell->changed_handler = g_signal_connect(editor, "changed", G_CALLBACK(on_editor_changed),
                                             cell);
    g_signal_connect(editor, "activate", G_CALLBACK(on_editor_activate), cell);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_editor_key), cell);
    gtk_widget_add_controller(editor, keys);

    cell->editor = editor;
    gtk_widget_set_visible(cell->label, FALSE);
    gtk_box_append(GTK_BOX(cell->box), editor);
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(editor));
    if (text != NULL) {
        gtk_editable_set_text(GTK_EDITABLE(editor), text); /* a real edit: goes to the doc */
    }
    gtk_editable_set_position(GTK_EDITABLE(editor), -1);
}

/* Keys on a cell that isn't being edited: Enter / F2 edit it, Backspace /
 * Delete edit it starting empty, and a printable character starts
 * editing with that character. Everything else (arrows, Tab, shortcuts)
 * passes through. */
static gboolean on_cell_key(G_GNUC_UNUSED GtkEventControllerKey *controller, guint keyval,
                            G_GNUC_UNUSED guint keycode, GdkModifierType state,
                            gpointer user_data) {
    AdifCell *cell = user_data;
    if (cell->editor != NULL) {
        return FALSE; /* the editor's keys bubbling up */
    }
    switch (keyval) {
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_ISO_Enter:
        case GDK_KEY_F2:
            begin_edit(cell, NULL);
            return TRUE;
        case GDK_KEY_BackSpace:
        case GDK_KEY_Delete:
        case GDK_KEY_KP_Delete:
            begin_edit(cell, "");
            return TRUE;
        default:
            break;
    }
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) {
        return FALSE;
    }
    gunichar c = gdk_keyval_to_unicode(keyval);
    if (c == 0 || !g_unichar_isprint(c)) {
        return FALSE;
    }
    char utf8[8];
    utf8[g_unichar_to_utf8(c, utf8)] = '\0';
    begin_edit(cell, utf8);
    return TRUE;
}

/* Focus arrived in a cell: move the row highlight to its row (a click
 * that starts editing never reaches the GtkColumnView, and keyboard focus
 * moves don't select), and remember which column holds the cursor, for
 * the toolbar buttons. */
static void on_cell_focus_enter(G_GNUC_UNUSED GtkEventControllerFocus *controller,
                                gpointer user_data) {
    AdifCell *cell = user_data;
    AdifTable *table = cell->info->table;
    if (table == NULL) {
        return;
    }
    table->current_di = cell->info->data_index;

    guint pos = 0;
    if (cell->row != NULL && adif_document_find_row(table->doc, cell->row, &pos)) {
        GtkSelectionModel *model = gtk_column_view_get_model(GTK_COLUMN_VIEW(table->column_view));
        gtk_selection_model_select_item(model, pos, TRUE);
    }
}

static gboolean end_edit_idle_cb(gpointer data) {
    GtkWidget *box = data;
    if (!focus_within(box)) { /* not refocused in the meantime */
        end_edit(adif_cell(box), FALSE);
    }
    return G_SOURCE_REMOVE;
}

/* Focus left the cell (and its editor) altogether: close the edit's undo
 * step now, so a cell edit is one Ctrl+Z, and drop the editor once GTK
 * has finished moving focus - unparenting the widget focus is leaving,
 * mid-move, breaks GTK's own walk up from it. */
static void on_cell_focus_leave(G_GNUC_UNUSED GtkEventControllerFocus *controller,
                                gpointer user_data) {
    AdifCell *cell = user_data;
    if (cell->info->table != NULL) {
        adif_document_commit_cell_edit(cell->info->table->doc);
    }
    if (cell->editor != NULL) {
        g_idle_add_full(G_PRIORITY_DEFAULT, G_SOURCE_FUNC(end_edit_idle_cb),
                        g_object_ref(cell->box), g_object_unref);
    }
}

static void open_cell_menu(AdifCell *cell, GtkWidget *widget, double x, double y);

/* Primary click edits the cell; secondary opens the row/column context
 * menu. Capture phase, so the menu wins over the editor's own text menu
 * while the cell is being edited. */
static void on_cell_pressed(GtkGestureClick *gesture, G_GNUC_UNUSED gint n_press, gdouble x,
                            gdouble y, gpointer user_data) {
    AdifCell *cell = user_data;
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (button == GDK_BUTTON_SECONDARY) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        open_cell_menu(cell, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), x, y);
    } else if (button == GDK_BUTTON_PRIMARY && cell->editor == NULL) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        begin_edit(cell, NULL);
    }
}

static void setup_cell_cb(G_GNUC_UNUSED GtkSignalListItemFactory *factory, GtkListItem *item,
                          gpointer user_data) {
    AdifColumnInfo *info = user_data;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "adif-cell");
    gtk_widget_set_focusable(box, TRUE);
    if (info->table != NULL) {
        /* An editor's height, so the row doesn't jump when one appears. */
        gtk_widget_set_size_request(box, -1, adif_table_cell_height(info->table));
    }

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    if (info->table != NULL) {
        /* Line the text up with where the editor's will be. */
        int inset = adif_table_cell_inset(info->table);
        gtk_widget_set_margin_start(label, inset);
        gtk_widget_set_margin_end(label, inset);
    }
    gtk_box_append(GTK_BOX(box), label);

    AdifCell *cell = g_new0(AdifCell, 1);
    cell->info = g_rc_box_acquire(info);
    cell->box = box;
    cell->label = label;
    g_object_set_data_full(G_OBJECT(box), ADIF_CELL_KEY, cell, cell_free);

    g_ptr_array_add(info->cells, box);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_cell_key), cell);
    gtk_widget_add_controller(box, keys);

    /* Keep the row highlight in step with which cell has focus, and end
     * the edit when focus leaves the cell. */
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_cell_focus_enter), cell);
    g_signal_connect(focus, "leave", G_CALLBACK(on_cell_focus_leave), cell);
    gtk_widget_add_controller(box, focus);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* any button */
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_cell_pressed), cell);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));

    gtk_list_item_set_child(item, box);
}

static void bind_cell_cb(G_GNUC_UNUSED GtkSignalListItemFactory *factory, GtkListItem *item,
                         gpointer user_data) {
    AdifColumnInfo *info = user_data;
    GtkWidget *box = gtk_list_item_get_child(item);
    AdifCell *cell = adif_cell(box);
    cell->row = ADIF_ROW(gtk_list_item_get_item(item)); /* borrowed for the bound lifetime */

    gtk_label_set_text(GTK_LABEL(cell->label), adif_row_get_value(cell->row, info->data_index));
    /* Cells are recycled, so set or clear it on every bind. The flags are
     * fixed at load, so nothing needs to refresh them later. */
    if (adif_row_is_repaired(cell->row, info->data_index)) {
        gtk_widget_add_css_class(box, "adif-repaired");
    } else {
        gtk_widget_remove_css_class(box, "adif-repaired");
    }
    if (info->table != NULL) {
        adif_table_apply_match_class(info->table, box, cell->row, info->data_index);
    }
}

static void unbind_cell_cb(G_GNUC_UNUSED GtkSignalListItemFactory *factory, GtkListItem *item,
                           G_GNUC_UNUSED gpointer user_data) {
    AdifCell *cell = adif_cell(gtk_list_item_get_child(item));
    end_edit(cell, FALSE); /* scrolled away or removed mid-edit: keep what was typed */
    cell->row = NULL;
}

static void teardown_cell_cb(G_GNUC_UNUSED GtkSignalListItemFactory *factory, GtkListItem *item,
                             gpointer user_data) {
    AdifColumnInfo *info = user_data;
    GtkWidget *box = gtk_list_item_get_child(item);
    if (box != NULL) {
        g_ptr_array_remove_fast(info->cells, box);
    }
}

/* ------------------------------------------------------------------- */
/* Context menus                                                        */
/* ------------------------------------------------------------------- */

/* A plain action item carrying a uint32 target. */
static void menu_add_item(GMenu *menu, const char *label, const char *action, guint target) {
    GMenuItem *item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(item, action, g_variant_new_uint32(target));
    g_menu_append_item(menu, item);
    g_object_unref(item);
}

/* An icon button for a "horizontal-buttons" section; `label` becomes the
 * tooltip / accessible name. */
static void menu_add_button(GMenu *menu, const char *label, const char *icon, const char *action,
                            guint target) {
    GMenuItem *item = g_menu_item_new(label, NULL);
    g_menu_item_set_attribute(item, "verb-icon", "s", icon);
    g_menu_item_set_action_and_target_value(item, action, g_variant_new_uint32(target));
    g_menu_append_item(menu, item);
    g_object_unref(item);
}

/* Append `buttons` as a row of icon-only buttons. */
static void menu_append_button_row(GMenu *menu, GMenu *buttons) {
    GMenuItem *section = g_menu_item_new_section(NULL, G_MENU_MODEL(buttons));
    g_menu_item_set_attribute(section, "display-hint", "s", "horizontal-buttons");
    g_menu_append_item(menu, section);
    g_object_unref(section);
}

static void menu_add_column_insert_buttons(GMenu *buttons, guint data_index) {
    menu_add_button(buttons, "Insert column to the left", "adifedit-insert-column-left-symbolic",
                    "win.insert_column_left", data_index);
    menu_add_button(buttons, "Insert column to the right", "adifedit-insert-column-right-symbolic",
                    "win.insert_column_right", data_index);
}

/* Header menu: a column insert-button row, then Fill / Delete Column. */
static GMenuModel *create_header_menu(guint data_index) {
    GMenu *menu = g_menu_new();

    GMenu *buttons = g_menu_new();
    menu_add_column_insert_buttons(buttons, data_index);
    menu_append_button_row(menu, buttons);
    g_object_unref(buttons);

    GMenu *actions = g_menu_new();
    menu_add_item(actions, "Fill Column…", "win.fill_column", data_index);
    menu_add_item(actions, "Delete Column", "win.delete_column", data_index);
    g_menu_append_section(menu, NULL, G_MENU_MODEL(actions));
    g_object_unref(actions);

    return G_MENU_MODEL(menu);
}

/* A cell's menu: one row of insert buttons (row above/below, column
 * left/right), then Fill / Delete. Kept to two sections so the popover
 * stays short enough not to scroll. */
static GMenuModel *create_cell_menu(guint data_index, guint row_index) {
    GMenu *menu = g_menu_new();

    GMenu *buttons = g_menu_new();
    menu_add_button(buttons, "Insert row above", "adifedit-insert-row-above-symbolic",
                    "win.insert_row", row_index);
    menu_add_button(buttons, "Insert row below", "adifedit-insert-row-below-symbolic",
                    "win.insert_row", row_index + 1);
    menu_add_column_insert_buttons(buttons, data_index);
    menu_append_button_row(menu, buttons);
    g_object_unref(buttons);

    GMenu *actions = g_menu_new();
    menu_add_item(actions, "Fill Column…", "win.fill_column", data_index);
    menu_add_item(actions, "Delete Column", "win.delete_column", data_index);
    menu_add_item(actions, "Delete Row", "win.delete_row", row_index);
    g_menu_append_section(menu, NULL, G_MENU_MODEL(actions));
    g_object_unref(actions);

    return G_MENU_MODEL(menu);
}

/* Pop the cell menu up at (x, y) in `widget`, the cell's box. */
static void open_cell_menu(AdifCell *cell, GtkWidget *widget, double x, double y) {
    AdifTable *self = cell->info->table;
    if (self == NULL || cell->row == NULL) {
        return;
    }
    GtkWidget *column_view = self->column_view;

    guint row_index = 0;
    if (!adif_document_find_row(self->doc, cell->row, &row_index)) {
        return;
    }
    guint data_index = cell->info->data_index;

    GtkSelectionModel *selection = gtk_column_view_get_model(GTK_COLUMN_VIEW(column_view));
    gtk_selection_model_select_item(selection, row_index, TRUE);

    GMenuModel *model = create_cell_menu(data_index, row_index);
    gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(self->cell_menu), model);
    g_object_unref(model);

    /* GtkPopoverMenu under-measures a "horizontal-buttons" section by a
     * few pixels and would put the menu in a scroller. Pin the popover to
     * a hair over what its content actually wants. */
    GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(self->cell_menu));
    if (GTK_IS_SCROLLED_WINDOW(content)) {
        GtkWidget *viewport = gtk_widget_get_first_child(content);
        int content_nat = 0;
        if (viewport != NULL) {
            gtk_widget_measure(viewport, GTK_ORIENTATION_VERTICAL, -1, NULL, &content_nat, NULL,
                               NULL);
        }
        gtk_widget_set_size_request(self->cell_menu, -1, content_nat > 0 ? content_nat + 40 : -1);
    }

    graphene_point_t src = { (float)x, (float)y };
    graphene_point_t dst;
    if (gtk_widget_compute_point(widget, column_view, &src, &dst)) {
        GdkRectangle rect = { (int)dst.x, (int)dst.y, 1, 1 };
        gtk_popover_set_pointing_to(GTK_POPOVER(self->cell_menu), &rect);
        /* Open toward whichever half of the view has more room. A popover
         * cannot grow past the window, so aiming it at the roomy side
         * keeps the menu from being squashed into a scroller. */
        int view_h = gtk_widget_get_height(column_view);
        if (view_h > 0) {
            gtk_popover_set_position(GTK_POPOVER(self->cell_menu),
                                     (dst.y > (float)view_h / 2.0f) ? GTK_POS_TOP : GTK_POS_BOTTOM);
        }
    }
    gtk_popover_popup(GTK_POPOVER(self->cell_menu));
}

/* ------------------------------------------------------------------- */
/* Column widget                                                        */
/* ------------------------------------------------------------------- */

GtkColumnViewColumn *adif_column_new(AdifTable *table, const char *fieldname, guint data_index) {
    GtkColumnViewColumn *column = gtk_column_view_column_new(fieldname, NULL);

    AdifColumnInfo *info = g_rc_box_new0(AdifColumnInfo);
    info->table = table;
    info->name = g_strdup(fieldname);
    info->data_index = data_index;
    info->cells = g_ptr_array_new();
    g_object_set_data_full(G_OBJECT(column), ADIF_COLUMN_INFO_KEY, info, column_info_release);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(setup_cell_cb), info);
    g_signal_connect(factory, "bind", G_CALLBACK(bind_cell_cb), info);
    g_signal_connect(factory, "unbind", G_CALLBACK(unbind_cell_cb), NULL);
    g_signal_connect(factory, "teardown", G_CALLBACK(teardown_cell_cb), info);
    gtk_column_view_column_set_factory(column, factory);
    g_object_unref(factory);

    gtk_column_view_column_set_resizable(column, TRUE);

    /* Right-clicking the header offers only the column operations. */
    GMenuModel *header_menu = create_header_menu(data_index);
    gtk_column_view_column_set_header_menu(column, header_menu);
    g_object_unref(header_menu);

    return column;
}
