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
 * The Find / Find & Replace bar. Ctrl+F opens it in find-only mode,
 * Ctrl+H reveals the replace row as well. All the matching and mutation
 * lives in AdifDocument (adif_document_find / _replace / _replace_all);
 * this file is just the widget wiring and the "where is the cursor"
 * bookkeeping. Replace All is a single undo step by construction.
 */

#include "window_priv.h"

/* Read the bar's widgets into a query. `q->query` / `q->replacement`
 * borrow the entry buffers - valid for the synchronous call that follows. */
static void build_query(AppWindow *w, AdifQuery *q) {
    q->query = gtk_editable_get_text(GTK_EDITABLE(w->search_entry));
    q->replacement = gtk_editable_get_text(GTK_EDITABLE(w->replace_entry));
    q->case_sensitive = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->search_case));
    q->whole_field = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->search_whole));

    guint scope = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->search_scope));
    if (scope == 0 || scope == GTK_INVALID_LIST_POSITION) {
        q->scoped = FALSE;
        q->data_index = 0;
    } else {
        q->scoped = TRUE;
        q->data_index = adif_document_column_data_index_at(w->doc, scope - 1);
    }
}

static void set_info(AppWindow *w, const char *text) {
    gtk_label_set_text(GTK_LABEL(w->search_info), text);
}

/* Recompute the match count for `q` and repaint the info label / entry
 * state. A full scan of the log, so only when the query or the data
 * changed - not when merely stepping between matches. */
static void refresh_count(AppWindow *w, const AdifQuery *q) {
    if (q->query[0] == '\0') {
        set_info(w, "");
        gtk_widget_remove_css_class(w->search_entry, "error");
        return;
    }
    guint n = adif_document_count_matches(w->doc, q);
    if (n == 0) {
        set_info(w, "No matches");
        gtk_widget_add_css_class(w->search_entry, "error");
    } else {
        char *text = g_strdup_printf(n == 1 ? "%u match" : "%u matches", n);
        set_info(w, text);
        g_free(text);
        gtk_widget_remove_css_class(w->search_entry, "error");
    }
}

/* Forget the current match: no cursor, no highlight. */
static void clear_cursor(AppWindow *w) {
    g_clear_object(&w->search_cursor_row);
    adif_table_clear_match(w->table);
}

/* Move to `match`: remember it as the cursor and reveal it in the grid. */
static void land_on(AppWindow *w, AdifCellRef match) {
    g_set_object(&w->search_cursor_row, adif_document_get_row(w->doc, match.row));
    w->search_cursor_di = match.data_index;
    adif_table_go_to_match(w->table, match);
}

/* The current match as a cell, if there is one and its row is still in
 * the document. */
static gboolean cursor_cell(AppWindow *w, AdifCellRef *cell) {
    guint pos = 0;
    if (w->search_cursor_row == NULL
        || !adif_document_find_row(w->doc, w->search_cursor_row, &pos)) {
        return FALSE;
    }
    cell->row = pos;
    cell->data_index = w->search_cursor_di;
    return TRUE;
}

/* Where a fresh search should start from: the selected row (if any),
 * column 0, so "find next" walks forward from the user's position. */
static AdifCellRef search_origin(AppWindow *w) {
    AdifCellRef origin = { .row = G_MAXUINT, .data_index = 0 }; /* out of range: the top */
    if (!cursor_cell(w, &origin)) {
        guint sel = adif_table_selected_row(w->table);
        if (sel != GTK_INVALID_LIST_POSITION) {
            origin.row = sel;
        }
    }
    return origin;
}

/* Re-run from the top of the current position (used while typing). */
static void run_search(AppWindow *w) {
    AdifQuery q;
    build_query(w, &q);
    refresh_count(w, &q);

    AdifCellRef match;
    if (q.query[0] != '\0'
        && adif_document_find(w->doc, &q, search_origin(w), FALSE, TRUE, &match)) {
        land_on(w, match);
    } else {
        clear_cursor(w);
    }
}

static void step(AppWindow *w, gboolean backward) {
    AdifQuery q;
    build_query(w, &q);
    if (q.query[0] == '\0') {
        return;
    }
    AdifCellRef match;
    if (adif_document_find(w->doc, &q, search_origin(w), backward, FALSE, &match)) {
        land_on(w, match);
    }
}

/* --- actions ------------------------------------------------------- */

static void open_bar(AppWindow *w, gboolean with_replace) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(w->replace_revealer), with_replace);
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(w->search_bar), TRUE);

    /* Seed the query with the focused cell's text on first open - handy
     * for "find the other rows like this one". */
    const char *seed = gtk_editable_get_text(GTK_EDITABLE(w->search_entry));
    if (seed[0] == '\0') {
        guint di = 0;
        guint pos = adif_table_current_column(w->table);
        guint row = adif_table_selected_row(w->table);
        if (pos != GTK_INVALID_LIST_POSITION && row != GTK_INVALID_LIST_POSITION) {
            di = adif_document_column_data_index_at(w->doc, pos);
            const char *cell = adif_document_get_value(w->doc, row, di);
            if (cell[0] != '\0') {
                gtk_editable_set_text(GTK_EDITABLE(w->search_entry), cell);
            }
        }
    }
    gtk_widget_grab_focus(w->search_entry);
    gtk_editable_select_region(GTK_EDITABLE(w->search_entry), 0, -1);
    run_search(w);
}

void win_action_find(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                     gpointer user_data) {
    open_bar(user_data, FALSE);
}

void win_action_find_replace(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                             gpointer user_data) {
    open_bar(user_data, TRUE);
}

void win_action_find_next(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                          gpointer user_data) {
    step(user_data, FALSE);
}

void win_action_find_prev(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                          gpointer user_data) {
    step(user_data, TRUE);
}

void win_action_replace(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                        gpointer user_data) {
    AppWindow *w = user_data;

    AdifQuery q;
    build_query(w, &q);
    if (q.query[0] == '\0') {
        return;
    }
    AdifCellRef cell;
    if (cursor_cell(w, &cell) && adif_document_replace(w->doc, &q, cell)) {
        refresh_count(w, &q); /* one fewer match */
    }
    step(w, FALSE); /* on to the next match (the replaced cell no longer matches) */
}

void win_action_replace_all(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                            gpointer user_data) {
    AppWindow *w = user_data;

    AdifQuery q;
    build_query(w, &q);
    if (q.query[0] == '\0') {
        return;
    }
    guint n = adif_document_replace_all(w->doc, &q); /* one undo step */
    clear_cursor(w);

    char *text = g_strdup_printf("Replaced %u", n);
    set_info(w, text);
    g_free(text);
}

/* --- widget callbacks -------------------------------------------- */

static void on_search_changed(G_GNUC_UNUSED GtkSearchEntry *entry, gpointer user_data) {
    run_search(user_data);
}

static void on_search_activate(G_GNUC_UNUSED GtkSearchEntry *entry, gpointer user_data) {
    step(user_data, FALSE);
}

static void on_search_stop(G_GNUC_UNUSED GtkSearchEntry *entry, gpointer user_data) {
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(((AppWindow *)user_data)->search_bar), FALSE);
}

static void on_option_toggled(G_GNUC_UNUSED GtkWidget *widget, gpointer user_data) {
    run_search(user_data);
}

static void on_scope_changed(G_GNUC_UNUSED GObject *dropdown, G_GNUC_UNUSED GParamSpec *pspec,
                             gpointer user_data) {
    run_search(user_data);
}

static void on_replace_activate(G_GNUC_UNUSED GtkEntry *entry, gpointer user_data) {
    win_action_replace(NULL, NULL, user_data);
}

/* The bar closed: drop the match highlight and hand focus back. */
static void on_search_mode_changed(GObject *bar, G_GNUC_UNUSED GParamSpec *pspec,
                                   gpointer user_data) {
    AppWindow *w = user_data;
    if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar))) {
        clear_cursor(w);
        gtk_widget_grab_focus(adif_table_get_widget(w->table));
    }
}

/* Keep the column-scope dropdown in step with the schema. */
static void fill_scope(AppWindow *w) {
    GtkStringList *list = gtk_string_list_new(NULL);
    gtk_string_list_append(list, "All columns");
    guint n = adif_document_get_n_columns(w->doc);
    for (guint i = 0; i < n; i++) {
        gtk_string_list_append(list, adif_document_column_name_at(w->doc, i));
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(w->search_scope), G_LIST_MODEL(list));
    g_object_unref(list);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->search_scope), 0);
}

static void on_schema_changed_scope(G_GNUC_UNUSED AdifDocument *doc, gpointer user_data) {
    fill_scope(user_data);
}

/* --- construction ----------------------------------------------- */

static GtkWidget *icon_button(const char *icon, const char *tip, const char *action) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    return b;
}

GtkWidget *app_window_search_bar_new(AppWindow *w) {
    w->search_bar = gtk_search_bar_new();
    gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(w->search_bar), TRUE);

    GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    /* Row 1: find. */
    GtkWidget *find_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    w->search_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(w->search_entry, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(w->search_entry), "Find in cell values");
    g_signal_connect(w->search_entry, "search-changed", G_CALLBACK(on_search_changed), w);
    g_signal_connect(w->search_entry, "activate", G_CALLBACK(on_search_activate), w);
    g_signal_connect(w->search_entry, "next-match", G_CALLBACK(on_search_activate), w);
    g_signal_connect(w->search_entry, "stop-search", G_CALLBACK(on_search_stop), w);
    gtk_box_append(GTK_BOX(find_row), w->search_entry);

    w->search_info = gtk_label_new("");
    gtk_widget_add_css_class(w->search_info, "dim-label");
    gtk_box_append(GTK_BOX(find_row), w->search_info);

    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(nav, "linked");
    gtk_box_append(GTK_BOX(nav), icon_button("go-up-symbolic", "Previous match", "win.find_prev"));
    gtk_box_append(GTK_BOX(nav), icon_button("go-down-symbolic", "Next match", "win.find_next"));
    gtk_box_append(GTK_BOX(find_row), nav);

    w->search_scope = gtk_drop_down_new(NULL, NULL);
    gtk_widget_set_tooltip_text(w->search_scope, "Which column to search");
    fill_scope(w);
    g_signal_connect(w->search_scope, "notify::selected", G_CALLBACK(on_scope_changed), w);
    gtk_box_append(GTK_BOX(find_row), w->search_scope);

    w->search_case = gtk_toggle_button_new_with_label("Match case");
    w->search_whole = gtk_toggle_button_new_with_label("Whole field");
    gtk_widget_set_tooltip_text(w->search_whole, "Match the whole cell value, not a substring");
    g_signal_connect(w->search_case, "toggled", G_CALLBACK(on_option_toggled), w);
    g_signal_connect(w->search_whole, "toggled", G_CALLBACK(on_option_toggled), w);
    gtk_box_append(GTK_BOX(find_row), w->search_case);
    gtk_box_append(GTK_BOX(find_row), w->search_whole);

    gtk_box_append(GTK_BOX(rows), find_row);

    /* Row 2: replace (revealed only in replace mode). */
    w->replace_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(w->replace_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget *replace_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    w->replace_entry = gtk_entry_new();
    gtk_widget_set_hexpand(w->replace_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->replace_entry), "Replace with");
    gtk_entry_set_activates_default(GTK_ENTRY(w->replace_entry), FALSE);
    g_signal_connect(w->replace_entry, "activate", G_CALLBACK(on_replace_activate), w);
    gtk_box_append(GTK_BOX(replace_row), w->replace_entry);

    GtkWidget *replace_btn = gtk_button_new_with_label("Replace");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(replace_btn), "win.replace");
    GtkWidget *replace_all_btn = gtk_button_new_with_label("Replace All");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(replace_all_btn), "win.replace_all");
    gtk_box_append(GTK_BOX(replace_row), replace_btn);
    gtk_box_append(GTK_BOX(replace_row), replace_all_btn);

    gtk_revealer_set_child(GTK_REVEALER(w->replace_revealer), replace_row);
    gtk_box_append(GTK_BOX(rows), w->replace_revealer);

    gtk_search_bar_set_child(GTK_SEARCH_BAR(w->search_bar), rows);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(w->search_bar), GTK_EDITABLE(w->search_entry));
    g_signal_connect(w->search_bar, "notify::search-mode-enabled",
                     G_CALLBACK(on_search_mode_changed), w);
    g_signal_connect(w->doc, "schema-changed", G_CALLBACK(on_schema_changed_scope), w);

    return w->search_bar;
}
